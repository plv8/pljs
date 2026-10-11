#include "postgres.h"

#include <ctype.h>

#include "access/genam.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "pljs.h"
#include "semver.h"

/*
 * Modules and packages live in pljs.modules and pljs.packages.
 *
 * They are loaded through QuickJS's own module system: pljs_module_normalize()
 * turns whatever a module asked for into the canonical name of one stored
 * module, and pljs_module_loader() compiles it.  QuickJS keeps a module under
 * its canonical name, so a module is evaluated once per context and every
 * importer shares that instance.
 *
 * The canonical name carries a generation number.  Anything that changes
 * pljs.modules or pljs.packages (see pljs_modules_changed()) bumps the
 * generation in every backend, so the next import asks for modules under new
 * names and QuickJS loads them afresh.  That is the whole cache invalidation:
 * a context cannot unload a module, so the old instances stay until the
 * context goes (pljs_reset(), or the backend ending), but nothing refers to
 * them by name any more.
 */

// Column positions in pljs.modules (1-based).
enum module_data_access {
  Amodule_path = 1,
  Amodule_source,
  Amodule_package_name,
  Amodule_package_version,
  Amodule_created_at,
  Amodule_updated_at,
  Amodule_sloppy,
  Amodule_bytecode,
  Amodule_requires,
  Amodule_is_trusted,
  Amodule_length = 10
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

PGDLLEXPORT Datum pljs_modules_changed(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pljs_modules_changed);

/*
 * ---------- Generation, and the catalog OIDs it also invalidates ----------
 */

/*
 * Part of every canonical name, and what everything loaded is kept against: it
 * moves on, and all of it is dropped, when the module tables change and when a
 * module's evaluation fails.  QuickJS keeps a module whose evaluation threw,
 * and throws the same error at every later import of it, so loading it again
 * has to be under a new name -- and so does everything else, or a module that
 * was fine would be loaded twice, with state of its own.
 */
static uint64 module_generation = 1;

// Bumped only when the module tables change; see pljs_module_load_once().
static uint64 module_table_changes = 0;

// Whether a module could not be found or compiled, as against evaluated; set
// by the normalizer and loader, to tell the two kinds of failure apart.
static bool module_load_failed = false;

// Whether this transaction read modules under its own snapshot; see
// pljs_scan_snapshot().
static bool modules_read_in_snapshot_xact = false;

/*
 * Set while a function's source is only being checked for syntax.  QuickJS
 * loads a module's imports as it compiles it, which a check must not do: the
 * modules may not exist yet, and a check should not read the tables at all.
 * Imports then resolve to themselves, and to empty modules.
 */
static bool syntax_check_only = false;

// The tables and indexes the module readers scan, found once.
static Oid modules_relid = InvalidOid;
static Oid packages_relid = InvalidOid;
static Oid modules_path_index = InvalidOid;
static Oid modules_package_index = InvalidOid;
static Oid packages_name_index = InvalidOid;
static Oid packages_pkey_index = InvalidOid;

/**
 * @brief Relcache callback: drop everything derived from the module tables.
 *
 * Runs in the backend that sees the invalidation -- the one that changed the
 * tables at the end of the statement, and any other at its next transaction or
 * lock -- and must not touch the catalogs.
 */
static void pljs_modules_inval_cb(Datum arg, Oid relid) {
  if (relid == InvalidOid || relid == modules_relid || relid == packages_relid) {
    module_generation++;
    module_table_changes++;

    modules_relid = InvalidOid;
    packages_relid = InvalidOid;
    modules_path_index = InvalidOid;
    modules_package_index = InvalidOid;
    packages_name_index = InvalidOid;
    packages_pkey_index = InvalidOid;
  }
}

/**
 * @brief Forgets what a transaction loaded under its own snapshot.
 *
 * A repeatable-read transaction reads the modules as they were when it began,
 * which may be older than a change this backend has already been told about.
 * What it loaded is then not the current modules, and nothing will say so
 * later, so it is dropped when the transaction ends.
 */
static void pljs_modules_xact_cb(XactEvent event, void *arg) {
  if ((event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT) &&
      modules_read_in_snapshot_xact) {
    module_generation++;
    modules_read_in_snapshot_xact = false;
  }
}

/**
 * @brief Registers the invalidation callbacks; called from _PG_init().
 */
void pljs_modules_init(void) {
  CacheRegisterRelcacheCallback(pljs_modules_inval_cb, (Datum)0);
  RegisterXactCallback(pljs_modules_xact_cb, NULL);
}

/**
 * @brief Trigger function that invalidates cached modules.
 *
 * Attached, for every statement that changes them, to pljs.modules and
 * pljs.packages.  Invalidating the table's relcache entry reaches every
 * backend, at commit, and this one now -- and again if the transaction rolls
 * back, which is what puts a rolled-back change's modules back.
 */
Datum pljs_modules_changed(PG_FUNCTION_ARGS) {
  if (!CALLED_AS_TRIGGER(fcinfo)) {
    elog(ERROR, "pljs_modules_changed: not called by trigger manager");
  }

  CacheInvalidateRelcache(((TriggerData *)fcinfo->context)->tg_relation);

  return PointerGetDatum(NULL);
}

/**
 * @brief Finds the module tables and their indexes, if they are not known.
 *
 * @returns @c bool - false when the tables are not there
 */
static bool pljs_module_relids(void) {
  if (OidIsValid(modules_relid) && OidIsValid(packages_relid) &&
      OidIsValid(modules_path_index) && OidIsValid(modules_package_index) &&
      OidIsValid(packages_name_index) && OidIsValid(packages_pkey_index)) {
    return true;
  }

  Oid nsp = get_namespace_oid("pljs", true);

  if (!OidIsValid(nsp)) {
    return false;
  }

  modules_relid = get_relname_relid("modules", nsp);
  packages_relid = get_relname_relid("packages", nsp);
  modules_path_index = get_relname_relid("pljs_modules_path", nsp);
  modules_package_index = get_relname_relid("pljs_modules_pkg", nsp);
  packages_name_index = get_relname_relid("pljs_packages_name", nsp);
  packages_pkey_index = get_relname_relid("packages_pkey", nsp);

  return OidIsValid(modules_relid) && OidIsValid(packages_relid) &&
         OidIsValid(modules_path_index) && OidIsValid(modules_package_index) &&
         OidIsValid(packages_name_index) && OidIsValid(packages_pkey_index);
}

/*
 * ---------- Reading the tables ----------
 */

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
  pljs_encoding_init();
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
 * @brief Snapshot for a catalog scan.
 *
 * The current statement's snapshot may be older than a change to the modules
 * that this backend has just been told about, and what is read under it would
 * be loaded as the new generation's.  So a read-committed transaction reads
 * with a snapshot of its own, as current as can be.  A repeatable-read or
 * serializable one must keep to its snapshot, and its loaded modules are
 * dropped when it ends; see pljs_modules_xact_cb().
 *
 * Also pushes one when none is active (a module can be loaded from
 * pljs.start_proc, say), which a scan could not do without.  Pair with
 * pljs_scan_snapshot_release().
 */
static Snapshot pljs_scan_snapshot(bool *pushed) {
  *pushed = false;

  if (!ActiveSnapshotSet()) {
    PushActiveSnapshot(GetTransactionSnapshot());
    *pushed = true;
  } else if (IsolationUsesXactSnapshot()) {
    modules_read_in_snapshot_xact = true;
  } else {
    PushActiveSnapshot(GetLatestSnapshot());
    *pushed = true;
  }

  if (IsolationUsesXactSnapshot()) {
    modules_read_in_snapshot_xact = true;
  }

  return GetActiveSnapshot();
}

static void pljs_scan_snapshot_release(bool pushed) {
  if (pushed) {
    PopActiveSnapshot();
  }
}

/**
 * @brief Identifies one stored module: a standalone one by its path, a
 * package's by package, version and path within the package.
 */
typedef struct pljs_module_id {
  char *package; // NULL for a standalone module
  char *version;
  char *path;
} pljs_module_id;

static void pljs_module_id_free(pljs_module_id *id) {
  if (id->package) {
    pfree(id->package);
  }
  if (id->version) {
    pfree(id->version);
  }
  if (id->path) {
    pfree(id->path);
  }
  memset(id, 0, sizeof(*id));
}

/**
 * @brief Looks a stored module up, and optionally reads its source, bytecode,
 * sloppy flag, requires allowlist, and is_trusted flag.
 *
 * @param id #pljs_module_id - the module
 * @param source @c uint8_t** - set to the UTF-8 source, NUL-terminated, or
 * NULL
 * @param source_len @c size_t* - set to the source's length
 * @param bytecode @c bytea** - set to a copy of the bytecode, or NULL
 * @param sloppy @c bool* - set to whether the module asks for sloppy mode, or
 * NULL
 * @param requires_out @c ArrayType** - set to a copy of the requires text[],
 * or NULL
 * @param is_trusted_out @c bool* - set to whether the module is trusted, or
 * NULL
 * @returns @c bool - whether the module exists with a source or bytecode
 */
static bool pljs_fetch_module(const pljs_module_id *id, uint8_t **source,
                              size_t *source_len, bytea **bytecode,
                              bool *sloppy, ArrayType **requires_out,
                              bool *is_trusted_out) {
  bool found = false;
  ScanKeyData scankey[3];
  int nkeys;
  Oid index_oid;

  if (source) {
    *source = NULL;
    *source_len = 0;
  }
  if (bytecode) {
    *bytecode = NULL;
  }
  if (sloppy) {
    *sloppy = false;
  }
  if (requires_out) {
    *requires_out = NULL;
  }
  if (is_trusted_out) {
    *is_trusted_out = true;
  }

  if (id->package) {
    // Attribute numbers are table column positions, not index positions.
    pljs_text_key(&scankey[0], Amodule_package_name, id->package);
    pljs_text_key(&scankey[1], Amodule_package_version, id->version);
    pljs_text_key(&scankey[2], Amodule_path, id->path);
    nkeys = 3;
    index_oid = modules_package_index;
  } else {
    pljs_text_key(&scankey[0], Amodule_path, id->path);
    nkeys = 1;
    index_oid = modules_path_index;
  }

  Relation table = table_open(modules_relid, AccessShareLock);
  Relation index = index_open(index_oid, AccessShareLock);

  bool pushed;
  Snapshot snapshot = pljs_scan_snapshot(&pushed);

  SysScanDesc scan =
      systable_beginscan_ordered(table, index, snapshot, nkeys, scankey);

  HeapTuple tuple;

  while (!found &&
         HeapTupleIsValid(tuple = systable_getnext_ordered(
                              scan, ForwardScanDirection))) {
    CHECK_FOR_INTERRUPTS();
    Datum datum_array[Amodule_length];
    bool is_null_array[Amodule_length];

    // A table made before newer columns has fewer attributes; deforming
    // leaves the rest alone, so they start out null.
    memset(is_null_array, true, sizeof(is_null_array));
    heap_deform_tuple(tuple, RelationGetDescr(table), datum_array,
                      is_null_array);

    // The path index also holds package modules; a standalone module is the
    // one with no package.
    if (!id->package && !is_null_array[Amodule_package_name - 1]) {
      continue;
    }

    if (is_null_array[Amodule_source - 1] &&
        is_null_array[Amodule_bytecode - 1]) {
      break;
    }

    if (source && !is_null_array[Amodule_source - 1]) {
      *source = pljs_module_source(datum_array[Amodule_source - 1], source_len);
    }
    if (bytecode && !is_null_array[Amodule_bytecode - 1]) {
      *bytecode = DatumGetByteaPCopy(datum_array[Amodule_bytecode - 1]);
    }
    if (sloppy && !is_null_array[Amodule_sloppy - 1]) {
      *sloppy = DatumGetBool(datum_array[Amodule_sloppy - 1]);
    }
    if (requires_out && !is_null_array[Amodule_requires - 1]) {
      *requires_out = DatumGetArrayTypePCopy(datum_array[Amodule_requires - 1]);
    }
    if (is_trusted_out && !is_null_array[Amodule_is_trusted - 1]) {
      *is_trusted_out = DatumGetBool(datum_array[Amodule_is_trusted - 1]);
    }
    found = true;
  }

  systable_endscan_ordered(scan);
  pljs_scan_snapshot_release(pushed);
  index_close(index, AccessShareLock);
  table_close(table, AccessShareLock);

  return found;
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

  Relation table = table_open(packages_relid, AccessShareLock);
  Relation index = index_open(packages_name_index, AccessShareLock);

  bool pushed;
  Snapshot snapshot = pljs_scan_snapshot(&pushed);

  SysScanDesc scan =
      systable_beginscan_ordered(table, index, snapshot, 1, scankey);

  HeapTuple tuple;
  while ((tuple = systable_getnext_ordered(scan, ForwardScanDirection)) !=
         NULL) {
    CHECK_FOR_INTERRUPTS();
    Datum datum_array[Apackage_length];
    bool is_null_array[Apackage_length];

    memset(is_null_array, true, sizeof(is_null_array));
    heap_deform_tuple(tuple, RelationGetDescr(table), datum_array,
                      is_null_array);

    if (is_null_array[Apackage_version - 1]) {
      continue;
    }

    char *ver_str = TextDatumGetCString(datum_array[Apackage_version - 1]);
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
  pljs_scan_snapshot_release(pushed);
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
static char *pljs_get_package_info(const char *pkg_name, const char *pkg_version,
                                   Datum *deps_out, bool *deps_is_null) {
  char *entry_point = NULL;

  // Defined even when the package is not found, so callers can read them.
  if (deps_is_null) {
    *deps_is_null = true;
  }

  ScanKeyData scankey[2];
  pljs_text_key(&scankey[0], Apackage_name, pkg_name);
  pljs_text_key(&scankey[1], Apackage_version, pkg_version);

  Relation table = table_open(packages_relid, AccessShareLock);
  Relation index = index_open(packages_pkey_index, AccessShareLock);

  bool pushed;
  Snapshot snapshot = pljs_scan_snapshot(&pushed);

  SysScanDesc scan =
      systable_beginscan_ordered(table, index, snapshot, 2, scankey);

  HeapTuple tuple = systable_getnext_ordered(scan, ForwardScanDirection);

  if (HeapTupleIsValid(tuple)) {
    Datum datum_array[Apackage_length];
    bool is_null_array[Apackage_length];

    memset(is_null_array, true, sizeof(is_null_array));
    heap_deform_tuple(tuple, RelationGetDescr(table), datum_array,
                      is_null_array);

    if (!is_null_array[Apackage_entry_point - 1]) {
      entry_point =
          TextDatumGetCString(datum_array[Apackage_entry_point - 1]);
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
  pljs_scan_snapshot_release(pushed);
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

  char *ep =
      pljs_get_package_info(pkg_name, pkg_version, &deps_datum, &deps_is_null);
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

/*
 * ---------- Canonical names, and resolving what a module asked for ----------
 */

#define PLJS_STANDALONE_PREFIX "pljs:"
#define PLJS_PACKAGE_PREFIX "pljs-pkg:"

/**
 * @brief Strip optional tle://, tle:, or pgtle: prefix from a module specifier.
 */
static const char *pljs_strip_spec_prefix(const char *spec) {
  if (spec == NULL) {
    return NULL;
  }
  if (strncmp(spec, "tle://", 6) == 0) {
    return spec + 6;
  }
  if (strncmp(spec, "tle:", 4) == 0) {
    return spec + 4;
  }
  if (strncmp(spec, "pgtle:", 6) == 0) {
    return spec + 6;
  }
  return spec;
}

/**
 * @brief The canonical name QuickJS knows a stored module by.
 *
 *   pljs:<path>#<generation>
 *   pljs-pkg:<package>@<version>/<path>#<generation>
 */
static char *pljs_module_id_format(const pljs_module_id *id) {
  if (id->package) {
    return psprintf(PLJS_PACKAGE_PREFIX "%s@%s/%s#" UINT64_FORMAT, id->package,
                    id->version, id->path, module_generation);
  }

  return psprintf(PLJS_STANDALONE_PREFIX "%s#" UINT64_FORMAT, id->path,
                  module_generation);
}

/**
 * @brief Reads a canonical name back into the module it names.
 *
 * The generation is ignored: it says when the name was made, not which module
 * it is.
 *
 * @returns @c bool - false when @p name is not a canonical name of ours
 */
static bool pljs_module_id_parse(const char *name, pljs_module_id *id) {
  memset(id, 0, sizeof(*id));

  if (name == NULL) {
    return false;
  }

  bool is_package = strncmp(name, PLJS_PACKAGE_PREFIX,
                            strlen(PLJS_PACKAGE_PREFIX)) == 0;

  if (!is_package &&
      strncmp(name, PLJS_STANDALONE_PREFIX, strlen(PLJS_STANDALONE_PREFIX)) !=
          0) {
    return false;
  }

  const char *rest = name + strlen(is_package ? PLJS_PACKAGE_PREFIX
                                              : PLJS_STANDALONE_PREFIX);
  const char *hash = strrchr(rest, '#');
  char *body = hash ? pnstrdup(rest, hash - rest) : pstrdup(rest);

  if (!is_package) {
    id->path = body;
    return true;
  }

  // "<package>@<version>/<path>"; a scoped package name starts with '@'.
  char *at = strchr(body + (body[0] == '@' ? 1 : 0), '@');
  char *slash = at ? strchr(at, '/') : NULL;

  if (!at || !slash) {
    pfree(body);
    return false;
  }

  id->package = pnstrdup(body, at - body);
  id->version = pnstrdup(at + 1, slash - at - 1);
  id->path = pstrdup(slash + 1);
  pfree(body);

  return true;
}

/**
 * @brief Whether a specifier is relative to the module asking for it.
 */
static bool pljs_spec_is_relative(const char *spec) {
  return spec[0] == '.' &&
         (spec[1] == '\0' || spec[1] == '/' ||
          (spec[1] == '.' && (spec[2] == '\0' || spec[2] == '/')));
}

/**
 * @brief Resolves a relative specifier against the directory of a module's
 * path: "./a" from "lib/b" is "lib/a", and "../a" is "a".
 *
 * @returns @c char* - the path, or NULL when it climbs above the root
 */
static char *pljs_join_relative(const char *base_path, const char *spec) {
  StringInfoData combined;
  char *dir = pstrdup(base_path);
  char *slash = strrchr(dir, '/');

  if (slash) {
    *slash = '\0';
  } else {
    dir[0] = '\0';
  }

  initStringInfo(&combined);
  if (dir[0] != '\0') {
    appendStringInfo(&combined, "%s/", dir);
  }
  appendStringInfoString(&combined, spec);

  // Walk the segments, keeping a stack of the ones that survive.
  char **segments = palloc(sizeof(char *) * (combined.len + 1));
  int count = 0;
  bool escaped = false;
  char *cursor = combined.data;

  while (cursor != NULL && !escaped) {
    char *next = strchr(cursor, '/');

    if (next) {
      *next++ = '\0';
    }

    if (strcmp(cursor, "..") == 0) {
      if (count == 0) {
        escaped = true;
      } else {
        count--;
      }
    } else if (cursor[0] != '\0' && strcmp(cursor, ".") != 0) {
      segments[count++] = cursor;
    }

    cursor = next;
  }

  char *result = NULL;

  if (!escaped && count > 0) {
    StringInfoData joined;

    initStringInfo(&joined);
    for (int i = 0; i < count; i++) {
      if (i > 0) {
        appendStringInfoChar(&joined, '/');
      }
      appendStringInfoString(&joined, segments[i]);
    }
    result = joined.data;
  }

  pfree(segments);
  pfree(combined.data);
  pfree(dir);

  return result;
}

typedef enum pljs_resolve_result {
  PLJS_RESOLVED,
  PLJS_NOT_FOUND,
  PLJS_ESCAPES_ROOT,
} pljs_resolve_result;

/**
 * @brief Works out which stored module a specifier means.
 *
 * @param raw_spec @c char* - what the importing code wrote
 * @param importer #pljs_module_id - the module it is written in, or NULL for
 * code that is not a stored module (a function, or a DO block)
 * @param out #pljs_module_id - the module, when it was found
 */
static pljs_resolve_result pljs_resolve_spec(const char *raw_spec,
                                             const pljs_module_id *importer,
                                             pljs_module_id *out) {
  memset(out, 0, sizeof(*out));

  if (pljs_module_id_parse(raw_spec, out)) {
    if (pljs_fetch_module(out, NULL, NULL, NULL, NULL, NULL, NULL)) {
      return PLJS_RESOLVED;
    }
    pljs_module_id_free(out);
    return PLJS_NOT_FOUND;
  }

  const char *spec = pljs_strip_spec_prefix(raw_spec);

  if (pljs_spec_is_relative(spec)) {
    if (importer == NULL) {
      return PLJS_NOT_FOUND;
    }

    char *path = pljs_join_relative(importer->path, spec);

    if (path == NULL) {
      return PLJS_ESCAPES_ROOT;
    }

    out->path = path;
    if (importer->package) {
      out->package = pstrdup(importer->package);
      out->version = pstrdup(importer->version);
    }

    if (pljs_fetch_module(out, NULL, NULL, NULL, NULL, NULL, NULL)) {
      return PLJS_RESOLVED;
    }
    pljs_module_id_free(out);
    return PLJS_NOT_FOUND;
  }

  // Otherwise a package: "<package>" or "<package>/<module>", where a scoped
  // package keeps its first slash ("@scope/pkg/module").
  // Also support inline version/range ("<package>@<range>" or "<package>@<range>/<module>").
  char *pkg_name = pstrdup(spec);
  char *sub_path = NULL;
  char *inline_range = NULL;
  char *slash = strchr(pkg_name + (pkg_name[0] == '@' ? 1 : 0), '/');

  if (slash && pkg_name[0] == '@') {
    slash = strchr(slash + 1, '/');
  }
  if (slash) {
    *slash = '\0';
    sub_path = slash + 1;
  }

  char *at = strchr(pkg_name + (pkg_name[0] == '@' ? 1 : 0), '@');
  if (at) {
    *at = '\0';
    inline_range = at + 1;
  } else if (sub_path) {
    char *sub_at = strrchr(sub_path, '@');
    if (sub_at) {
      *sub_at = '\0';
      inline_range = sub_at + 1;
    }
  }

  /*
   * What the importing package says it depends on, or its own name, is that
   * package.  A standalone module of the same name does not stand in for it:
   * rows anyone could add would otherwise replace a package's pinned
   * dependency.
   */
  char *range = NULL;
  bool is_declared = false;

  if (inline_range) {
    range = pstrdup(inline_range);
  } else if (importer && importer->package) {
    if (strcmp(importer->package, pkg_name) == 0) {
      is_declared = true;
    } else {
      range = pljs_get_package_dependency_range(importer->package,
                                                importer->version, pkg_name);
      is_declared = range != NULL;
    }
  }

  // A standalone module, by its whole path (or stripped of inline @version if
  // the exact path is not found and no package matches).
  if (!is_declared) {
    out->path = pstrdup(spec);
    if (pljs_fetch_module(out, NULL, NULL, NULL, NULL, NULL, NULL)) {
      if (range) {
        pfree(range);
      }
      pfree(pkg_name);
      return PLJS_RESOLVED;
    }
    pljs_module_id_free(out);
  }

  char *version = NULL;

  if (!inline_range && importer && importer->package &&
      strcmp(importer->package, pkg_name) == 0) {
    version = pstrdup(importer->version);
  } else {
    version = pljs_resolve_package_version(pkg_name, range);
  }

  if (range) {
    pfree(range);
  }

  if (version != NULL) {
    char *path = sub_path ? pstrdup(sub_path)
                          : pljs_get_package_info(pkg_name, version, NULL, NULL);

    if (path != NULL) {
      out->package = pkg_name;
      out->version = version;
      out->path = path;

      if (pljs_fetch_module(out, NULL, NULL, NULL, NULL, NULL, NULL)) {
        return PLJS_RESOLVED;
      }
      pljs_module_id_free(out);
      return PLJS_NOT_FOUND;
    }
    pfree(version);
  }

  // Fallback for standalone modules referenced with an inline @version suffix
  // (e.g. 'pkg/sub/back.js@1.0.0' where standalone path is 'pkg/sub/back.js').
  if (!is_declared && inline_range) {
    const char *last_at = strrchr(spec, '@');
    if (last_at && last_at > spec) {
      out->path = pnstrdup(spec, last_at - spec);
      if (pljs_fetch_module(out, NULL, NULL, NULL, NULL, NULL, NULL)) {
        pfree(pkg_name);
        return PLJS_RESOLVED;
      }
      pljs_module_id_free(out);
    }
  }

  pfree(pkg_name);
  return PLJS_NOT_FOUND;
}

/**
 * @brief Throws a JavaScript error with an attached PostgreSQL SQLSTATE.
 */
static JSValue pljs_throw_coded_module_error(JSContext *ctx, int sqlerrcode,
                                             const char *msg,
                                             bool is_ref_error) {
  module_load_failed = true;
  if (is_ref_error) {
    JS_ThrowReferenceError(ctx, "%s", msg ? msg : "error");
  } else {
    JS_ThrowInternalError(ctx, "%s", msg ? msg : "error");
  }
  if (sqlerrcode != 0) {
    JSValue ex = JS_GetException(ctx);
    if (JS_IsObject(ex)) {
      JS_DefinePropertyValueStr(ctx, ex, "sqlstate",
                                JS_NewString(ctx, unpack_sql_state(sqlerrcode)),
                                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    return JS_Throw(ctx, ex);
  }
  return JS_EXCEPTION;
}

/**
 * @brief Throws the error for a specifier that could not be resolved.
 */
static void pljs_throw_unresolved(JSContext *ctx, pljs_resolve_result result,
                                  const char *spec) {
  module_load_failed = true;

  if (result == PLJS_ESCAPES_ROOT) {
    char *msg = psprintf(
        "relative module specifier '%s' goes above the module root", spec);
    pljs_throw_coded_module_error(ctx, ERRCODE_INVALID_PARAMETER_VALUE, msg,
                                  true);
    pfree(msg);
  } else {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", spec);
  }
}

/**
 * @brief Checks module security columns (`is_trusted` and `requires`).
 *
 * Returns true if allowed, or false with a pending JavaScript exception
 * carrying `sqlstate = "42501"` (`ERRCODE_INSUFFICIENT_PRIVILEGE`).
 */
static bool pljs_check_module_access(JSContext *ctx,
                                     const pljs_module_id *importer,
                                     const pljs_module_id *target) {
  bool is_trusted = true;
  pljs_fetch_module(target, NULL, NULL, NULL, NULL, NULL, &is_trusted);

  const char *target_label =
      target->package ? target->package : target->path;

  if (!is_trusted && !superuser()) {
    char *msg = psprintf("permission denied for untrusted pljs module \"%s\"",
                         target_label);
    pljs_throw_coded_module_error(ctx, ERRCODE_INSUFFICIENT_PRIVILEGE, msg,
                                  false);
    pfree(msg);
    return false;
  }

  if (importer != NULL) {
    ArrayType *req_arr = NULL;
    pljs_fetch_module(importer, NULL, NULL, NULL, NULL, &req_arr, NULL);
    if (req_arr != NULL) {
      Datum *elems = NULL;
      bool *nulls = NULL;
      int nelems = 0;
      deconstruct_array(req_arr, TEXTOID, -1, false, TYPALIGN_INT, &elems,
                        &nulls, &nelems);

      bool allowed = false;
      for (int i = 0; i < nelems && !allowed; i++) {
        if (nulls[i]) {
          continue;
        }
        char *raw_entry = TextDatumGetCString(elems[i]);
        const char *entry = pljs_strip_spec_prefix(raw_entry);

        // Split optional @version from entry
        const char *at =
            strchr(entry + (entry[0] == '@' ? 1 : 0), '@');
        char *e_name = at ? pnstrdup(entry, at - entry) : pstrdup(entry);
        char *e_ver = at ? pstrdup(at + 1) : NULL;

        if (target->package) {
          char *pkg_sub = psprintf("%s/%s", target->package, target->path);
          if ((strcmp(e_name, target->package) == 0 ||
               strcmp(e_name, pkg_sub) == 0 ||
               strcmp(e_name, target->path) == 0) &&
              (e_ver == NULL || strcmp(e_ver, target->version) == 0)) {
            allowed = true;
          }
          pfree(pkg_sub);
        } else {
          if (strcmp(e_name, target->path) == 0 ||
              strcmp(entry, target->path) == 0) {
            allowed = true;
          }
        }

        pfree(e_name);
        if (e_ver) {
          pfree(e_ver);
        }
        pfree(raw_entry);
      }

      if (elems) {
        pfree(elems);
      }
      if (nulls) {
        pfree(nulls);
      }
      pfree(req_arr);

      if (!allowed) {
        const char *imp_label =
            importer->package ? importer->package : importer->path;
        char *msg = psprintf(
            "pljs module \"%s\" is not listed in requires of \"%s\"",
            target_label, imp_label);
        pljs_throw_coded_module_error(ctx, ERRCODE_INSUFFICIENT_PRIVILEGE, msg,
                                      false);
        pfree(msg);
        return false;
      }
    }
  }

  return true;
}

/*
 * ---------- The QuickJS loader ----------
 */

/**
 * @brief The canonical name of the module running at a stack level, or NULL
 * when the code there is not a stored module.
 */
static bool pljs_running_module(JSContext *ctx, int level,
                                pljs_module_id *importer) {
  JSAtom atom = JS_GetScriptOrModuleName(ctx, level);
  bool ok = false;

  memset(importer, 0, sizeof(*importer));

  if (atom != JS_ATOM_NULL) {
    const char *name = JS_AtomToCString(ctx, atom);

    if (name != NULL) {
      ok = pljs_module_id_parse(name, importer);
      JS_FreeCString(ctx, name);
    }

    JS_FreeAtom(ctx, atom);
  }

  return ok;
}

/**
 * @brief QuickJS module normalizer: the canonical name of what a module's
 * `import` asked for.
 */
char *pljs_module_normalize(JSContext *ctx, const char *base_name,
                            const char *name, void *opaque) {
  pljs_module_id importer, id;
  bool has_importer;
  char *canonical, *ret;

  if (syntax_check_only) {
    return js_strdup(ctx, name);
  }

  if (!pljs_module_relids()) {
    module_load_failed = true;
    JS_ThrowInternalError(ctx, "the pljs module tables are not installed");
    return NULL;
  }

  has_importer = pljs_module_id_parse(base_name, &importer);

  pljs_resolve_result result =
      pljs_resolve_spec(name, has_importer ? &importer : NULL, &id);

  if (result != PLJS_RESOLVED) {
    if (has_importer) {
      pljs_module_id_free(&importer);
    }
    pljs_throw_unresolved(ctx, result, name);
    return NULL;
  }

  if (!pljs_check_module_access(ctx, has_importer ? &importer : NULL, &id)) {
    if (has_importer) {
      pljs_module_id_free(&importer);
    }
    pljs_module_id_free(&id);
    return NULL;
  }

  if (has_importer) {
    pljs_module_id_free(&importer);
  }

  canonical = pljs_module_id_format(&id);
  ret = js_strdup(ctx, canonical);

  pfree(canonical);
  pljs_module_id_free(&id);

  return ret;
}

/*
 * The module state lives in the JavaScript context, as a hidden property of
 * the pljs object, so it goes with the context however the context goes:
 *
 *   gen      the generation it was made for
 *   memo     what each import resolved to, by importer and specifier
 *   kinds    the canonical names of modules that are CommonJS
 *   loading  the canonical names being loaded now
 *   load     a function that imports a name, for the module system to run
 */
#define PLJS_STATE_PROPERTY "__modules"

/**
 * @brief The context's module state, emptied if the generation moved on.
 *
 * @returns #JSValue - a reference the caller frees, or JS_EXCEPTION
 */
static JSValue pljs_module_state(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ns = JS_GetPropertyStr(ctx, global, "pljs");
  JSValue state = JS_GetPropertyStr(ctx, ns, PLJS_STATE_PROPERTY);

  JS_FreeValue(ctx, global);

  if (!JS_IsObject(state)) {
    JS_FreeValue(ctx, state);
    state = JS_NewObject(ctx);

    // Writable and configurable, so tearing the context down can clear it.
    JS_DefinePropertyValueStr(ctx, ns, PLJS_STATE_PROPERTY,
                              JS_DupValue(ctx, state),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  }

  JS_FreeValue(ctx, ns);

  JSValue gen = JS_GetPropertyStr(ctx, state, "gen");
  int64_t known = -1;

  JS_ToInt64(ctx, &known, gen);
  JS_FreeValue(ctx, gen);

  // Functions of sloppy modules, by canonical name.  Not emptied with the rest:
  // a name is only ever made once, and a module that is being loaded must find
  // its function even if the names move on before it runs.
  JSValue sloppy_map = JS_GetPropertyStr(ctx, state, "sloppy");

  if (!JS_IsObject(sloppy_map)) {
    JS_SetPropertyStr(ctx, state, "sloppy", JS_NewObject(ctx));
  }
  JS_FreeValue(ctx, sloppy_map);

  if (known != (int64_t)module_generation) {
    JS_SetPropertyStr(ctx, state, "gen",
                      JS_NewInt64(ctx, (int64_t)module_generation));
    JS_SetPropertyStr(ctx, state, "memo", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, state, "kinds", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, state, "loading", JS_NewObject(ctx));
  }

  return state;
}

/**
 * @brief Whether source is CommonJS: it compiles as the body of a function
 * with `exports`, `module` and `require`, which an `import` or an `export`
 * statement does not.
 */
static bool pljs_source_is_commonjs(JSContext *ctx, const char *source,
                                    size_t len) {
  if (JS_DetectModule(source, len)) {
    return false;
  }

  StringInfoData wrapped;

  initStringInfo(&wrapped);
  appendStringInfoString(&wrapped, "(function (exports, module, require) {");
  appendBinaryStringInfo(&wrapped, source, (int)len);
  appendStringInfoString(&wrapped, "\n})");

  JSValue fn = JS_Eval(ctx, wrapped.data, wrapped.len, "<commonjs>",
                       JS_EVAL_FLAG_COMPILE_ONLY);
  pfree(wrapped.data);

  if (JS_IsException(fn)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }

  JS_FreeValue(ctx, fn);

  return true;
}

/**
 * @brief Appends a string as a JavaScript string literal.
 */
static void pljs_append_js_string(StringInfo out, const char *s) {
  appendStringInfoChar(out, '"');

  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;

    if (c == '"' || c == '\\') {
      appendStringInfoChar(out, '\\');
      appendStringInfoChar(out, c);
    } else if (c < 0x20) {
      appendStringInfo(out, "\\u%04x", c);
    } else {
      appendStringInfoChar(out, c);
    }
  }

  appendStringInfoChar(out, '"');
}

/**
 * @brief Compiles a CommonJS module that asked for sloppy mode.
 *
 * Its code is a function compiled as a plain script, which is sloppy unless
 * the code says otherwise, and the module made for the loader only calls it.
 * Everything else about it is as for any CommonJS module: its own `module` and
 * `exports`, `require`, the default export being its `module.exports`.
 *
 * @returns #JSValue - the module, compiled, or JS_EXCEPTION
 */
static JSValue pljs_compile_sloppy_commonjs(JSContext *ctx, const char *name,
                                            const char *source, size_t len) {
  StringInfoData wrapped;

  initStringInfo(&wrapped);
  appendStringInfoString(&wrapped, "(function (exports, module, require) {");
  appendBinaryStringInfo(&wrapped, source, (int)len);
  appendStringInfoString(&wrapped, "\n})");

  JSValue fn = JS_Eval(ctx, wrapped.data, wrapped.len, name, 0);
  pfree(wrapped.data);

  if (JS_IsException(fn)) {
    return fn;
  }

  JSValue state = pljs_module_state(ctx);
  JSValue sloppy_map = JS_GetPropertyStr(ctx, state, "sloppy");

  JS_SetPropertyStr(ctx, sloppy_map, name, fn);
  JS_FreeValue(ctx, sloppy_map);
  JS_FreeValue(ctx, state);

  initStringInfo(&wrapped);
  appendStringInfoString(&wrapped, "const module = { exports: {} }; pljs."
                                   PLJS_STATE_PROPERTY ".sloppy[");
  pljs_append_js_string(&wrapped, name);
  appendStringInfoString(
      &wrapped, "].call(module.exports, module.exports, module, "
                "pljs.require);\nexport default module.exports;\n");

  JSValue compiled = JS_Eval(ctx, wrapped.data, wrapped.len, name,
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  pfree(wrapped.data);

  return compiled;
}

typedef struct pljs_js_module_def_head {
  int ref_count;
  uint8_t gc_obj_type_and_mark;
  uint8_t dummy1;
  uint16_t dummy2;
  void *gc_link_prev;
  void *gc_link_next;
  JSAtom module_name;
} pljs_js_module_def_head;

/**
 * @brief QuickJS module loader: compiles the stored module a canonical name
 * names.
 *
 * A CommonJS module becomes an ES module whose default export is its
 * `module.exports`, so the two kinds import each other.  The wrapper starts on
 * the source's first line, so errors still point into the stored source.
 * Precompiled QuickJS bytecode (`bytecode BYTEA`) is deserialized directly via
 * `JS_ReadObject`.
 */
JSModuleDef *pljs_module_loader(JSContext *ctx, const char *name,
                                void *opaque) {
  pljs_module_id id;
  uint8_t *source = NULL;
  size_t source_len = 0;
  bytea *bytecode = NULL;
  bool sloppy = false;
  JSModuleDef *m = NULL;

  if (syntax_check_only) {
    JSValue empty = JS_Eval(ctx, "", 0, name,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);

    if (JS_IsException(empty)) {
      return NULL;
    }

    m = JS_VALUE_GET_PTR(empty);
    JS_FreeValue(ctx, empty);

    return m;
  }

  if (!pljs_module_relids()) {
    module_load_failed = true;
    JS_ThrowInternalError(ctx, "the pljs module tables are not installed");
    return NULL;
  }

  if (!pljs_module_id_parse(name, &id) ||
      !pljs_fetch_module(&id, &source, &source_len, &bytecode, &sloppy, NULL,
                         NULL)) {
    module_load_failed = true;
    JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
    pljs_module_id_free(&id);
    return NULL;
  }

  pljs_module_id_free(&id);

  if (bytecode != NULL) {
    const uint8_t *buf = (const uint8_t *)VARDATA_ANY(bytecode);
    size_t buf_len = VARSIZE_ANY_EXHDR(bytecode);
    JSValue func_val = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
    bool has_source = (source != NULL && source_len > 0);
    bool commonjs =
        has_source && pljs_source_is_commonjs(ctx, (char *)source, source_len);
    pfree(bytecode);
    if (source) {
      pfree(source);
    }

    if (JS_IsException(func_val)) {
      module_load_failed = true;
      return NULL;
    }

    if (JS_VALUE_GET_TAG(func_val) != JS_TAG_MODULE) {
      JS_FreeValue(ctx, func_val);
      module_load_failed = true;
      JS_ThrowTypeError(ctx, "bytecode for '%s' is not a compiled module",
                        name);
      return NULL;
    }

    pljs_js_module_def_head *mh =
        (pljs_js_module_def_head *)JS_VALUE_GET_PTR(func_val);
    JSAtom new_name = JS_NewAtom(ctx, name);
    if (new_name != JS_ATOM_NULL) {
      JS_FreeAtom(ctx, mh->module_name);
      mh->module_name = new_name;
    }

    if (JS_ResolveModule(ctx, func_val) < 0) {
      JS_FreeValue(ctx, func_val);
      module_load_failed = true;
      return NULL;
    }

    m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);

    if (commonjs || !has_source) {
      JSValue state = pljs_module_state(ctx);
      JSValue kinds = JS_GetPropertyStr(ctx, state, "kinds");

      JS_SetPropertyStr(ctx, kinds, name,
                        commonjs ? JS_TRUE : JS_NewInt32(ctx, 2));
      JS_FreeValue(ctx, kinds);
      JS_FreeValue(ctx, state);
    }

    return m;
  }

  bool commonjs = pljs_source_is_commonjs(ctx, (char *)source, source_len);
  JSValue compiled;

  if (commonjs && sloppy) {
    compiled = pljs_compile_sloppy_commonjs(ctx, name, (char *)source,
                                            source_len);
  } else if (commonjs) {
    StringInfoData wrapped;

    initStringInfo(&wrapped);
    appendStringInfoString(
        &wrapped, "const module = { exports: {} }; (function (exports, module, "
                  "require) {");
    appendBinaryStringInfo(&wrapped, (char *)source, (int)source_len);
    appendStringInfoString(&wrapped,
                           "\n}).call(module.exports, module.exports, module, "
                           "pljs.require);\nexport default module.exports;\n");

    compiled = JS_Eval(ctx, wrapped.data, wrapped.len, name,
                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    pfree(wrapped.data);
  } else {
    compiled = JS_Eval(ctx, (char *)source, source_len, name,
                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  }

  pfree(source);

  if (JS_IsException(compiled)) {
    module_load_failed = true;
    return NULL;
  }

  m = JS_VALUE_GET_PTR(compiled);
  JS_FreeValue(ctx, compiled);

  if (commonjs) {
    JSValue state = pljs_module_state(ctx);
    JSValue kinds = JS_GetPropertyStr(ctx, state, "kinds");

    JS_SetPropertyStr(ctx, kinds, name, JS_TRUE);
    JS_FreeValue(ctx, kinds);
    JS_FreeValue(ctx, state);
  }

  return m;
}

/**
 * @brief Waits for a promise, running the jobs that settle it.
 *
 * Unlike js_std_await() it gives up, with an error, when nothing is left to
 * run and the promise still has not settled: a module waiting on one that is
 * waiting on it, or on an `await` that never finishes, would otherwise spin
 * the backend forever.
 *
 * @param obj #JSValue - the promise; consumed
 */
static JSValue pljs_await(JSContext *ctx, JSValue obj) {
  for (;;) {
    switch (JS_PromiseState(ctx, obj)) {
    case JS_PROMISE_FULFILLED: {
      JSValue ret = JS_PromiseResult(ctx, obj);

      JS_FreeValue(ctx, obj);
      return ret;
    }
    case JS_PROMISE_REJECTED: {
      JSValue ret = JS_Throw(ctx, JS_PromiseResult(ctx, obj));

      JS_FreeValue(ctx, obj);
      return ret;
    }
    case JS_PROMISE_PENDING: {
      JSContext *job_ctx;

      int ran = JS_ExecutePendingJob(JS_GetRuntime(ctx), &job_ctx);

      if (ran < 0) {
        JS_FreeValue(job_ctx, JS_GetException(job_ctx));
      } else if (ran == 0) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowInternalError(
            ctx, "module did not finish loading: it is waiting on itself, or "
                 "on an await that never completes");
      }
      break;
    }
    default:
      // Not a promise: the value itself.
      return obj;
    }
  }
}

/**
 * @brief Imports a canonical name and returns the module's namespace.
 *
 * Goes through a dynamic `import()` in a function made once per context, so
 * QuickJS finds the module it already holds under that name, or loads it.
 */
static JSValue pljs_import_canonical(JSContext *ctx, JSValueConst state,
                                     const char *canonical) {
  JSValue load = JS_GetPropertyStr(ctx, state, "load");

  if (!JS_IsFunction(ctx, load)) {
    JS_FreeValue(ctx, load);
    static const char helper[] = "(function (name) { return import(name); })";

    load = JS_Eval(ctx, helper, sizeof(helper) - 1, "<pljs-import>", 0);

    if (JS_IsException(load)) {
      return load;
    }

    JS_SetPropertyStr(ctx, state, "load", JS_DupValue(ctx, load));
  }

  module_load_failed = false;

  JSValue arg = JS_NewString(ctx, canonical);
  JSValue promise = JS_Call(ctx, load, JS_UNDEFINED, 1, &arg);

  JS_FreeValue(ctx, arg);
  JS_FreeValue(ctx, load);

  if (JS_IsException(promise)) {
    return promise;
  }

  return pljs_await(ctx, promise);
}

// Depth of the pljs.import()/pljs.require() calls running now, against
// pljs.max_module_depth.
static int module_depth = 0;

/**
 * @brief One attempt at importing or requiring a stored module.
 *
 * @param stale @c bool* - set when the module tables changed while the module
 * was loading, so the result may be of the wrong generation
 */
static JSValue pljs_module_load_once(JSContext *ctx, const char *spec,
                                     bool as_require, bool *stale) {
  JSValue state = pljs_module_state(ctx);
  JSValue memo = JS_GetPropertyStr(ctx, state, "memo");
  JSValue result = JS_EXCEPTION;
  pljs_module_id importer, id;
  char *importer_name = NULL;
  char *canonical = NULL;
  uint64 generation = module_generation;
  uint64 table_changes = module_table_changes;
  JSValue kinds_before = JS_GetPropertyStr(ctx, state, "kinds");

  memset(&id, 0, sizeof(id));
  *stale = false;

  // Level 1 is the code that called this native function.
  bool has_importer = pljs_running_module(ctx, 1, &importer);

  if (has_importer) {
    importer_name = pljs_module_id_format(&importer);
  }

  char *key = psprintf("%c\x1f%s\x1f%s", as_require ? 'r' : 'i',
                       importer_name ? importer_name : "", spec);
  JSValue hit = JS_GetPropertyStr(ctx, memo, key);

  if (!JS_IsUndefined(hit)) {
    result = hit;
    goto done;
  }

  pljs_resolve_result resolved =
      pljs_resolve_spec(spec, has_importer ? &importer : NULL, &id);

  if (resolved != PLJS_RESOLVED) {
    pljs_throw_unresolved(ctx, resolved, spec);
    goto done;
  }

  if (!pljs_check_module_access(ctx, has_importer ? &importer : NULL, &id)) {
    goto done;
  }

  canonical = pljs_module_id_format(&id);

  JSValue loading = JS_GetPropertyStr(ctx, state, "loading");
  JSValue being = JS_GetPropertyStr(ctx, loading, canonical);

  if (JS_ToBool(ctx, being)) {
    JS_ThrowInternalError(ctx, "circular module dependency: '%s' is loading",
                          spec);
  } else {
    JS_SetPropertyStr(ctx, loading, canonical, JS_TRUE);
    module_depth++;

    JSValue ns = pljs_import_canonical(ctx, state, canonical);

    module_depth--;

    JSAtom atom = JS_NewAtom(ctx, canonical);

    JS_DeleteProperty(ctx, loading, atom, 0);
    JS_FreeAtom(ctx, atom);

    if (module_table_changes != table_changes) {
      // The tables changed while it loaded, so it may be the old modules under
      // a new name; the caller tries again.
      *stale = true;

      if (JS_IsException(ns)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
      } else {
        JS_FreeValue(ctx, ns);
      }
    } else if (JS_IsException(ns)) {
      // A module that threw while it was evaluated stays broken under its
      // name, so move the names on; see module_generation.  Not for one that
      // was merely not found, which left nothing behind.
      if (!module_load_failed) {
        module_generation++;
      }
    } else {
      // The loader noted a CommonJS module in the kinds of whichever state was
      // current as it loaded.
      JSValue kinds = JS_GetPropertyStr(ctx, state, "kinds");
      JSValue is_cjs = JS_GetPropertyStr(ctx, kinds, canonical);

      if (!JS_ToBool(ctx, is_cjs)) {
        JS_FreeValue(ctx, is_cjs);
        is_cjs = JS_GetPropertyStr(ctx, kinds_before, canonical);
      }

      bool unwrap_default = false;
      if (as_require) {
        if (JS_IsBool(is_cjs) && JS_ToBool(ctx, is_cjs)) {
          unwrap_default = true;
        } else if (JS_IsNumber(is_cjs) && JS_IsObject(ns)) {
          JSPropertyEnum *ptab = NULL;
          uint32_t plen = 0;
          if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, ns,
                                     JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) ==
              0) {
            if (plen == 1) {
              const char *kname = JS_AtomToCString(ctx, ptab[0].atom);
              if (kname && strcmp(kname, "default") == 0) {
                unwrap_default = true;
              }
              if (kname) {
                JS_FreeCString(ctx, kname);
              }
            }
            for (uint32_t i = 0; i < plen; i++) {
              JS_FreeAtom(ctx, ptab[i].atom);
            }
            js_free(ctx, ptab);
          }
        }
      }

      if (unwrap_default) {
        result = JS_GetPropertyStr(ctx, ns, "default");
        JS_FreeValue(ctx, ns);
      } else {
        result = ns;
      }

      JS_FreeValue(ctx, is_cjs);
      JS_FreeValue(ctx, kinds);

      // Remembered unless the names moved on meanwhile, which emptied the
      // state this was loaded against.
      if (!JS_IsException(result) && module_generation == generation) {
        JS_SetPropertyStr(ctx, memo, key, JS_DupValue(ctx, result));
      }
    }
  }

  JS_FreeValue(ctx, being);
  JS_FreeValue(ctx, loading);

done:
  if (canonical) {
    pfree(canonical);
  }
  if (has_importer) {
    pljs_module_id_free(&importer);
  }
  if (importer_name) {
    pfree(importer_name);
  }
  pljs_module_id_free(&id);
  pfree(key);
  JS_FreeValue(ctx, kinds_before);
  JS_FreeValue(ctx, memo);
  JS_FreeValue(ctx, state);

  return result;
}

/**
 * @brief Import or require a stored module.
 *
 * What an import resolved to is remembered per context, by the module it was
 * written in and the specifier, so the second `pljs.import('x')` in a function
 * does no catalog reads at all.  It is forgotten, with the module, when the
 * generation moves on.
 *
 * Any failure leaves the JavaScript exception pending.
 *
 * @param spec @c char* - what the code asked for
 * @param as_require @c bool - whether to give a CommonJS module's exports
 * rather than its namespace
 */
static JSValue pljs_module_load(JSContext *ctx, const char *spec,
                                bool as_require) {
  const char *what = as_require ? "require" : "import";

  if (module_depth >= configuration.max_module_depth) {
    return JS_ThrowInternalError(
        ctx, "module %s depth exceeded (%d): possible circular dependency",
        what, configuration.max_module_depth);
  }

  // See changes other backends have committed since this one last looked.
  AcceptInvalidationMessages();

  if (!pljs_module_relids()) {
    return JS_ThrowInternalError(ctx,
                                 "the pljs module tables are not installed");
  }

  /*
   * Looking at the tables can itself show this backend a change that was
   * committed meanwhile.  A load that straddles one is done again, under the
   * new generation.
   *
   * The tables are read in the middle of QuickJS's work, and a PostgreSQL error
   * there -- a cancel while waiting for a lock on them, running out of memory
   * -- leaves for the caller without unwinding what this keeps: how deep the
   * loads are, and which modules are marked as loading, the latter of which
   * would make every later import of them a "circular dependency".
   */
  volatile int saved_depth = module_depth;
  volatile bool loaded = false;
  JSValue volatile result = JS_UNDEFINED;

  PG_TRY();
  {
    for (int attempt = 0; attempt < 3 && !loaded; attempt++) {
      bool stale;
      JSValue attempted = pljs_module_load_once(ctx, spec, as_require, &stale);

      if (stale) {
        JS_FreeValue(ctx, attempted);
      } else {
        result = attempted;
        loaded = true;
      }
    }
  }
  PG_CATCH();
  {
    module_depth = saved_depth;
    syntax_check_only = false;

    JSValue state = pljs_module_state(ctx);

    JS_SetPropertyStr(ctx, state, "loading", JS_NewObject(ctx));
    JS_FreeValue(ctx, state);

    PG_RE_THROW();
  }
  PG_END_TRY();

  if (loaded) {
    return result;
  }

  return JS_ThrowInternalError(ctx, "module '%s' kept changing while it loaded",
                               spec);
}

/**
 * @brief Import a JavaScript module from the database.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name What to import.
 * @returns JSValue The module namespace, or JS_EXCEPTION.
 */
JSValue pljs_module_import(JSContext *ctx, const char *module_name) {
  return pljs_module_load(ctx, module_name, false);
}

/**
 * @brief Require a JavaScript module from the database.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name What to require.
 * @returns JSValue A CommonJS module's `module.exports`, or an ES module's
 * namespace, or JS_EXCEPTION.
 */
JSValue pljs_module_require(JSContext *ctx, const char *module_name) {
  return pljs_module_load(ctx, module_name, true);
}

/**
 * @brief `pljs.compile_bytecode(source[, module_name])` compiles an ES or
 * CommonJS module source string into QuickJS serialized bytecode
 * (`Uint8Array` / `bytea`) that can be stored in `pljs.modules.bytecode`.
 */
JSValue pljs_compile_bytecode_js(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  if (argc < 1) {
    return js_throw(
        "pljs.compile_bytecode(source[, module_name]) requires source", ctx);
  }

  const char *src = JS_ToCString(ctx, argv[0]);
  if (!src) {
    return JS_EXCEPTION;
  }

  const char *raw_mod_name = NULL;
  char *norm_mod_name = NULL;
  const char *mod_name = "<bytecode_module>";
  if (argc >= 2 && JS_IsString(argv[1])) {
    raw_mod_name = JS_ToCString(ctx, argv[1]);
    if (raw_mod_name && raw_mod_name[0] != '\0') {
      if (strncmp(raw_mod_name, PLJS_PACKAGE_PREFIX,
                  strlen(PLJS_PACKAGE_PREFIX)) == 0 ||
          strncmp(raw_mod_name, PLJS_STANDALONE_PREFIX,
                  strlen(PLJS_STANDALONE_PREFIX)) == 0) {
        mod_name = raw_mod_name;
      } else {
        const char *clean = pljs_strip_spec_prefix(raw_mod_name);
        if (strncmp(clean, "plv8-pkg:", 9) == 0) {
          clean += 9;
        } else if (strncmp(clean, "plv8:", 5) == 0) {
          clean += 5;
        }
        const char *at = strchr(clean + (clean[0] == '@' ? 1 : 0), '@');
        if (at != NULL) {
          const char *slash = strchr(at, '/');
          if (slash != NULL) {
            norm_mod_name = psprintf(PLJS_PACKAGE_PREFIX "%s", clean);
          } else {
            norm_mod_name = psprintf(PLJS_PACKAGE_PREFIX "%s/index.js", clean);
          }
        } else {
          norm_mod_name = psprintf(PLJS_STANDALONE_PREFIX "%s", clean);
        }
        mod_name = norm_mod_name;
      }
    }
  }

  size_t src_len = strlen(src);
  bool commonjs = pljs_source_is_commonjs(ctx, src, src_len);
  JSValue obj;

  syntax_check_only = true;
  if (commonjs) {
    StringInfoData wrapped;

    initStringInfo(&wrapped);
    appendStringInfoString(
        &wrapped, "const module = { exports: {} }; (function (exports, module, "
                  "require) {");
    appendBinaryStringInfo(&wrapped, src, (int)src_len);
    appendStringInfoString(&wrapped,
                           "\n}).call(module.exports, module.exports, module, "
                           "pljs.require);\nexport default module.exports;\n");

    obj = JS_Eval(ctx, wrapped.data, wrapped.len, mod_name,
                  JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    pfree(wrapped.data);
  } else {
    obj = JS_Eval(ctx, src, src_len, mod_name,
                  JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  }
  syntax_check_only = false;

  if (norm_mod_name) {
    pfree(norm_mod_name);
  }
  if (raw_mod_name) {
    JS_FreeCString(ctx, raw_mod_name);
  }
  JS_FreeCString(ctx, src);

  if (JS_IsException(obj)) {
    return obj;
  }

  size_t out_len = 0;
  uint8_t *out_buf =
      JS_WriteObject(ctx, &out_len, obj, JS_WRITE_OBJ_BYTECODE);
  JS_FreeValue(ctx, obj);

  if (!out_buf) {
    return JS_EXCEPTION;
  }

  JSValue ab = JS_NewArrayBufferCopy(ctx, out_buf, out_len);
  js_free(ctx, out_buf);
  if (JS_IsException(ab)) {
    return ab;
  }

  JSValue global_obj = JS_GetGlobalObject(ctx);
  JSValue u8_ctor = JS_GetPropertyStr(ctx, global_obj, "Uint8Array");
  JSValue args[1] = {ab};
  JSValue u8 = JS_CallConstructor(ctx, u8_ctor, 1, args);
  JS_FreeValue(ctx, ab);
  JS_FreeValue(ctx, u8_ctor);
  JS_FreeValue(ctx, global_obj);
  return u8;
}

/**
 * @brief The generation of the loaded modules, after seeing every change
 * committed so far.
 *
 * A compiled function that imports modules binds them when it is compiled, so
 * it records this and is compiled again when it no longer matches.  Never 0.
 */
uint64 pljs_modules_generation(void) {
  AcceptInvalidationMessages();

  return module_generation;
}

/**
 * @brief Evaluates source as a module and returns its namespace.
 *
 * The module's own `import` statements are loaded the way any other module's
 * are.  A failure leaves the JavaScript exception pending.
 *
 * @param source @c char* - the module's source
 * @param len @c size_t - its length
 * @param name @c char* - what to call it, as errors show it
 * @param check_only @c bool - only compile it, so that a syntax error is found
 * without reading what it imports; the result is then undefined
 */
JSValue pljs_module_eval_source(JSContext *ctx, const char *source, size_t len,
                                const char *name, bool check_only) {
  JSValue val;

  if (check_only) {
    syntax_check_only = true;
    val = JS_Eval(ctx, source, len, name,
                  JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    syntax_check_only = false;

    if (JS_IsException(val)) {
      return val;
    }

    JS_FreeValue(ctx, val);
    return JS_UNDEFINED;
  }

  AcceptInvalidationMessages();

  if (!pljs_module_relids()) {
    return JS_ThrowInternalError(ctx,
                                 "the pljs module tables are not installed");
  }

  module_load_failed = false;

  val = JS_Eval(ctx, source, len, name,
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);

  if (JS_IsException(val)) {
    return val;
  }

  // Compiling a module loads what it imports; see syntax_check_only.
  JSModuleDef *m = JS_VALUE_GET_PTR(val);
  JSValue done = pljs_await(ctx, JS_EvalFunction(ctx, JS_DupValue(ctx, val)));

  JS_FreeValue(ctx, val);

  if (JS_IsException(done)) {
    // See module_generation.
    if (!module_load_failed) {
      module_generation++;
    }
    return done;
  }

  JS_FreeValue(ctx, done);

  return JS_GetModuleNamespace(ctx, m);
}

/*
 * ---------- Static imports at the top of a function ----------
 */

static bool pljs_is_ident_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '$' ||
         (unsigned char)c >= 0x80;
}

/**
 * @brief Skips white space and comments.
 *
 * @returns @c char* - where the next token starts, or the end of the string
 * if a comment is not closed
 */
static const char *pljs_skip_trivia(const char *p) {
  for (;;) {
    while (isspace((unsigned char)*p)) {
      p++;
    }

    if (p[0] == '/' && p[1] == '/') {
      while (*p && *p != '\n') {
        p++;
      }
    } else if (p[0] == '/' && p[1] == '*') {
      const char *end = strstr(p + 2, "*/");

      if (end == NULL) {
        return p + strlen(p);
      }
      p = end + 2;
    } else {
      return p;
    }
  }
}

/**
 * @brief Skips a quoted string.
 *
 * @param p @c char* - at the opening quote
 * @returns @c char* - after the closing quote, or NULL if there is none
 */
static const char *pljs_skip_string(const char *p) {
  char quote = *p++;

  while (*p && *p != quote) {
    if (*p == '\\' && p[1]) {
      p++;
    }
    p++;
  }

  return *p ? p + 1 : NULL;
}

/**
 * @brief Whether a word starts at p.
 */
static bool pljs_at_word(const char *p, const char *word) {
  size_t len = strlen(word);

  return strncmp(p, word, len) == 0 && !pljs_is_ident_char(p[len]);
}

/**
 * @brief Finds the end of the static import statement at p.
 *
 * Strings and comments are skipped and braces counted, so a `;`, a newline or
 * a quote inside `import { "a-b" as c } from "m"` does not end it early, and
 * `import("m")` and `import.meta` are not imports at all.
 *
 * @param p @c char* - at the word `import`
 * @returns @c char* - after the statement, or NULL if it is not one
 */
static const char *pljs_skip_import(const char *p) {
  const char *q = p + strlen("import");
  int depth = 0;

  if (pljs_is_ident_char(*q)) {
    return NULL;
  }

  q = pljs_skip_trivia(q);

  if (*q == '(' || *q == '.') {
    return NULL;
  }

  // Up to the module specifier: the first string outside the braces.
  for (;;) {
    q = pljs_skip_trivia(q);

    if (*q == '\0') {
      return NULL;
    }

    if (*q == '"' || *q == '\'') {
      q = pljs_skip_string(q);

      if (q == NULL) {
        return NULL;
      }
      if (depth == 0) {
        break;
      }
      continue;
    }

    if (*q == '{') {
      depth++;
    } else if (*q == '}') {
      depth--;
    }
    q++;
  }

  // Import attributes: `with { type: "json" }`.
  const char *r = pljs_skip_trivia(q);

  if (pljs_at_word(r, "with") || pljs_at_word(r, "assert")) {
    r = pljs_skip_trivia(r + (pljs_at_word(r, "with") ? 4 : 6));

    if (*r == '{') {
      depth = 0;
      for (; *r; r++) {
        if (*r == '"' || *r == '\'') {
          r = pljs_skip_string(r);
          if (r == NULL) {
            return NULL;
          }
          r--;
        } else if (*r == '{') {
          depth++;
        } else if (*r == '}' && --depth == 0) {
          q = r + 1;
          break;
        }
      }
    }
  }

  // The statement's own semicolon, if it has one.
  r = pljs_skip_trivia(q);
  if (*r == ';') {
    q = r + 1;
  }

  return q;
}

/**
 * @brief Appends an import statement on a single line, so that hoisting it
 * does not move the lines of the code after it.
 */
static void pljs_append_import(StringInfo out, const char *p, const char *end) {
  while (p < end) {
    if (*p == '"' || *p == '\'') {
      const char *after = pljs_skip_string(p);

      appendBinaryStringInfo(out, p, after - p);
      p = after;
    } else if (p[0] == '/' && p[1] == '/') {
      while (p < end && *p != '\n') {
        p++;
      }
      appendStringInfoChar(out, ' ');
    } else if (p[0] == '/' && p[1] == '*') {
      const char *close = strstr(p + 2, "*/");

      p = close ? close + 2 : end;
      appendStringInfoChar(out, ' ');
    } else {
      appendStringInfoChar(out, isspace((unsigned char)*p) ? ' ' : *p);
      p++;
    }
  }

  // Joined on one line, so a statement that relied on its newline needs a `;`.
  while (out->len > 0 && out->data[out->len - 1] == ' ') {
    out->data[--out->len] = '\0';
  }
  if (out->len > 0 && out->data[out->len - 1] != ';') {
    appendStringInfoChar(out, ';');
  }
  appendStringInfoChar(out, ' ');
}

/**
 * @brief Splits the static `import` statements off the front of a function.
 *
 * Only the imports at the very start count -- after white space and comments,
 * before any other code -- as an `import` is only allowed at the top level of
 * a module.
 *
 * @param source @c char* - the function's body
 * @param imports #StringInfo - receives the imports, on one line
 * @param body #StringInfo - receives the body with the imports blanked out, so
 * every other character keeps its line and column
 * @returns @c bool - whether there were any
 */
bool pljs_extract_static_imports(const char *source, StringInfo imports,
                                 StringInfo body) {
  const char *p = source;
  bool found = false;

  appendStringInfoString(body, source);

  for (;;) {
    p = pljs_skip_trivia(p);

    if (!pljs_at_word(p, "import")) {
      break;
    }

    const char *end = pljs_skip_import(p);

    if (end == NULL) {
      break;
    }

    pljs_append_import(imports, p, end);

    for (const char *c = p; c < end; c++) {
      if (*c != '\n' && *c != '\r') {
        body->data[c - source] = ' ';
      }
    }

    p = end;
    found = true;
  }

  return found;
}
