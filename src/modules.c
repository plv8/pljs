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

static uint8_t *pljs_read_module(size_t *pbuf_len, const char *filename) {
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
 * @brief Import a JavaScript module from the database.
 *
 * @param ctx The JavaScript context to use for loading the module.
 * @param module_name The name of the module to load.
 * @returns JSValue A copy of the compiled module.
 */
JSValue pljs_module_load(JSContext *ctx, const char *module_name) {
  size_t buf_len;
  char *buf;

  buf = (char *)pljs_read_module(&buf_len, module_name);
  if (!buf) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);

    return JS_EXCEPTION;
  }

  /* compile the module */
  JSValue res = JS_Eval(ctx, buf, buf_len, module_name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  // pfree(buf);

  if (JS_IsException(res)) {
    JSValue ex = JS_GetException(ctx);
    const char *err = JS_ToCString(ctx, ex);
    elog(WARNING, "could not compile module '%s': %s", module_name, err);

    JS_FreeCString(ctx, err);
    JS_FreeValue(ctx, ex);

    return JS_EXCEPTION;
  }

  if (JS_ResolveModule(ctx, res) < 0) {
    JS_FreeValue(ctx, res);
    elog(WARNING, "unable to resolve module %s", module_name);

    return JS_EXCEPTION;
  }

  JSValue mod_res = JS_EvalFunction(ctx, JS_DupValue(ctx, res));
  mod_res = js_std_await(ctx, mod_res);

  if (JS_IsException(mod_res)) {
    JS_FreeValue(ctx, res);
    js_std_dump_error(ctx);
    return JS_EXCEPTION;
  }
  JS_FreeValue(ctx, mod_res);

  /* get namespace from the *compiled* module object, not the eval result */
  JSModuleDef *m = JS_VALUE_GET_PTR(res);
  JS_FreeValue(ctx, res);
  JSValue ns = JS_GetModuleNamespace(ctx, m);
  return ns;
}
