#include "postgres.h"

#include "access/xlog_internal.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "deps/quickjs/quickjs.h"

#include "pljs.h"

// helper functions that should really exist as part of quickjs.
static JSClassID JS_CLASS_OBJECT = 1;
static JSClassID JS_CLASS_ARRAY_BUFFER = 19;
static JSClassID JS_CLASS_SHARED_ARRAY_BUFFER = 20;
static JSClassID JS_CLASS_UINT8C_ARRAY = 21;
static JSClassID JS_CLASS_INT8_ARRAY = 22;
static JSClassID JS_CLASS_UINT8_ARRAY = 23;
static JSClassID JS_CLASS_INT16_ARRAY = 24;
static JSClassID JS_CLASS_UINT16_ARRAY = 25;
static JSClassID JS_CLASS_INT32_ARRAY = 26;
static JSClassID JS_CLASS_UINT32_ARRAY = 27;

inline static bool Is_ArrayType(JSValueConst obj, JSClassID class_id) {
  return NULL != JS_GetOpaque(obj, class_id);
}

// if given object is array buffer.
inline static bool Is_ArrayBuffer(JSValueConst obj) {
  return NULL != JS_GetOpaque(obj, JS_CLASS_ARRAY_BUFFER);
}

// if given object is shared array buffer.
inline static bool Is_SharedArrayBuffer(JSValueConst obj) {
  return NULL != JS_GetOpaque(obj, JS_CLASS_SHARED_ARRAY_BUFFER);
}

// if this is an actual object of any sort.
inline static bool Is_Object(JSValueConst obj) {
  return NULL != JS_GetOpaque(obj, JS_CLASS_OBJECT);
}

// allocate memory and copy data from the varlena text representation.
static char *dup_pgtext(text *what) {
  size_t len = VARSIZE(what) - VARHDRSZ;
  char *dup = palloc(len + 1);

  memcpy(dup, VARDATA(what), len);
  dup[len] = 0;

  return dup;
}

static const char *spi_status_string(int status) {
  static char private_buf[1024];

  if (status > 0)
    return "OK";

  switch (status) {
  case SPI_ERROR_CONNECT:
    return "SPI_ERROR_CONNECT";
  case SPI_ERROR_COPY:
    return "SPI_ERROR_COPY";
  case SPI_ERROR_OPUNKNOWN:
    return "SPI_ERROR_OPUNKNOWN";
  case SPI_ERROR_UNCONNECTED:
  case SPI_ERROR_TRANSACTION:
    return "current transaction is aborted, "
           "commands ignored until end of transaction block";
  case SPI_ERROR_CURSOR:
    return "SPI_ERROR_CURSOR";
  case SPI_ERROR_ARGUMENT:
    return "SPI_ERROR_ARGUMENT";
  case SPI_ERROR_PARAM:
    return "SPI_ERROR_PARAM";
  case SPI_ERROR_NOATTRIBUTE:
    return "SPI_ERROR_NOATTRIBUTE";
  case SPI_ERROR_NOOUTFUNC:
    return "SPI_ERROR_NOOUTFUNC";
  case SPI_ERROR_TYPUNKNOWN:
    return "SPI_ERROR_TYPUNKNOWN";
  default:
    snprintf(private_buf, sizeof(private_buf), "SPI_ERROR: %d", status);
    return private_buf;
  }
}

uint32_t js_array_length(JSContext *ctx, JSValueConst obj) {
  JSValue length = JS_GetPropertyStr(ctx, obj, "length");
  int32_t array_length_int;
  JS_ToInt32(ctx, &array_length_int, length);

  return array_length_int;
}

// convert an Oid a pljs_type.
void pljs_type_fill(pljs_type *type, Oid typid) {
  bool is_preferred;

  type->typid = typid;
  type->fn_input.fn_mcxt = CurrentMemoryContext;
  type->fn_output.fn_mcxt = CurrentMemoryContext;

  get_type_category_preferred(typid, &type->category, &is_preferred);

  type->is_composite = (type->category == TYPCATEGORY_COMPOSITE);

  get_typlenbyvalalign(typid, &type->len, &type->byval, &type->align);

  if (type->category == TYPCATEGORY_ARRAY) {
    Oid elemid = get_element_type(typid);

    if (elemid == InvalidOid) {
      ereport(ERROR,
              (errmsg("cannot determine element type of array: %u", typid)));
    }

    type->typid = elemid;
    type->is_composite = (TypeCategory(elemid) == TYPCATEGORY_COMPOSITE);
    get_typlenbyvalalign(type->typid, &type->len, &type->byval, &type->align);
  }
}

