#include "postgres.h"

#include "access/genam.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "pljs.h"
#include "semver.h"

// Column positions in pljs.modules (1-based).
enum module_data_access {
  Amodule_path = 1,
  Amodule_source,
  Amodule_package_name,
  Amodule_package_version,
  Amodule_created_at,
  Amodule_updated_at,
  Amodule_length = 6
};

// Column positions in pljs.packages (1-based).
enum package_data_access {
  Apackage_name = 1,
  Apackage_version,
  Apackage_entry_point,
  Apackage_dependencies,
  Apackage_created_at,
  Apackage_updated_at,
  Apackage_length = 6
};

static Oid get_pljs_schema_oid(void) {
  return get_namespace_oid("pljs", false);
}

static Oid get_pljs_module_relid(void) {
  return get_relname_relid("modules", get_pljs_schema_oid());
}

static Oid get_pljs_module_index_relid(void) {
  return get_relname_relid("pljs_modules_path", get_pljs_schema_oid());
}

static Oid get_pljs_package_module_index_relid(void) {
  return get_relname_relid("pljs_modules_pkg", get_pljs_schema_oid());
}

static Oid get_pljs_package_relid(void) {
  return get_relname_relid("packages", get_pljs_schema_oid());
}

static Oid get_pljs_package_name_index_relid(void) {
  return get_relname_relid("pljs_packages_name", get_pljs_schema_oid());
}

static Oid get_pljs_package_pkey_relid(void) {
  return get_relname_relid("packages_pkey", get_pljs_schema_oid());
}

/**
 * @brief Initialize a scan key that matches a TEXT column against a C string.
 *
 * ScanKeyInit() stamps the key with the C collation, which is right for
 * catalog columns but not for these: their indexes are ordered by the
 * database's default collation, and a key compared under a different one
 * makes the btree search land in the wrong place and miss rows that exist.
 */
static void pljs_text_key(ScanKey key, AttrNumber attno, const char *value) {
  ScanKeyInit(key, attno, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(value));
  key->sk_collation = DEFAULT_COLLATION_OID;
}

/**
 * @brief Copy a module's TEXT source out of a catalog tuple, as UTF-8.
 *
 * QuickJS reads UTF-8, so source stored in another database encoding is
 * converted the way a function's source is.  The result is NUL-terminated.
 */
static uint8_t *pljs_module_source(Datum source_datum, size_t *len_out) {
  text *source = DatumGetTextPP(source_datum);
  size_t len = VARSIZE_ANY_EXHDR(source);
  char *utf8 = pljs_server_to_utf8(VARDATA_ANY(source), len);
  uint8_t *ret;

  if (utf8 == VARDATA_ANY(source)) {
    // Not converted, and not NUL-terminated either: copy it.
    ret = (uint8_t *)palloc(len + 1);
    memcpy(ret, utf8, len);
    ret[len] = 0;
  } else {
    ret = (uint8_t *)utf8;
    len = strlen(utf8);
  }

  *len_out = len;

  return ret;
}

/**
 * @brief Snapshot for a catalog scan, pushing one if none is active.
 *
 * A module can be loaded where no snapshot is active yet (for example from
 * pljs.start_proc), and a scan without one would crash.  Pair with
 * pljs_scan_snapshot_release().
 */
static Snapshot pljs_scan_snapshot(bool *pushed) {
  *pushed = false;

  if (!ActiveSnapshotSet()) {
    PushActiveSnapshot(GetTransactionSnapshot());
    *pushed = true;
  }

  return GetActiveSnapshot();
}

static void pljs_scan_snapshot_release(bool pushed) {
  if (pushed) {
    PopActiveSnapshot();
  }
}

/**
 * @brief Read a standalone module source from the database by path.
 */
