#include "postgres.h"

#include "access/genam.h"
#include "catalog/namespace.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "pljs.h"

enum module_data_access {
  Amodule_path = 1,
  Amodule_source,
  Amodule_created_at,
  Amodule_updated_at,
  Amodule_length = 4
};

/**
 * @brief Get the schema oid for the pljs namespace.
 *
 * @return Oid The schema oid for the pljs namespace.
 */
static Oid get_pljs_schema_oid(void) {
  return get_namespace_oid("pljs", false);
}

/**
 * @brief Get the relation oid for the pljs module.
 *
 * @return Oid The relation oid for the pljs module.
 */
static Oid get_pljs_module_relid(void) {
  return get_relname_relid("modules", get_pljs_schema_oid());
}

/**
 * @brief Get the relation oid for the pljs module index.
 *
 * @return Oid The relation oid for the pljs module index.
 */
static Oid get_pljs_module_index_relid(void) {
  return get_relname_relid("pljs_modules_path", get_pljs_schema_oid());
}

int pljs_js_module_set_import_meta(JSContext *ctx, JSValueConst func_val,
                                   JS_BOOL use_realpath, JS_BOOL is_main) {
  JSModuleDef *m;
  char buf[PATH_MAX + 16];
  JSValue meta_obj;
  JSAtom module_name_atom;
  const char *module_name;

  if (JS_VALUE_GET_TAG(func_val) != JS_TAG_MODULE) {
    elog(ERROR, "Bad Magic");
    return -1;
  }

  m = JS_VALUE_GET_PTR(func_val);

  module_name_atom = JS_GetModuleName(ctx, m);
  module_name = JS_AtomToCString(ctx, module_name_atom);
  JS_FreeAtom(ctx, module_name_atom);
  if (!module_name)
    return -1;
  if (!strchr(module_name, ':')) {
    strcpy(buf, "file://");
    {
      strcat(buf, module_name);
    }
  } else {
    strncpy(buf, module_name, sizeof(buf));
  }
  JS_FreeCString(ctx, module_name);

  meta_obj = JS_GetImportMeta(ctx, m);
  if (JS_IsException(meta_obj))
    return -1;
  elog(NOTICE, "url: %s", buf);
  JS_DefinePropertyValueStr(ctx, meta_obj, "url", JS_NewString(ctx, buf),
                            JS_PROP_C_W_E);
  JS_DefinePropertyValueStr(ctx, meta_obj, "main", JS_NewBool(ctx, is_main),
                            JS_PROP_C_W_E);
  JS_FreeValue(ctx, meta_obj);
  return 0;
}

uint8_t *pljs_read_module(size_t *pbuf_len, const char *filename) {
  // Initialize any return data.
  uint8_t *ret = NULL;
  *pbuf_len = 0;

  // We need enough scan key data to load the module: the name.
  ScanKeyData scankey[1];

  // Set up the scan key: filename.
  ScanKeyInit(&scankey[0], Amodule_path, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(filename));

  /* Open up the table and index for scanning. */
  Relation table = table_open(get_pljs_module_relid(), AccessShareLock);
  Relation index = index_open(get_pljs_module_index_relid(), AccessShareLock);

  /* Set up the scan. */
  SysScanDesc scan_descriptor =
      systable_beginscan_ordered(table, index, GetActiveSnapshot(), 1, scankey);

  HeapTuple tuple =
      systable_getnext_ordered(scan_descriptor, ForwardScanDirection);

  if (HeapTupleIsValid(tuple)) {
    Datum datum_array[Amodule_length];
    bool is_null_array[Amodule_length];

    TupleDesc tuple_desc = RelationGetDescr(table);

    /* Convert the tuple into arrays of Datum and null values. */
    heap_deform_tuple(tuple, tuple_desc, datum_array, is_null_array);

    text *source = DatumGetTextP(datum_array[Amodule_source - 1]);
    size_t len = VARSIZE(source) - VARHDRSZ;

    ret = (uint8_t *)palloc(len + 1);
    memcpy(ret, VARDATA(source), len);
    ret[len] = 0;

    *pbuf_len = len;
  }

  systable_endscan_ordered(scan_descriptor);
  index_close(index, AccessShareLock);
  table_close(table, AccessShareLock);

  return ret;
}

/**
 * @brief Import a JavaScript module from the databse.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name The name of the module to load.
 * @returns JSValue A copy of the compiled function.
 */
JSValue pljs_module_load(JSContext *ctx, const char *module_name) {
  size_t buf_len;
  uint8_t *buf;
  JSValue func_val;

  buf = pljs_read_module(&buf_len, module_name);
  if (!buf) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    elog(NOTICE, "could not load module '%s'", module_name);

    return JS_EXCEPTION;
  }

  /* compile the module */
  func_val = JS_Eval(ctx, (char *)buf, buf_len, module_name,
                     JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  pfree(buf);

  if (JS_IsException(func_val)) {
    return JS_EXCEPTION;
  }

  /* XXX: could propagate the exception */
  pljs_js_module_set_import_meta(ctx, func_val, true, false);

  return func_val;
}

/**
 * @brief Import a JavaScript module from the databse.
 *
 * Default module loader implementation.  The default behavior is to load
 * the module from the database.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name The name of the module to load.
 * @param opaque An opaque pointer that can be used by the module loader.
 * @returns JSModuleDef* A pointer to the loaded module, or NULL on error.
 */
JSModuleDef *pljs_defaultjs_module_loader(JSContext *ctx,
                                          const char *module_name,
                                          void *opaque) {
  JSModuleDef *m;

  JSValue func_val = pljs_module_load(ctx, module_name);

  if (JS_IsException(func_val)) {
    return NULL;
  }

  /* the module is already referenced, so we must free it */
  m = JS_VALUE_GET_PTR(func_val);
  JS_FreeValue(ctx, func_val);

  return m;
}