JSValue pljs_datum_to_object(Datum arg, pljs_type *type, JSContext *ctx) {
  JSValue obj;

  HeapTupleHeader rec = DatumGetHeapTupleHeader(arg);
  Oid tupType;
  int32 tupTypmod;
  TupleDesc tupdesc = NULL;
  HeapTupleData tuple;

  PG_TRY();
  {
    /* Extract type info from the tuple itself. */
    tupType = HeapTupleHeaderGetTypeId(rec);
    tupTypmod = HeapTupleHeaderGetTypMod(rec);
    tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
  }
  PG_CATCH();
  { elog(WARNING, "caught error"); }
  PG_END_TRY();

  obj = JS_NewObject(ctx);

  if (tupdesc) {
    for (int16 i = 0; i < tupdesc->natts; i++) {
      Datum datum;
      bool isnull = false;

      if (TupleDescAttr(tupdesc, i)->attisdropped) {
        continue;
      }

      char *colname = NameStr(TupleDescAttr(tupdesc, i)->attname);
      tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
      ItemPointerSetInvalid(&(tuple.t_self));
      tuple.t_tableOid = InvalidOid;
      tuple.t_data = rec;

      datum = heap_getattr(&tuple, i + 1, tupdesc, &isnull);

      if (isnull) {
        JS_SetPropertyStr(ctx, obj, colname, JS_NULL);
      } else {
        JS_SetPropertyStr(
            ctx, obj, colname,
            pljs_datum_to_jsvalue(datum, tupdesc->attrs[i].atttypid, ctx));
      }
    }

    ReleaseTupleDesc(tupdesc);
  }

  return obj;
}

// convert a datum to a quickjs array.
JSValue pljs_datum_to_array(Datum arg, pljs_type *type, JSContext *ctx) {
  JSValue array = JS_NewArray(ctx);
  Datum *values;
  bool *nulls;
  int nelems;

  deconstruct_array(DatumGetArrayTypeP(arg), type->typid, type->len,
                    type->byval, type->align, &values, &nulls, &nelems);

  for (int i = 0; i < nelems; i++) {
    JSValue value =
        nulls[i] ? JS_NULL : pljs_datum_to_jsvalue(values[i], type->typid, ctx);

    JS_SetPropertyUint32(ctx, array, i, value);
  }

  JSValue length = JS_NewInt32(ctx, nelems);
  JS_SetPropertyStr(ctx, array, "length", length);

  pfree(values);
  pfree(nulls);

  return array;
}

// convert a value to a quickjs value.
JSValue pljs_datum_to_jsvalue(Datum arg, Oid argtype, JSContext *ctx) {
  JSValue return_result;
  char *str;
  Jsonb *jb;

  pljs_type type;
  pljs_type_fill(&type, argtype);

  if (type.category == TYPCATEGORY_ARRAY) {
    return pljs_datum_to_array(arg, &type, ctx);
  }

  if (type.is_composite) {
    return pljs_datum_to_object(arg, &type, ctx);
  }

  switch (type.typid) {
  case OIDOID:
    return_result = JS_NewInt64(ctx, arg);
    break;

  case BOOLOID:
    return_result = JS_NewBool(ctx, DatumGetBool(arg));
    break;

  case INT2OID:
    return_result = JS_NewInt32(ctx, DatumGetInt16(arg));
    break;

  case INT4OID:
    return_result = JS_NewInt32(ctx, DatumGetInt32(arg));
    break;

  case INT8OID:
    return_result = JS_NewInt64(ctx, DatumGetInt64(arg));
    break;

  case FLOAT4OID:
    return_result = JS_NewFloat64(ctx, DatumGetFloat4(arg));
    break;

  case FLOAT8OID:
    return_result = JS_NewFloat64(ctx, DatumGetFloat8(arg));
    break;

  case NUMERICOID:
    return_result = JS_NewFloat64(
        ctx, DatumGetFloat8(DirectFunctionCall1(numeric_float8, arg)));
    break;

  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID:
  case XMLOID:
    // get a copy of the string.
    str = dup_pgtext(DatumGetTextP(arg));

    return_result = JS_NewString(ctx, str);

    // free the memory allocated.
    pfree(str);
    break;

  case NAMEOID:
    return_result = JS_NewString(ctx, DatumGetName(arg)->data);
    break;

  case JSONOID:
    // get a copy of the string.
    str = dup_pgtext(DatumGetTextP(arg));

    return_result = JS_ParseJSON(ctx, str, strlen(str), NULL);

    // free the memory allocated.
    pfree(str);
    break;

  case JSONBOID:
    // get the datum
    jb = DatumGetJsonbP(arg);

    // convert it to a string (takes some casting, but JsonbContainer is also
    // a varlena).
    str = JsonbToCString(NULL, (JsonbContainer *)VARDATA(jb), VARSIZE(jb));

    return_result = JS_ParseJSON(ctx, str, strlen(str), NULL);

    // free the memory allocated.
    pfree(str);
    break;

  case BYTEAOID: {
    uint8_t g[5] = {1, 2, 3, 4, 5};
    void *p = PG_DETOAST_DATUM_COPY(arg);
    char *buf = palloc(VARSIZE_ANY_EXHDR(p) + 1);
    memcpy(buf, VARDATA(p), VARSIZE_ANY_EXHDR(p));

    // buf[VARSIZE_ANY_EXHDR(p)] = '\0';
    return_result = JS_NewStringLen(ctx, buf, VARSIZE_ANY_EXHDR(p));
    pfree(buf);
    break;
  }

  default:
    elog(NOTICE, "Unknown type: %d", argtype);
    return_result = JS_NULL;
  }

  return return_result;
}