static uint8_t *pljs_read_module(size_t *pbuf_len, const char *filename) {
  uint8_t *ret = NULL;
  *pbuf_len = 0;

  ScanKeyData scankey[1];

  pljs_text_key(&scankey[0], Amodule_path, filename);

  Relation table = table_open(get_pljs_module_relid(), AccessShareLock);
  Relation index = index_open(get_pljs_module_index_relid(), AccessShareLock);

  bool pushed;
  Snapshot snapshot = pljs_scan_snapshot(&pushed);

  SysScanDesc scan_descriptor =
      systable_beginscan_ordered(table, index, snapshot, 1, scankey);

  HeapTuple tuple =
      systable_getnext_ordered(scan_descriptor, ForwardScanDirection);

  // Walk results to find one with package_name IS NULL.
  while (HeapTupleIsValid(tuple)) {
    Datum datum_array[Amodule_length];
    bool is_null_array[Amodule_length];

    TupleDesc tuple_desc = RelationGetDescr(table);
    heap_deform_tuple(tuple, tuple_desc, datum_array, is_null_array);

    // Only match standalone modules (package_name IS NULL).
    if (is_null_array[Amodule_package_name - 1]) {
      if (!is_null_array[Amodule_source - 1]) {
        ret = pljs_module_source(datum_array[Amodule_source - 1], pbuf_len);
      }
      break;
    }

    tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
  }

  systable_endscan_ordered(scan_descriptor);
  pljs_scan_snapshot_release(pushed);
  index_close(index, AccessShareLock);
  table_close(table, AccessShareLock);

  return ret;
}

/**
 * @brief Read a package module source from the database.
 */
static uint8_t *pljs_read_package_module(size_t *pbuf_len,
                                         const char *package_name,
                                         const char *package_version,
                                         const char *module_path) {
  uint8_t *ret = NULL;
  *pbuf_len = 0;

  ScanKeyData scankey[3];

  // Attribute numbers are table column positions, not index positions.
  pljs_text_key(&scankey[0], Amodule_package_name, package_name);
  pljs_text_key(&scankey[1], Amodule_package_version, package_version);
  pljs_text_key(&scankey[2], Amodule_path, module_path);

  Relation table = table_open(get_pljs_module_relid(), AccessShareLock);
  Relation index =
      index_open(get_pljs_package_module_index_relid(), AccessShareLock);

  bool pushed;
  Snapshot snapshot = pljs_scan_snapshot(&pushed);

  SysScanDesc scan =
      systable_beginscan_ordered(table, index, snapshot, 3, scankey);

  HeapTuple tuple = systable_getnext_ordered(scan, ForwardScanDirection);

  if (HeapTupleIsValid(tuple)) {
    Datum datum_array[Amodule_length];
    bool is_null_array[Amodule_length];

    heap_deform_tuple(tuple, RelationGetDescr(table), datum_array,
                      is_null_array);

    if (!is_null_array[Amodule_source - 1]) {
      ret = pljs_module_source(datum_array[Amodule_source - 1], pbuf_len);
    }
  }

  systable_endscan_ordered(scan);
  pljs_scan_snapshot_release(pushed);
  index_close(index, AccessShareLock);
  table_close(table, AccessShareLock);

  return ret;
}

/**
 * @brief Find the best matching version of a package.
 *
 * Scans pljs.packages by name, filters by semver range, returns highest match.
 * If range_str is NULL, returns the latest version.
 */
