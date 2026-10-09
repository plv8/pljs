#include "postgres.h"

#include "access/genam.h"
#include "catalog/namespace.h"
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
 * @brief Read a standalone module source from the database by path.
 */
static uint8_t *pljs_read_module(size_t *pbuf_len, const char *filename) {
  uint8_t *ret = NULL;
  *pbuf_len = 0;

  ScanKeyData scankey[1];

  ScanKeyInit(&scankey[0], Amodule_path, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(filename));

  Relation table = table_open(get_pljs_module_relid(), AccessShareLock);
  Relation index = index_open(get_pljs_module_index_relid(), AccessShareLock);

  SysScanDesc scan_descriptor =
      systable_beginscan_ordered(table, index, GetActiveSnapshot(), 1, scankey);

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
      text *source = DatumGetTextP(datum_array[Amodule_source - 1]);
      size_t len = VARSIZE(source) - VARHDRSZ;

      ret = (uint8_t *)palloc(len + 1);
      memcpy(ret, VARDATA(source), len);
      ret[len] = 0;

      *pbuf_len = len;
      break;
    }

    tuple = systable_getnext_ordered(scan_descriptor, ForwardScanDirection);
  }

  systable_endscan_ordered(scan_descriptor);
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
  ScanKeyInit(&scankey[0], Amodule_package_name, BTEqualStrategyNumber,
              F_TEXTEQ, CStringGetTextDatum(package_name));
  ScanKeyInit(&scankey[1], Amodule_package_version, BTEqualStrategyNumber,
              F_TEXTEQ, CStringGetTextDatum(package_version));
  ScanKeyInit(&scankey[2], Amodule_path, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(module_path));

  Relation table = table_open(get_pljs_module_relid(), AccessShareLock);
  Relation index =
      index_open(get_pljs_package_module_index_relid(), AccessShareLock);

  SysScanDesc scan =
      systable_beginscan_ordered(table, index, GetActiveSnapshot(), 3, scankey);

  HeapTuple tuple = systable_getnext_ordered(scan, ForwardScanDirection);

  if (HeapTupleIsValid(tuple)) {
    Datum datum_array[Amodule_length];
    bool is_null_array[Amodule_length];

    heap_deform_tuple(tuple, RelationGetDescr(table), datum_array,
                      is_null_array);

    text *source = DatumGetTextP(datum_array[Amodule_source - 1]);
    size_t len = VARSIZE(source) - VARHDRSZ;

    ret = (uint8_t *)palloc(len + 1);
    memcpy(ret, VARDATA(source), len);
    ret[len] = 0;

    *pbuf_len = len;
  }

  systable_endscan_ordered(scan);
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
  ScanKeyInit(&scankey[0], Apackage_name, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(pkg_name));

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

  ScanKeyData scankey[2];
  ScanKeyInit(&scankey[0], Apackage_name, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(pkg_name));
  ScanKeyInit(&scankey[1], Apackage_version, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(pkg_version));

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
  Datum deps_datum;
  bool deps_is_null;

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
  if (!found || found->type != jbvString) {
    return NULL;
  }

  return pnstrdup(found->val.string.val, found->val.string.len);
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
  char *slash = strchr(pkg_name, '/');
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
 * @brief Import a JavaScript module from the database.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name The name of the module to load.
 * @returns JSValue A copy of the compiled module.
 */
JSValue pljs_module_import(JSContext *ctx, const char *module_name) {
  if (current_module_depth >= configuration.max_module_depth) {
    JS_ThrowInternalError(
        ctx, "module import depth exceeded (%d): possible circular dependency",
        configuration.max_module_depth);
    return JS_EXCEPTION;
  }

  pljs_resolved_module resolved = pljs_resolve_module(module_name);

  if (!resolved.source) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    return JS_EXCEPTION;
  }

  // Save and set package context for nested imports.
  const char *old_pkg_name = current_package_name;
  const char *old_pkg_version = current_package_version;
  current_package_name = resolved.package_name;
  current_package_version = resolved.package_version;
  current_module_depth++;

  char *buf = (char *)resolved.source;
  size_t buf_len = resolved.source_len;

  JSValue res = JS_Eval(ctx, buf, buf_len, module_name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  pfree(buf);

  if (JS_IsException(res)) {
    JSValue ex = JS_GetException(ctx);
    const char *err = JS_ToCString(ctx, ex);
    elog(WARNING, "could not compile module '%s': %s", module_name, err);
    JS_FreeCString(ctx, err);
    JS_FreeValue(ctx, ex);

    current_package_name = old_pkg_name;
    current_package_version = old_pkg_version;
    current_module_depth--;
    return JS_EXCEPTION;
  }

  if (JS_ResolveModule(ctx, res) < 0) {
    JS_FreeValue(ctx, res);
    elog(WARNING, "unable to resolve module %s", module_name);

    current_package_name = old_pkg_name;
    current_package_version = old_pkg_version;
    current_module_depth--;
    return JS_EXCEPTION;
  }

  JSValue mod_res = JS_EvalFunction(ctx, JS_DupValue(ctx, res));
  mod_res = js_std_await(ctx, mod_res);

  if (JS_IsException(mod_res)) {
    JS_FreeValue(ctx, res);
    js_std_dump_error(ctx);

    current_package_name = old_pkg_name;
    current_package_version = old_pkg_version;
    current_module_depth--;
    return JS_EXCEPTION;
  }
  JS_FreeValue(ctx, mod_res);

  JSModuleDef *m = JS_VALUE_GET_PTR(res);
  JS_FreeValue(ctx, res);
  JSValue ns = JS_GetModuleNamespace(ctx, m);

  current_package_name = old_pkg_name;
  current_package_version = old_pkg_version;
  current_module_depth--;

  return ns;
}