// convert a quickjs array to a datum array.
Datum pljs_jsvalue_to_array(JSValue val, pljs_type *type, JSContext *ctx,
                            FunctionCallInfo fcinfo) {
  ArrayType *result;
  Datum *values;
  bool *nulls;
  int ndims[1];
  int lbs[] = {[0] = 1};

  int32_t array_length = js_array_length(ctx, val);

  values = (Datum *)palloc(sizeof(Datum) * array_length);
  nulls = (bool *)palloc(sizeof(bool) * array_length);
  memset(nulls, 0, sizeof(bool) * array_length);

  ndims[0] = array_length;

  for (int i = 0; i < array_length; i++) {
    JSValue elem = JS_GetPropertyUint32(ctx, val, i);

    if (JS_IsNull(elem)) {
      nulls[i] = true;
    } else {
      values[i] =
          pljs_jsvalue_to_datum(elem, type->typid, ctx, fcinfo, &nulls[i]);
    }
  }

  result = construct_md_array(values, nulls, 1, ndims, lbs, type->typid,
                              type->len, type->byval, type->align);
  pfree(values);
  pfree(nulls);

  return PointerGetDatum(result);
}

Datum pljs_jsvalue_to_record(JSValue val, pljs_type *type, JSContext *ctx,
                             bool *is_null) {
  TupleDesc tupdesc = NULL;
  Datum result = 0;
  Oid rettype = type->typid;

  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    *is_null = true;
    return (Datum)0;
  }

  PG_TRY();
  { tupdesc = lookup_rowtype_tupdesc(rettype, -1); }
  PG_CATCH();
  { elog(WARNING, "in catch"); }
  PG_END_TRY();

  if (tupdesc != NULL) {
    Datum *values = (Datum *)palloc(sizeof(Datum) * tupdesc->natts);
    bool *nulls = (bool *)palloc(sizeof(bool) * tupdesc->natts);
    memset(nulls, 0, sizeof(bool) * tupdesc->natts);

    for (int16 c = 0; c < tupdesc->natts; c++) {
      if (TupleDescAttr(tupdesc, c)->attisdropped) {
        nulls[c] = true;
        continue;
      }

      char *colname = NameStr(TupleDescAttr(tupdesc, c)->attname);
      JSValue o = JS_GetPropertyStr(ctx, val, colname);

      if (JS_IsNull(o) || JS_IsUndefined(o)) {
        nulls[c] = true;
        continue;
      }

      values[c] = pljs_jsvalue_to_datum(o, tupdesc->attrs[c].atttypid, ctx,
                                        NULL, &nulls[c]);
    }

    result = HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls));
    ReleaseTupleDesc(tupdesc);
  }

  return result;
}