static char *pljs_resolve_package_version(const char *pkg_name,
                                          const char *range_str) {
  char *best_version = NULL;
  semver best_semver = {0, 0, 0};
  bool found = false;

  semver_range range;
  if (range_str) {
    if (!semver_range_parse(range_str, &range)) {
      return NULL;
    }
  }

  ScanKeyData scankey[1];
  pljs_text_key(&scankey[0], Apackage_name, pkg_name);

  Relation table = table_open(get_pljs_package_relid(), AccessShareLock);
  Relation index =
      index_open(get_pljs_package_name_index_relid(), AccessShareLock);

  SysScanDesc scan =
      systable_beginscan_ordered(table, index, GetActiveSnapshot(), 1, scankey);

  HeapTuple tuple;
  while ((tuple = systable_getnext_ordered(scan, ForwardScanDirection)) !=
         NULL) {
    Datum datum_array[Apackage_length];
    bool is_null_array[Apackage_length];

    heap_deform_tuple(tuple, RelationGetDescr(table), datum_array,
                      is_null_array);

    if (is_null_array[Apackage_version - 1]) {
      continue;
    }

    char *ver_str =
        TextDatumGetCString(datum_array[Apackage_version - 1]);
    semver ver;
    if (!semver_parse(ver_str, &ver)) {
      pfree(ver_str);
      continue;
    }

    bool matches = range_str ? semver_satisfies(&ver, &range) : true;

    if (matches && (!found || semver_compare(&ver, &best_semver) > 0)) {
      if (best_version) {
        pfree(best_version);
      }
      best_version = pstrdup(ver_str);
      best_semver = ver;
      found = true;
    }

    pfree(ver_str);
  }

  systable_endscan_ordered(scan);
  index_close(index, AccessShareLock);
  table_close(table, AccessShareLock);

  return best_version;
}

/**
 * @brief Look up entry_point and optionally dependencies for a package.
 *
 * Scans pljs.packages by (name, version) using the primary key index.
 * Returns the entry_point string, or NULL if not found.
 * If deps_out is non-NULL and the package has a dependencies JSONB column,
 * the Datum is written there for the caller to extract dependency ranges.
 */
static char *pljs_get_package_info(const char *pkg_name,
                                   const char *pkg_version,
                                   Datum *deps_out, bool *deps_is_null) {
  char *entry_point = NULL;

  // Defined even when the package is not found, so callers can read them.
  if (deps_is_null) {
    *deps_is_null = true;
  }

  ScanKeyData scankey[2];
  pljs_text_key(&scankey[0], Apackage_name, pkg_name);
  pljs_text_key(&scankey[1], Apackage_version, pkg_version);

  Relation table = table_open(get_pljs_package_relid(), AccessShareLock);
  Relation index =
      index_open(get_pljs_package_pkey_relid(), AccessShareLock);

  SysScanDesc scan =
      systable_beginscan_ordered(table, index, GetActiveSnapshot(), 2, scankey);

  HeapTuple tuple = systable_getnext_ordered(scan, ForwardScanDirection);

  if (HeapTupleIsValid(tuple)) {
    Datum datum_array[Apackage_length];
    bool is_null_array[Apackage_length];

    heap_deform_tuple(tuple, RelationGetDescr(table), datum_array,
                      is_null_array);

    if (!is_null_array[Apackage_entry_point - 1]) {
      entry_point =
          pstrdup(TextDatumGetCString(datum_array[Apackage_entry_point - 1]));
    }

    if (deps_out && deps_is_null) {
      *deps_is_null = is_null_array[Apackage_dependencies - 1];
      if (!*deps_is_null) {
        // Copy the JSONB datum so it survives after table_close.
        *deps_out = PointerGetDatum(
            PG_DETOAST_DATUM_COPY(datum_array[Apackage_dependencies - 1]));
      }
    }
  }

  systable_endscan_ordered(scan);
  index_close(index, AccessShareLock);
  table_close(table, AccessShareLock);

  return entry_point;
}

/**
 * @brief Look up a dependency version range from a package's dependencies JSONB.
 */
static char *pljs_get_package_dependency_range(const char *pkg_name,
                                               const char *pkg_version,
                                               const char *dep_name) {
  Datum deps_datum = (Datum)0;
  bool deps_is_null = true;

  char *ep = pljs_get_package_info(pkg_name, pkg_version, &deps_datum,
                                    &deps_is_null);
  if (ep) {
    pfree(ep);
  }

  if (deps_is_null) {
    return NULL;
  }

  // Look up dep_name directly in the JSONB container.
  Jsonb *jb = DatumGetJsonbP(deps_datum);
  JsonbValue kval;
  kval.type = jbvString;
  kval.val.string.val = (char *)dep_name;
  kval.val.string.len = strlen(dep_name);

  JsonbValue *found = findJsonbValueFromContainer(&jb->root, JB_FOBJECT, &kval);
  char *range = NULL;

  if (found && found->type == jbvString) {
    range = pnstrdup(found->val.string.val, found->val.string.len);
  }

  pfree(jb);

  return range;
}