/**
 * @brief Require a JavaScript commonjs module from the database.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name The name of the module to load.
 * @returns JSValue A copy of the compiled module.
 */
JSValue pljs_module_require(JSContext *ctx, const char *module_name) {
  if (current_module_depth >= configuration.max_module_depth) {
    JS_ThrowInternalError(
        ctx, "module require depth exceeded (%d): possible circular dependency",
        configuration.max_module_depth);
    return JS_EXCEPTION;
  }

  pljs_resolved_module resolved = pljs_resolve_module(module_name);

  if (!resolved.source) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    return JS_EXCEPTION;
  }

  // Save and set package context for nested requires.
  const char *old_pkg_name = current_package_name;
  const char *old_pkg_version = current_package_version;
  current_package_name = resolved.package_name;
  current_package_version = resolved.package_version;
  current_module_depth++;

  JSValue global = JS_GetGlobalObject(ctx);

  JSValue module_obj = JS_NewObject(ctx);
  JSValue exports_obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, module_obj, "exports", JS_DupValue(ctx, exports_obj));
  JS_SetPropertyStr(ctx, global, "module", JS_DupValue(ctx, module_obj));
  JS_SetPropertyStr(ctx, global, "exports", JS_DupValue(ctx, exports_obj));
  JS_FreeValue(ctx, exports_obj);
  JS_FreeValue(ctx, module_obj);

  char *buf = (char *)resolved.source;
  size_t buf_len = resolved.source_len;

  JSValue res =
      JS_Eval(ctx, buf, buf_len, module_name, JS_EVAL_FLAG_COMPILE_ONLY);
  pfree(buf);

  if (JS_IsException(res)) {
    JSValue ex = JS_GetException(ctx);
    const char *err = JS_ToCString(ctx, ex);
    elog(WARNING, "could not compile commonjs module '%s': %s", module_name,
         err);
    JS_FreeCString(ctx, err);
    JS_FreeValue(ctx, ex);
    JS_FreeValue(ctx, global);

    current_package_name = old_pkg_name;
    current_package_version = old_pkg_version;
    current_module_depth--;
    return JS_EXCEPTION;
  }

  JSValue eval_res = JS_EvalFunction(ctx, res);

  if (JS_IsException(eval_res)) {
    JSValue ex = JS_GetException(ctx);
    const char *err = JS_ToCString(ctx, ex);
    elog(WARNING, "could not execute commonjs module '%s': %s", module_name,
         err);
    JS_FreeCString(ctx, err);
    JS_FreeValue(ctx, ex);
    JS_FreeValue(ctx, global);

    current_package_name = old_pkg_name;
    current_package_version = old_pkg_version;
    current_module_depth--;
    return JS_EXCEPTION;
  }

  JS_FreeValue(ctx, eval_res);

  JSValue result = JS_UNDEFINED;
  JSValue mod = JS_GetPropertyStr(ctx, global, "module");

  if (!JS_IsUndefined(mod) && !JS_IsNull(mod)) {
    result = JS_GetPropertyStr(ctx, mod, "exports");
  }

  JS_FreeValue(ctx, mod);

  if (JS_IsUndefined(result) || JS_IsNull(result)) {
    JS_FreeValue(ctx, result);
    result = JS_GetPropertyStr(ctx, global, "exports");
  }

  JS_FreeValue(ctx, global);

  // Restore previous package context.
  current_package_name = old_pkg_name;
  current_package_version = old_pkg_version;
  current_module_depth--;

  return result;
}