// Convert a quickjs value to a datum.
Datum pljs_jsvalue_to_datum(JSValue val, Oid rettype, JSContext *ctx,
                            FunctionCallInfo fcinfo, bool *isnull) {

  pljs_type type;

  pljs_type_fill(&type, rettype);

  if (type.typid != JSONOID && type.typid != JSONBOID && JS_IsArray(ctx, val)) {
    return pljs_jsvalue_to_array(val, &type, ctx, fcinfo);
  }

  if (type.is_composite) {
    return pljs_jsvalue_to_record(val, &type, ctx, isnull);
  }

  if (JS_VALUE_GET_TAG(val) == JS_TAG_NULL) {
    if (fcinfo) {
      PG_RETURN_NULL();
    } else {
      if (isnull) {
        *isnull = true;
      }

      return (Datum)0;
    }
  }

  switch (rettype) {
  case VOIDOID:
    PG_RETURN_VOID();
    break;

  case OIDOID: {
    int64_t in;
    JS_ToInt64(ctx, &in, val);

    PG_RETURN_OID(in);
    break;
  }

  case BOOLOID: {
    int8_t in = JS_ToBool(ctx, val);
    PG_RETURN_BOOL(in);
    break;
  }

  case INT2OID: {
    int32_t in;
    JS_ToInt32(ctx, &in, val);

    PG_RETURN_INT16((int16_t)in);
    break;
  }

  case INT4OID: {
    int32_t in;
    JS_ToInt32(ctx, &in, val);

    PG_RETURN_INT32(in);
    break;
  }

  case INT8OID: {
    int64_t in;
    JS_ToInt64(ctx, &in, val);

    PG_RETURN_INT64(in);
    break;
  }

  case FLOAT4OID: {
    double in;
    JS_ToFloat64(ctx, &in, val);

    PG_RETURN_FLOAT4((float4)in);
    break;
  }

  case FLOAT8OID: {
    double in;
    JS_ToFloat64(ctx, &in, val);

    PG_RETURN_FLOAT8(in);
    break;
  }

  case NUMERICOID: {
    double in;
    JS_ToFloat64(ctx, &in, val);

    return DirectFunctionCall1(float8_numeric, Float8GetDatum((float8)in));
    break;
  }

  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID:
  case NAMEOID:
  case XMLOID: {
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, val);

    text *t = (text *)palloc(plen + VARHDRSZ);
    SET_VARSIZE(t, plen + VARHDRSZ);
    memcpy(VARDATA(t), str, plen);
    JS_FreeCString(ctx, str);

    PG_RETURN_TEXT_P(t);
    break;
  }

  case JSONOID: {
    JSValueConst *argv = &val;
    JSValue js = JS_JSONStringify(ctx, argv[0], argv[1], argv[2]);
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, js);

    // return it as a CStringTextDatum.
    Datum ret = CStringGetTextDatum(str);

    JS_FreeCString(ctx, str);

    return ret;
    break;
  }

  case JSONBOID: {
    JSValueConst *argv = &val;
    JSValue js = JS_JSONStringify(ctx, argv[0], argv[1], argv[2]);
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, js);

    // return it as a Datum, since there is no direct CStringGetJsonb exposed.
    Datum ret = (Datum)DatumGetJsonbP(
        DirectFunctionCall1(jsonb_in, (Datum)(char *)str));

    JS_FreeCString(ctx, str);

    return ret;
    break;
  }

  case BYTEAOID: {
    size_t psize;
    size_t pbyte_offset = 0;
    size_t pbyte_length = 0;
    size_t pbytes_per_element = 0;

    uint8_t *buffer;

    uint32_t length = js_array_length(ctx, val);

    if (Is_ArrayType(val, JS_CLASS_UINT8_ARRAY) ||
        Is_ArrayType(val, JS_CLASS_INT8_ARRAY)) {
      pbytes_per_element = 1;
      psize = pbytes_per_element * length;
      uint8_t *array_copy = palloc(pbytes_per_element * length);

      for (size_t i = 0; i < length; i++) {
        int32_t in;

        JSValue jsval = JS_GetPropertyUint32(ctx, val, i);
        JS_ToInt32(ctx, &in, jsval);
        array_copy[i] = (uint8_t)in;
      }

      buffer = palloc(VARHDRSZ + psize);

      SET_VARSIZE(buffer, psize + VARHDRSZ);
      memcpy(VARDATA(buffer), array_copy, psize);

      pfree(array_copy);

      return PointerGetDatum(buffer);
    } else if (Is_ArrayType(val, JS_CLASS_UINT16_ARRAY) ||
               Is_ArrayType(val, JS_CLASS_INT16_ARRAY)) {
      pbytes_per_element = 2;
      psize = pbytes_per_element * length;
      uint16_t *array_copy = palloc(pbytes_per_element * length);

      for (size_t i = 0; i < length; i++) {
        int32_t in;

        JSValue jsval = JS_GetPropertyUint32(ctx, val, i);
        JS_ToInt32(ctx, &in, jsval);
        array_copy[i] = (uint16_t)in;
      }

      buffer = palloc(VARHDRSZ + psize);

      SET_VARSIZE(buffer, psize + VARHDRSZ);
      memcpy(VARDATA(buffer), array_copy, psize);

      pfree(array_copy);

      return PointerGetDatum(buffer);
    } else if (Is_ArrayType(val, JS_CLASS_UINT32_ARRAY) ||
               Is_ArrayType(val, JS_CLASS_INT32_ARRAY)) {
      pbytes_per_element = 4;
      psize = pbytes_per_element * length;
      uint32_t *array_copy = palloc(pbytes_per_element * length);

      for (size_t i = 0; i < pbytes_per_element * length; i++) {
        int32_t in;

        JSValue jsval = JS_GetPropertyUint32(ctx, val, i);
        JS_ToInt32(ctx, &in, jsval);
        array_copy[i] = (uint32_t)in;
      }

      buffer = palloc(VARHDRSZ + psize);

      SET_VARSIZE(buffer, psize + VARHDRSZ);
      memcpy(VARDATA(buffer), array_copy, psize);

      pfree(array_copy);

      return PointerGetDatum(buffer);

    } else if (Is_ArrayBuffer(val)) {
      uint8_t *array_copy = JS_GetArrayBuffer(ctx, &psize, val);
      buffer = palloc(VARHDRSZ + psize);

      SET_VARSIZE(buffer, psize + VARHDRSZ);
      memcpy(VARDATA(buffer), array_copy, psize);
      return PointerGetDatum(buffer);
    } else if (JS_IsString(val)) {
      size_t str_length;
      const char *str = JS_ToCStringLen(ctx, &str_length, val);

      buffer = palloc(str_length + VARHDRSZ);
      SET_VARSIZE(buffer, str_length + VARHDRSZ);
      memcpy(VARDATA(buffer), str, str_length);

      JS_FreeCString(ctx, str);

      return PointerGetDatum(buffer);
    } else {
      elog(NOTICE, "Unknown array type, tag: %lld", val.tag);
      for (uint8_t i = 0; i < 255; i++) {
        void *res = JS_GetOpaque(val, i);
        if (res != NULL) {
          elog(NOTICE, "class_id: %d", i);
        }
      }

      PG_RETURN_NULL();
    }
  }

  default:
    elog(NOTICE, "Unknown type: %d", rettype);
    if (fcinfo) {
      PG_RETURN_NULL();
    } else {
      if (isnull) {
        *isnull = true;
      }
      return (Datum)0;
    }
  }

  // shut up, compiler
  PG_RETURN_VOID();
}