typedef struct pljs_resolved_module {
  uint8_t *source;
  size_t source_len;
  char *package_name;
  char *package_version;
  char *module_path;
} pljs_resolved_module;

// Current package context for nested require/import resolution.
// Set before evaluating a package module, restored after.
static const char *current_package_name = NULL;
static const char *current_package_version = NULL;

// Depth of the current nested import/require chain. Guarded by
// pljs.max_module_depth to prevent unbounded recursion via circular deps.
static int current_module_depth = 0;

static pljs_resolved_module pljs_resolve_module(const char *specifier) {
  pljs_resolved_module result = {0};

  // Parse "PackageName/submodule" into package name and module path.
  char *pkg_name = pstrdup(specifier);
  char *sub_path = NULL;
  // A scoped name ("@scope/pkg") keeps its first slash: the package name is
  // "@scope/pkg", and a sub-module path may follow it.
  char *slash = strchr(pkg_name + (pkg_name[0] == '@' ? 1 : 0), '/');
  if (slash && pkg_name[0] == '@') {
    slash = strchr(slash + 1, '/');
  }
  if (slash) {
    *slash = '\0';
    sub_path = slash + 1;
  }

  // Step 1: Try standalone module (exact path match, no package).
  result.source = pljs_read_module(&result.source_len, specifier);
  if (result.source) {
    result.module_path = pstrdup(specifier);
    pfree(pkg_name);
    return result;
  }

  // Step 2: Resolve as a package.
  // Check if we're inside a package context (nested require).
  char *range_str = NULL;

  if (current_package_name && current_package_version) {
    // Look up the dependency constraint from the calling package.
    range_str = pljs_get_package_dependency_range(
        current_package_name, current_package_version, pkg_name);
  }

  // Resolve the version (latest if no range, best match if range).
  char *version = pljs_resolve_package_version(pkg_name, range_str);
  if (range_str) {
    pfree(range_str);
  }

  if (!version) {
    pfree(pkg_name);
    return result; // source is NULL — caller handles the error.
  }

  // Determine the module path within the package.
  char *module_path;
  if (sub_path) {
    module_path = pstrdup(sub_path);
  } else {
    module_path = pljs_get_package_info(pkg_name, version, NULL, NULL);
    if (!module_path) {
      pfree(pkg_name);
      pfree(version);
      return result;
    }
  }

  // Load the module source from the package.
  result.source =
      pljs_read_package_module(&result.source_len, pkg_name, version,
                               module_path);
  if (result.source) {
    result.package_name = pkg_name;
    result.package_version = version;
    result.module_path = module_path;
  } else {
    pfree(pkg_name);
    pfree(version);
    pfree(module_path);
  }

  return result;
}

/**
 * @brief Package and depth state saved around a nested import or require.
 */
typedef struct pljs_module_scope {
  const char *package_name;
  const char *package_version;
} pljs_module_scope;

static pljs_module_scope pljs_module_scope_enter(
    const pljs_resolved_module *resolved) {
  pljs_module_scope saved = {current_package_name, current_package_version};

  current_package_name = resolved->package_name;
  current_package_version = resolved->package_version;
  current_module_depth++;

  return saved;
}

static void pljs_module_scope_leave(pljs_module_scope saved) {
  current_package_name = saved.package_name;
  current_package_version = saved.package_version;
  current_module_depth--;
}

/**
 * @brief Import a JavaScript module from the database.
 *
 * Any failure leaves the JavaScript exception pending, so the caller sees the
 * real error (a syntax error in the module, or whatever it threw).
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name The name of the module to load.
 * @returns JSValue The module namespace, or JS_EXCEPTION.
 */