JSValue values_to_array(JSContext *ctx, JSValue *array, int argc, int start) {
  JSValue ret = JS_NewArray(ctx);

  uint32_t current = 0;
  for (int i = start; i < argc; i++) {
    JS_SetPropertyUint32(ctx, ret, current, array[i]);
    current++;
  }

  return ret;
}

JSValue tuple_to_jsvalue(JSContext *ctx, TupleDesc tuple,
                         HeapTuple heap_tuple) {
  JSValue obj = JS_NewObject(ctx);

  for (int i = 0; i < tuple->natts; i++) {
    FormData_pg_attribute *tuple_attrs = TupleDescAttr(tuple, i);
    if (tuple_attrs->attisdropped) {
      continue;
    }

    bool isnull;
    Datum datum = heap_getattr(heap_tuple, i + 1, tuple, &isnull);
    char *name = NameStr(tuple_attrs->attname);
    JS_SetPropertyStr(ctx, obj, name,
                      pljs_datum_to_jsvalue(datum, tuple_attrs->atttypid, ctx));
  }

  return obj;
}

JSValue spi_result_to_jsvalue(JSContext *ctx, int status) {
  JSValue result;

  if (status < 0) {
    return js_throw(ctx, spi_status_string(status));
  }

  switch (status) {
  case SPI_OK_UTILITY:
  case SPI_OK_REWRITTEN: {
    if (SPI_tuptable == NULL) {
      result = JS_NewInt32(ctx, SPI_processed);
      break;
    }
    // will fallthrough here to the "SELECT" logic below
  }
  case SPI_OK_SELECT:
  case SPI_OK_INSERT_RETURNING:
  case SPI_OK_DELETE_RETURNING:
  case SPI_OK_UPDATE_RETURNING: {
    int nrows = SPI_processed;
    TupleDesc tupdesc;
    bool m_is_scalar;
    int natts;
    MemoryContext m_memory_context;

    tupdesc = SPI_tuptable->tupdesc;

    JSValue obj = JS_NewArray(ctx);

    for (int r = 0; r < nrows; r++) {
      JSValue value = tuple_to_jsvalue(ctx, tupdesc, SPI_tuptable->vals[r]);

      JS_SetPropertyUint32(ctx, obj, r, value);
    }

    result = obj;
    break;
  }
  default:
    result = JS_NewInt32(ctx, SPI_processed);
    break;
  }

  return result;
}