JSValue pljs_module_import(JSContext *ctx, const char *module_name) {
  if (current_module_depth >= configuration.max_module_depth) {
    return JS_ThrowInternalError(
        ctx, "module import depth exceeded (%d): possible circular dependency",
        configuration.max_module_depth);
  }

  pljs_resolved_module resolved = pljs_resolve_module(module_name);

  if (!resolved.source) {
    return JS_ThrowReferenceError(ctx, "could not load module '%s'",
                                  module_name);
  }

  pljs_module_scope saved = pljs_module_scope_enter(&resolved);
  JSValue ret = JS_EXCEPTION;

  JSValue res = JS_Eval(ctx, (char *)resolved.source, resolved.source_len,
                        module_name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  pfree(resolved.source);

  if (JS_IsException(res)) {
    goto done;
  }

  if (JS_ResolveModule(ctx, res) < 0) {
    JS_FreeValue(ctx, res);
    goto done;
  }

  JSValue mod_res = JS_EvalFunction(ctx, JS_DupValue(ctx, res));
  mod_res = js_std_await(ctx, mod_res);

  if (JS_IsException(mod_res)) {
    JS_FreeValue(ctx, res);
    goto done;
  }
  JS_FreeValue(ctx, mod_res);

  JSModuleDef *m = JS_VALUE_GET_PTR(res);
  JS_FreeValue(ctx, res);
  ret = JS_GetModuleNamespace(ctx, m);

done:
  pljs_module_scope_leave(saved);

  return ret;
}

/**
 * @brief Require a JavaScript commonjs module from the database.
 *
 * The module runs inside a function that is given its own `exports` and
 * `module`, so nothing leaks into the global object and a nested require
 * cannot overwrite the module that is requiring it.  Any failure leaves the
 * JavaScript exception pending.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name The name of the module to load.
 * @returns JSValue The module's exports, or JS_EXCEPTION.
 */
JSValue pljs_module_require(JSContext *ctx, const char *module_name) {
  if (current_module_depth >= configuration.max_module_depth) {
    return JS_ThrowInternalError(
        ctx, "module require depth exceeded (%d): possible circular dependency",
        configuration.max_module_depth);
  }

  pljs_resolved_module resolved = pljs_resolve_module(module_name);

  if (!resolved.source) {
    return JS_ThrowReferenceError(ctx, "could not load module '%s'",
                                  module_name);
  }

  pljs_module_scope saved = pljs_module_scope_enter(&resolved);
  JSValue ret = JS_EXCEPTION;

  /*
   * The wrapper's first line is the source's first line, so error positions
   * still point into the module.  The trailing newline guards against a
   * final "//" comment swallowing the closing brace.
   */
  StringInfoData wrapped;
  initStringInfo(&wrapped);
  appendStringInfoString(&wrapped, "(function (exports, module) {");
  appendBinaryStringInfo(&wrapped, (char *)resolved.source,
                         (int)resolved.source_len);
  appendStringInfoString(&wrapped, "\n})");
  pfree(resolved.source);

  JSValue fn = JS_Eval(ctx, wrapped.data, wrapped.len, module_name, 0);
  pfree(wrapped.data);

  if (JS_IsException(fn)) {
    goto done;
  }

  JSValue module_obj = JS_NewObject(ctx);
  JSValue exports_obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, module_obj, "exports", JS_DupValue(ctx, exports_obj));

  JSValue args[2] = {exports_obj, module_obj};
  JSValue call_res = JS_Call(ctx, fn, exports_obj, 2, args);
  JS_FreeValue(ctx, fn);

  if (JS_IsException(call_res)) {
    JS_FreeValue(ctx, exports_obj);
    JS_FreeValue(ctx, module_obj);
    goto done;
  }
  JS_FreeValue(ctx, call_res);

  // module.exports may have been reassigned; that wins over `exports`.
  ret = JS_GetPropertyStr(ctx, module_obj, "exports");

  JS_FreeValue(ctx, exports_obj);
  JS_FreeValue(ctx, module_obj);

done:
  pljs_module_scope_leave(saved);

  return ret;
}
