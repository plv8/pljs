#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

#include "deps/quickjs/quickjs.h"

#include "pljs.h"
#include "varatt.h"

#include <string.h>

// Helper functions that should really exist as part of quickjs.
static JSClassID JS_CLASS_OBJECT = 1;
static JSClassID JS_CLASS_DATE = 10;
static JSClassID JS_CLASS_ARRAY_BUFFER = 19;
static JSClassID JS_CLASS_SHARED_ARRAY_BUFFER = 20;
static JSClassID JS_CLASS_UINT8C_ARRAY = 21;
static JSClassID JS_CLASS_INT8_ARRAY = 22;
static JSClassID JS_CLASS_UINT8_ARRAY = 23;
static JSClassID JS_CLASS_INT16_ARRAY = 24;
static JSClassID JS_CLASS_UINT16_ARRAY = 25;
static JSClassID JS_CLASS_INT32_ARRAY = 26;
static JSClassID JS_CLASS_UINT32_ARRAY = 27;

/**
 * Struct containing the type information for a catch-all*/
// if given object is an array.
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

// if given object is shared array buffer.
inline static bool Is_Date(JSValueConst obj) {
  return NULL != JS_GetOpaque(obj, JS_CLASS_DATE);
}

#if JSONB_DIRECT_CONVERSION
static JSValue convert_jsonb(JsonbContainer *in, JSContext *ctx);
static JSValue get_jsonb_value(JsonbValue *scalarVal, JSContext *ctx);
static Jsonb *convert_object(JSValue object, JSContext *ctx);
#endif

/**
 * @brief Converts a Javascript epoch to a Datum.
 *
 * @param @c double Javascript epoch
 * @returns #Datum of type `DATEADT`
 */
static Datum epoch_to_date(double epoch) {
  epoch -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;

#ifdef HAVE_INT64_TIMESTAMP
  epoch = (epoch * 1000) / USECS_PER_DAY;
#else
  epoch = (epoch / 1000) / SECS_PER_DAY;
#endif
  PG_RETURN_DATEADT((DateADT)epoch);
}

/**
 * @brief Converts a Javascript epoch to a Datum.
 *
 * @param @c double Javascript epoch
 * @returns #Datum of a timestamptz
 */
static Datum epoch_to_timestamptz(double epoch) {
  epoch -= (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;

#ifdef HAVE_INT64_TIMESTAMP
  return Int64GetDatum((int64)epoch * 1000);
#else
  return Float8GetDatum(epoch / 1000.0);
#endif
}

/**
 * @brief Converts a `DateADT` Datum to a Javascript epoch.
 *
 * @param #Datum of type `DateADT`
 * @returns @c double Javascript epoch
 */
static double date_to_epoch(DateADT date) {
  double epoch;

#ifdef HAVE_INT64_TIMESTAMP
  epoch = (double)date * USECS_PER_DAY / 1000.0;
#else
  epoch = (double)date * SECS_PER_DAY * 1000.0;
#endif

  return epoch + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;
}

/**
 * @brief Converts a `TimestampTz` Datum to a Javascript epoch.
 *
 * @param #Datum of type `TimestampTz`
 * @returns @c double Javascript epoch
 */
static double timestamptz_to_epoch(TimestampTz tm) {
  double epoch;

#ifdef HAVE_INT64_TIMESTAMP
  epoch = (double)tm / 1000.0;
#else
  epoch = (double)tm * 1000.0;
#endif

  return epoch + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;
}

/**
 * @brief Makes a copy of a #text type from Postgres and returns a `cstring`.
 *
 * Takes the input of a Postgres `TEXT` field, allocates memory in the
 * current memory context, and returns a `\0` terminated copy of the string
 * that was stored.  It is up to the caller to free the memory allocated.
 *
 * @param what #text - string to duplicate
 * @returns @c char * copy of the text field
 */
static char *dup_pgtext(text *what) {
  size_t len = VARSIZE(what) - VARHDRSZ;
  char *dup = palloc(len + 1);

  memcpy(dup, VARDATA(what), len);
  dup[len] = 0;

  return dup;
}

/**
 * @brief Converts an SPI status into static text.
 */
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

/**
 * @brief Helper for getting the length of a Javascript array.
 *
 * @param obj JSValueConst - Javascript array to check the length of
 * @param ctx #JSContext - Javascript context to execute in
 * @returns @c uint32_t
 */
uint32_t pljs_js_array_length(JSValueConst obj, JSContext *ctx) {
  JSValue length = JS_GetPropertyStr(ctx, obj, "length");
  int32_t array_length_int;
  JS_ToInt32(ctx, &array_length_int, length);

  return array_length_int;
}

/**
 * @brief Converts an `Oid` into `pljs_type`.
 *
 * Takes an input of a pointer to `pljs_type` and an `Oid`,
 * and queries Postgres for enough information for type conversions
 * between Postgres and Javascript.
 *
 * @param type #pljs_type - the location to store the type data
 * @param typid #Oid - the Postgres type to decode
 */
void pljs_type_fill(pljs_type *type, Oid typid) {
  bool is_preferred;
  type->typid = typid;

  get_type_category_preferred(typid, &type->category, &is_preferred);

  type->is_composite = (type->category == TYPCATEGORY_COMPOSITE);

  get_typlenbyvalalign(typid, &type->length, &type->byval, &type->align);

  if (type->category == TYPCATEGORY_ARRAY) {
    Oid elemid = get_element_type(typid);

    if (elemid == InvalidOid) {
      ereport(ERROR,
              (errmsg("cannot determine element type of array: %u", typid)));
    }

    type->typid = elemid;
    type->is_composite = (TypeCategory(elemid) == TYPCATEGORY_COMPOSITE);
    get_typlenbyvalalign(type->typid, &type->length, &type->byval,
                         &type->align);
  } else if (type->category == TYPCATEGORY_PSEUDOTYPE) {
    type->is_composite = true;
  }
}

/**
 * @brief Converts a #Datum for a Javascript object.
 *
 * Takes a #Datum and converts it to a Javascript object.  If there is
 * an error, throws a Javascript exception.
 *
 * @param arg #Datum - value to convert
 * @param type #pljs_type - type information of the #Datum
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the object or thrown exception in case of error
 */
JSValue pljs_datum_to_object(Datum arg, pljs_type *type, JSContext *ctx) {
  if (arg == 0) {
    return JS_UNDEFINED;
  }

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
  {
    ErrorData *edata = CopyErrorData();
    JSValue error = js_throw(edata->message, ctx);
    FlushErrorState();
    FreeErrorData(edata);

    return error;
  }
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
            pljs_datum_to_jsvalue(datum, TupleDescAttr(tupdesc, i)->atttypid,
                                  ctx, false));
      }
    }

    ReleaseTupleDesc(tupdesc);
  }

  return obj;
}

/**
 * @brief Converts a Postgres array to a Javascript array.
 *
 * Takes a Postgres #Datum and type and converts it into a Javascript
 * array.  All properties are set, including array length.
 *
 * @param arg #Datum - Postgres array to convert
 * @oaram type #pljs_type - type information for the array
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the array
 */
JSValue pljs_datum_to_array(Datum arg, pljs_type *type, JSContext *ctx) {
  JSValue array = JS_NewArray(ctx);
  Datum *values;
  bool *nulls;
  int nelems;

  deconstruct_array(DatumGetArrayTypeP(arg), type->typid, type->length,
                    type->byval, type->align, &values, &nulls, &nelems);

  for (int i = 0; i < nelems; i++) {
    JSValue value =
        nulls[i] ? JS_NULL
                 : pljs_datum_to_jsvalue(values[i], type->typid, ctx, false);

    JS_SetPropertyUint32(ctx, array, i, value);
  }

  JSValue length = JS_NewInt32(ctx, nelems);
  JS_SetPropertyStr(ctx, array, "length", length);

  pfree(values);
  pfree(nulls);

  return array;
}

/**
 * @brief Default type conversion from @Datum to @JSValue.
 *
 * If a type is unknown, instead of returning `undefined` or `NULL`, do a
 * `cstring` or `varlena`, or value based conversion to an `String` or `Int32`
 * and back.
 *
 * In the case of fixed-length `INTERNALLENGTH`, it will be a `String`
 * with those bytes copied into the string, and the length set appropriately.
 * When the `String` is variable, the length would be taken from the `varlena`
 * header, and the rest copied into the string.
 *
 * @param arg #Datum - Postgres datum to convert
 * @param type #pljs_type - type of the datum
 * @param ctx #JSContext - Javascript context
 * @returns #JSValue conversion of the Datum
 */
static JSValue pljs_datum_to_jsvalue_default(Datum arg, pljs_type type,
                                             JSContext *ctx) {
  JSValue ret = JS_UNDEFINED;

  if (type.byval) {
    ret = JS_NewInt32(ctx, arg);
  } else {
    // If this is a variable length type, make a copy of it.
    if (type.length == -1) {
      ret = JS_NewStringLen(ctx, (char *)VARDATA(arg), VARSIZE_ANY_EXHDR(arg));
      JS_SetPropertyStr(ctx, ret, "length",
                        JS_NewInt32(ctx, VARSIZE_ANY_EXHDR(arg)));
    } else {
      ret = JS_NewStringLen(ctx, (char *)arg, type.length);
      JS_SetPropertyStr(ctx, ret, "length", JS_NewInt32(ctx, type.length));
    }
  }

  return ret;
}

/**
 * @brief Converts a Postgres #Datum to a Javascript value.
 *
 * Takes a Postgres #Datum and type and converts it into a Javascript
 * value.  If the type is an array or is composite, then call out to
 * the correct functions.  If `skip_composite` is true, then the value
 * is directly converted, even if it is composite.  There is currently
 * only one case for this: conversion from a window function.
 *
 * @param arg #Datum - Postgres array to convert
 * @oaram type #pljs_type - type information for the type
 * @param ctx #JSContext - Javascript context to execute in
 * @param skip_composite @c bool - whether to skip the composite check
 * @returns #JSValue of the value, or JS_NULL if unable to convert
 */
JSValue pljs_datum_to_jsvalue(Datum arg, Oid argtype, JSContext *ctx,
                              bool skip_composite) {
  JSValue return_result;
  char *str;

  pljs_type type;
  pljs_type_fill(&type, argtype);

  if (type.category == TYPCATEGORY_ARRAY) {
    return pljs_datum_to_array(arg, &type, ctx);
  }

  if (!skip_composite && type.is_composite) {
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
    return_result = JS_NewBigInt64(ctx, DatumGetInt64(arg));
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
    // Get a copy of the string.
    str = dup_pgtext(DatumGetTextP(arg));

    return_result = JS_NewString(ctx, str);

    // Free the memory allocated.
    pfree(str);
    break;

  case NAMEOID:
    return_result = JS_NewString(ctx, DatumGetName(arg)->data);
    break;

  case JSONOID:
    // Get a copy of the string.
    str = dup_pgtext(DatumGetTextP(arg));

    return_result = JS_ParseJSON(ctx, str, strlen(str), NULL);

    // free the memory allocated.
    pfree(str);
    break;

  case JSONBOID: {
#if JSONB_DIRECT_CONVERSION
    Jsonb *jsonb = (Jsonb *)PG_DETOAST_DATUM(arg);

    if (JB_ROOT_IS_SCALAR(jsonb)) {
      JsonbValue jb;
      JsonbExtractScalar(&jsonb->root, &jb);
      return_result = get_jsonb_value(&jb, ctx);
    } else {
      return_result = convert_jsonb(&jsonb->root, ctx);
    }
#else
    // Get the datum.
    Jsonb *jb = DatumGetJsonbP(arg);

    // Convert it to a string (takes some casting, but JsonbContainer is also
    // a varlena).
    str = JsonbToCString(NULL, (JsonbContainer *)VARDATA(jb), VARSIZE(jb));

    return_result = JS_ParseJSON(ctx, str, strlen(str), NULL);

    // Free the memory allocated.
    pfree(str);
#endif
    break;
  }

  case BYTEAOID: {
    void *p = PG_DETOAST_DATUM_COPY(arg);
    char *buf = palloc(VARSIZE_ANY_EXHDR(p) + 1);

    memcpy(buf, VARDATA(p), VARSIZE_ANY_EXHDR(p));

    return_result = JS_NewStringLen(ctx, buf, VARSIZE_ANY_EXHDR(p));
    pfree(buf);
    break;
  }

  case DATEOID:
    return_result = JS_NewDate(ctx, date_to_epoch(DatumGetDateADT(arg)));
    break;
  case TIMESTAMPOID:
  case TIMESTAMPTZOID:
    return_result =
        JS_NewDate(ctx, timestamptz_to_epoch(DatumGetTimestampTz(arg)));
    break;

  default:
    return_result = pljs_datum_to_jsvalue_default(arg, type, ctx);
  }

  return return_result;
}

/**
 * @brief Converts a Javascript array to a Postgres array.
 *
 * Takes a Javascript #JSValue of an array and type and converts
 * it into a Postgres array.
 *
 * @param val #JSValue - Javascript array to convert
 * @oaram type #pljs_type - type information for the array
 * @param ctx #JSContext - Javascript context to execute in
 * @param fcinfo #FunctionCallInfo - needed to conversion back to a #Datum
 * @returns #Datum of the array
 */
Datum pljs_jsvalue_to_array(JSValue val, pljs_type *type, JSContext *ctx,
                            FunctionCallInfo fcinfo) {
  ArrayType *result;
  Datum *values;
  bool *nulls;
  int ndims[1];
  int lbs[] = {[0] = 1};

  int32_t array_length = pljs_js_array_length(val, ctx);

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
                              type->length, type->byval, type->align);
  pfree(values);
  pfree(nulls);

  return PointerGetDatum(result);
}

/**
 * @brief Determines whether a Javascript object contains all of the
 * column names.
 *
 * Takes a Javascript #JSValue object and the possible column names
 * and determines whether all of the column names are reflected in
 * the object.
 *
 * @param val #JSValue - Javascript object to check
 * @param ctx #JSContext - Javascript context to execute in
 * @oaram tupdesc #TupleDesc
 * @returns @c bool
 */
bool pljs_jsvalue_object_contains_all_column_names(JSValue val, JSContext *ctx,
                                                   TupleDesc tupdesc) {
  uint32_t object_keys_length = 0;
  JSPropertyEnum *tab;

  if (JS_GetOwnPropertyNames(ctx, &tab, &object_keys_length, val,
                             JS_GPN_STRING_MASK) < 0) {
    return false;
  }

  for (int16 c = 0; c < tupdesc->natts; c++) {
    if (TupleDescAttr(tupdesc, c)->attisdropped) {
      continue;
    }

    char *colname = NameStr(TupleDescAttr(tupdesc, c)->attname);

    // Check to see if the key exists in the object
    bool found = false;
    for (uint32_t object_key = 0; object_key < object_keys_length;
         object_key++) {
      const char *atom = JS_AtomToCString(ctx, tab[object_key].atom);

      if (strcmp(colname, atom) == 0) {
        found = true;
        JS_FreeCString(ctx, atom);
        break;
      }

      JS_FreeCString(ctx, atom);
    }

    if (!found) {
      return false;
    }
  }

  return true;
}

/**
 * @brief Converts a Javascript object into a Postgres record.
 *
 * Takes a Javascript object and converts it into a Postgres
 * record (composite Postgres type).
 *
 * @param val #JSValue - the Javascript object to convert
 * @oaram type #pljs_type - type information for the record
 * @param ctx #JSContext - Javascript context to execute in
 * @param is_null @c bool - pointer to fill of whether the record is null
 * @param tupdesc #TupleDesc - can be `NULL`
 * @param tupstore #Tuplestorestate
 * @returns #Datum of the Postgres record
 */
Datum pljs_jsvalue_to_record(JSValue val, pljs_type *type, JSContext *ctx,
                             bool *is_null, TupleDesc tupdesc,
                             Tuplestorestate *tupstore) {
  Datum result = 0;

  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    *is_null = true;
    return (Datum)0;
  }

  bool cleanup_tupdesc = false;
  PG_TRY();
  {
    if (tupdesc == NULL) {
      Oid rettype = type->typid;

      tupdesc = lookup_rowtype_tupdesc(rettype, -1);
      cleanup_tupdesc = true;
    }
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
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

      values[c] = pljs_jsvalue_to_datum(o, TupleDescAttr(tupdesc, c)->atttypid,
                                        ctx, NULL, &nulls[c]);
    }

    if (tupstore != NULL) {
      result = (Datum)0;
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    } else {
      result = HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls));
    }

    if (cleanup_tupdesc) {
      ReleaseTupleDesc(tupdesc);
    }
  }

  return result;
}

/**
 * @brief Default type conversion from @JSValue to @Datum.
 *
 * If a type is unknown, instead of returning `NULL`, do a
 * `cstring` or `varlena`, or value based conversion to an `Datum`.
 *
 * In the case of fixed-length `INTERNALLENGTH`, it will be a `cstring`
 * with those bytes copied into the cstring.  When the `String` is variable,
 * a `varlena` will be used and the size set appropriately.
 *
 * @param value #JSValue - Javascript to convert
 * @param isnull
 * @param type #pljs_type - type of the datum
 * @param ctx #JSContext - Javascript context
 * @returns #Datum conversion of the JSValue
 */
static Datum jsvalue_to_datum_default(JSValue value, bool *isnull,
                                      pljs_type type, JSContext *ctx) {
  Datum ret = 0;

  // Set whether the Datum is `NULL` or not.
  JSValue is_set_null_value = JS_GetPropertyStr(ctx, value, "is_null");
  *isnull = JS_ToBool(ctx, is_set_null_value);

  // If the value's property of `null` is set to `true`, we return an empty
  // Datum.
  if (*isnull) {
    return (Datum)0;
  }

  // If the type is by value, it's a 32bit value.
  if (type.byval) {
    int32_t v;
    ret = JS_ToInt32(ctx, &v, value);

    ret = v;
  } else {
    // Get a copy of the data, as well as its length.
    size_t length;
    const char *js_data = JS_ToCStringLen(ctx, &length, value);

    //  If this is a variable length array then we return a `varlena`.
    if (type.length == -1) {
      //  Allocate a new cstring of the length of the type.
      struct varlena *return_data = (struct varlena *)palloc(VARHDRSZ + length);

      // Copy in the data and set the size.
      memcpy(VARDATA(return_data), js_data, length);
      SET_VARSIZE(return_data, length + VARHDRSZ);

      ret = PointerGetDatum(return_data);
    } else if (type.length > 0) {
      // Allocate the memory for the type.
      char *return_data = palloc0(type.length);

      if (length < (size_t)type.length) {
        memcpy(return_data, js_data, length);
      } else {
        memcpy(return_data, js_data, type.length);
      }

      ret = PointerGetDatum(return_data);
    }
  }

  return ret;
}

/**
 * @brief Converts a Javascript value to a Postgres #Datum.
 *
 * Takes a Javascript value and converts it into a Postgres #Datum,
 * checking whether it is an array or record and converting it
 * properly.
 *
 * @param val #JSValue - the Javascript object to convert
 * @oaram type #pljs_type - type information for the record
 * @param ctx #JSContext - Javascript context to execute in
 * @param fcinfo #FunctionCallInfo
 * @param is_null @c bool - pointer to fill of whether the record is null
 * @returns #Datum of the Postgres value
 */
Datum pljs_jsvalue_to_datum(JSValue val, Oid rettype, JSContext *ctx,
                            FunctionCallInfo fcinfo, bool *isnull) {

  pljs_type type;

  pljs_type_fill(&type, rettype);

  if (type.typid != JSONOID && type.typid != JSONBOID && JS_IsArray(ctx, val)) {
    return pljs_jsvalue_to_array(val, &type, ctx, fcinfo);
  }

  if (type.category == TYPCATEGORY_ARRAY && !JS_IsArray(ctx, val)) {
    elog(ERROR, "value is not an Array");
  }

  if (type.is_composite) {
    return pljs_jsvalue_to_record(val, &type, ctx, isnull, NULL, NULL);
  }

  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    if (fcinfo) {
      PG_RETURN_NULL();
    } else {
      if (isnull) {
        *isnull = true;
      }

      PG_RETURN_NULL();
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
    if (JS_IsBigInt(ctx, val)) {
      int64_t big_in;

      JS_ToBigInt64(ctx, &big_in, val);

      in = (int32_t)big_in;
    } else {
      JS_ToInt32(ctx, &in, val);
    }

    PG_RETURN_INT16((int16_t)in);
    break;
  }

  case INT4OID: {
    int32_t in;
    if (JS_IsBigInt(ctx, val)) {
      int64_t big_in;

      JS_ToBigInt64(ctx, &big_in, val);

      in = (int32_t)big_in;
    } else {
      JS_ToInt32(ctx, &in, val);
    }

    PG_RETURN_INT32(in);
    break;
  }

  case INT8OID: {
    int64_t in;
    if (JS_IsBigInt(ctx, val)) {
      JS_ToBigInt64(ctx, &in, val);
    } else {
      JS_ToInt64(ctx, &in, val);
    }

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
    if (JS_IsBigInt(ctx, val)) {
      // Convert the value to a string then convert it to NUMERIC.
      JSValue str = JS_ToString(ctx, val);

      const char *in = JS_ToCString(ctx, str);

      return DirectFunctionCall3(numeric_in, (Datum)in,
                                 ObjectIdGetDatum(InvalidOid),
                                 Int32GetDatum((int32)-1));

    } else {
      double in;

      JS_ToFloat64(ctx, &in, val);

      return DirectFunctionCall1(float8_numeric, Float8GetDatum((float8)in));
    }
    break;
  }

  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID:
  case NAMEOID:
  case XMLOID: {
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, val);

    Datum ret = CStringGetTextDatum(str);
    JS_FreeCString(ctx, str);

    return ret;
    break;
  }

  case JSONOID: {
    JSValueConst *argv = &val;
    JSValue js = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, js);

    // return it as a CStringTextDatum.
    Datum ret = CStringGetTextDatum(str);

    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, js);

    return ret;
    break;
  }

  case JSONBOID: {
    JSValueConst *argv = &val;
#if JSONB_DIRECT_CONVERSION
    {
      Jsonb *obj = convert_object(argv[0], ctx);
      PG_RETURN_JSONB_P(DatumGetJsonbP((unsigned long)obj));
    }
#else // JSONB_DIRECT_CONVERSION
    JSValue js = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);

    const char *str = JS_ToCString(ctx, js);

    // return it as a Datum, since there is no direct CStringGetJsonb exposed.
    Datum ret = (Datum)DatumGetJsonbP(
        DirectFunctionCall1(jsonb_in, (Datum)(char *)str));

    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, js);

    return ret;
#endif
    break;
  }

  case BYTEAOID: {
    size_t psize;
    size_t pbytes_per_element = 0;

    uint8_t *buffer;

    uint32_t length = pljs_js_array_length(val, ctx);

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
      elog(DEBUG3, "Unknown array type, tag: %lld", val.tag);
      for (uint8_t i = 0; i < 255; i++) {
        void *res = JS_GetOpaque(val, i);
        if (res != NULL) {
          elog(DEBUG3, "class_id: %d", i);
        }
      }

      PG_RETURN_NULL();
    }
  }

  case DATEOID:
    if (Is_Date(val)) {
      double in;
      JS_ToFloat64(ctx, &in, val);
      return epoch_to_date(in);
    }
    break;
  case TIMESTAMPOID:
  case TIMESTAMPTZOID:
    if (Is_Date(val)) {
      double in;
      JS_ToFloat64(ctx, &in, val);
      return epoch_to_timestamptz(in);
    }
    break;

  default:
    return jsvalue_to_datum_default(val, isnull, type, ctx);
  }

  // shut up, compiler
  PG_RETURN_NULL();
}

/**
 * @brief Converts an array of Javascript values into a Javascript array.
 *
 * Takes an array Javascript values and converts it into a Javascript
 * array of values, starting at the index requested.
 *
 * @param array #JSValue - array of #JSValue values to convert
 * @param argc @c int - number of values to convert
 * @param start @c int - index to start the conversion
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue array of the results
 */
JSValue pljs_values_to_array(JSValue *array, int argc, int start,
                             JSContext *ctx) {
  JSValue ret = JS_NewArray(ctx);

  uint32_t current = 0;
  for (int i = start; i < argc; i++) {
    JS_SetPropertyUint32(ctx, ret, current, array[i]);
    current++;
  }

  return ret;
}

/**
 * @brief Converts a Postgres #HeapTuple to a Javascript value.
 *
 * @param tupledesc #TupleDesc
 * @param heap_tuple #HeapTuple - value to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the tuple value passed
 */
JSValue pljs_tuple_to_jsvalue(TupleDesc tupledesc, HeapTuple heap_tuple,
                              JSContext *ctx) {
  JSValue obj = JS_NewObject(ctx);

  for (int i = 0; i < tupledesc->natts; i++) {
    FormData_pg_attribute *tuple_attrs = TupleDescAttr(tupledesc, i);
    if (tuple_attrs->attisdropped) {
      continue;
    }

    bool isnull;
    Datum datum = heap_getattr(heap_tuple, i + 1, tupledesc, &isnull);

    char *name = NameStr(tuple_attrs->attname);

    if (isnull) {
      JS_SetPropertyStr(ctx, obj, name, JS_NULL);
    } else {
      JS_SetPropertyStr(
          ctx, obj, name,
          pljs_datum_to_jsvalue(datum, tuple_attrs->atttypid, ctx, false));
    }
  }

  return obj;
}

/**
 * @brief Converts a Postgres SPI result to a Javascript value.
 *
 * @param status @c int - SPI status to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the SPI status
 */
JSValue pljs_spi_result_to_jsvalue(int status, JSContext *ctx) {
  JSValue result;

  if (status < 0) {
    return js_throw(spi_status_string(status), ctx);
  }

  switch (status) {
  case SPI_OK_UTILITY:
  case SPI_OK_REWRITTEN:
    if (SPI_tuptable == NULL) {
      result = JS_NewInt32(ctx, SPI_processed);
      break;
    }
    // will fallthrough here to the "SELECT" logic below

  case SPI_OK_SELECT:
  case SPI_OK_INSERT_RETURNING:
  case SPI_OK_DELETE_RETURNING:
  case SPI_OK_UPDATE_RETURNING: {
    int nrows = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;

    JSValue obj = JS_NewArray(ctx);

    for (int r = 0; r < nrows; r++) {
      JSValue value =
          pljs_tuple_to_jsvalue(tupdesc, SPI_tuptable->vals[r], ctx);

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

#if JSONB_DIRECT_CONVERSION
/**
 * @brief Converts a #JsonbValue to a #JSValue.
 *
 * @param scalar_value #JsonbValue - value to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the #JsonbValue
 */
static JSValue get_jsonb_value(JsonbValue *scalar_value, JSContext *ctx) {
  // If the value is `null` then we return `null`.
  if (scalar_value->type == jbvNull) {
    return JS_NULL;
  } else if (scalar_value->type == jbvString) {
    // A `String`.
    return JS_NewStringLen(ctx, scalar_value->val.string.val,
                           scalar_value->val.string.len);
  } else if (scalar_value->type == jbvNumeric) {
    // `Number`.
    return JS_NewFloat64(
        ctx, DatumGetFloat8(DirectFunctionCall1(
                 numeric_float8, PointerGetDatum(scalar_value->val.numeric))));
  } else if (scalar_value->type == jbvBool) {
    // `Bool`.
    return JS_NewBool(ctx, scalar_value->val.boolean);
  } else {
    elog(ERROR, "unknown jsonb scalar type");
    return JS_NULL;
  }
}

/**
 * @brief Iterates through a #JsonbIterator.
 *
 * Iterate through a `JSONB` object and creates the proper Javascript type
 * for each: `Number`, `String`, `Bool`, `Date`, `Array`, `Object`.  This
 * function is meant to be run recursively.
 *
 * @param it #JsonbIterator - `JSONB` iterator to iterate on
 * @param container #JSValue - parent container to store the value in
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of `JSONB` value
 */
static JSValue jsonb_iterate(JsonbIterator **it, JSValue container,
                             JSContext *ctx) {
  JsonbValue value;
  int32 count = 0;
  JsonbIteratorToken token;
  JSValue key;
  char *key_string = NULL;
  JSValue obj;

  // Get the next value from the `JSONB` object.
  token = JsonbIteratorNext(it, &value, false);

  // Iterate through the values until the end of the `JSONB` object.
  while (token != WJB_DONE) {
    switch (token) {
    // If it is a new Object, create one.
    case WJB_BEGIN_OBJECT:
      obj = JS_NewObject(ctx);

      // If our container is an `Array`, append the object.
      // Iterate through the `JSONB` array until we get to the end of the array.
      if (JS_IsArray(ctx, container)) {
        JS_SetPropertyUint32(ctx, container, count,
                             jsonb_iterate(it, obj, ctx));
        count++;
      } else {
        // Otherwise set the property of the `Object`.  We use the
        // #key_string that we previously stored from the `JSONB` object.
        // Iterate through the `JSONB` object until we get to the end of the
        // object.
        JS_SetPropertyStr(ctx, container, key_string,
                          jsonb_iterate(it, obj, ctx));
        JS_FreeCString(ctx, key_string);
        key_string = NULL;
      }
      break;

      // If we are done with the object, return the container.
    case WJB_END_OBJECT:
      return container;

      break;

      // Start of a new `Array`.
    case WJB_BEGIN_ARRAY:
      obj = JS_NewArray(ctx);
      if (JS_IsArray(ctx, container)) {
        JS_SetPropertyUint32(ctx, container, count,
                             jsonb_iterate(it, obj, ctx));
        count++;
      } else {
        JS_SetPropertyStr(ctx, container, key_string,
                          jsonb_iterate(it, obj, ctx));
        JS_FreeCString(ctx, key_string);
        key_string = NULL;
      }
      break;

      // End of the array, return the container.
    case WJB_END_ARRAY:
      return container;

      break;

      // Retrieve the key for an object and store it as `key_string`.
    case WJB_KEY:
      key = get_jsonb_value(&value, ctx);
      key_string = (char *)JS_ToCString(ctx, key);
      JS_FreeValue(ctx, key);

      break;

      // Retrieve the object value and set it using `key_string`.
    case WJB_VALUE:
      JS_SetPropertyStr(ctx, container, key_string,
                        get_jsonb_value(&value, ctx));
      JS_FreeCString(ctx, key_string);

      // Clear the `key_string` so it cannot be re-used.
      key_string = NULL;

      break;

      // Retrieve an array element and set it, then increment the count.
    case WJB_ELEM:
      JS_SetPropertyUint32(ctx, container, count, get_jsonb_value(&value, ctx));
      count++;
      break;

      // We are done, return the container.
    case WJB_DONE:
      return container;
      break;

    default:
      elog(ERROR, "unknown jsonb iterator value");
    }

    // Retrieve the next `JSONB` token for the loop.
    token = JsonbIteratorNext(it, &value, false);
  }

  return container;
}

/**
 * @brief Converts a #JsonbContainer to a Javascript value.
 *
 * Entry function for the `JSONB` iterator, sets up a container to
 * eventually be returned, then calls the iterator function to fill the
 * container.
 *
 * @param in #JsonbContainer - the `JSONB` object to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the `JSONB` object
 */
static JSValue convert_jsonb(JsonbContainer *in, JSContext *ctx) {
  JsonbValue val;
  JsonbIterator *it = JsonbIteratorInit(in);
  JsonbIteratorToken token = JsonbIteratorNext(&it, &val, false);

  // `JSONB` objects always need to be an `Array` or `Object`.
  JSValue container;

  // If this is an array, then create an `Array`.
  if (token == WJB_BEGIN_ARRAY) {
    container = JS_NewArray(ctx);
  } else {
    // Otherwise it is an `Object` by default.
    container = JS_NewObject(ctx);
  }

  return jsonb_iterate(&it, container, ctx);
}

// Forward declarations of the conversion functions.
static JsonbValue *jsonb_object_from_object(JSValue object,
                                            JsonbParseState **pstate,
                                            JSContext *ctx);
static JsonbValue *
jsonb_array_from_array(JSValue array, JsonbParseState **pstate, JSContext *ctx);

/**
 * @brief Converts a Postgres time in milliseconds to a 8601 datetime string.
 *
 * @param millis @c double - Postgres time in milliseconds
 * @returns @c char * representation of the date and time
 */
static char *time_as_8601(double millis) {
  char tmp[100];
  char *buf = (char *)palloc(25);

  time_t t = (time_t)(millis / 1000);
  strftime(tmp, 25, "%Y-%m-%dT%H:%M:%S", gmtime(&t));

  double integral;
  double fractional = modf(millis / 1000, &integral);

  sprintf(buf, "%s.%03dZ", tmp, (int)(fractional * 1000));

  return buf;
}

/**
 * @brief Converts a #JSValue into a `JSONB` value.
 *
 * @param parse_state #JsonbParseState - current state of the `JSONB` parsing
 * @param value #JSValue - the value to convert
 * @param type #JsonbIteratorToken
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JsonbValue `JSONB` result from the conversion
 */
static JsonbValue *jsonb_from_value(JSValue value,
                                    JsonbParseState **parse_state,
                                    JsonbIteratorToken type, JSContext *ctx) {
  JsonbValue val;

  // If the token type is a key, the only valid value is `jbvString`.
  if (type == WJB_KEY) {
    val.type = jbvString;
    size_t len;
    const char *key = JS_ToCStringLen(ctx, &len, value);

    val.val.string.val = palloc(len);
    memcpy(val.val.string.val, key, len);
    val.val.string.len = len;

    JS_FreeCString(ctx, key);
  } else {
    // Otherwise make the conversion based on the #JSValue type.
    if (JS_IsBool(value)) {
      val.type = jbvBool;
      val.val.boolean = JS_ToBool(ctx, value);
    } else if (JS_IsNull(value)) {
      val.type = jbvNull;
    } else if (JS_IsUndefined(value)) {
      return NULL;
    } else if (JS_IsString(value)) {
      val.type = jbvString;
      size_t len;
      const char *v = JS_ToCStringLen(ctx, &len, value);

      val.val.string.val = palloc(len);
      memcpy(val.val.string.val, v, len);
      val.val.string.len = len;

      JS_FreeCString(ctx, v);
    } else if (JS_IsNumber(value)) {
      double in;

      JS_ToFloat64(ctx, &in, value);

      val.val.numeric = DatumGetNumeric(
          DirectFunctionCall1(float8_numeric, Float8GetDatum((float8)in)));
      val.type = jbvNumeric;
    } else if (Is_Date(value)) {
      double in;

      JS_ToFloat64(ctx, &in, value);

      if (isnan(in)) {
        val.type = jbvNull;
      } else {
        val.val.string.val = time_as_8601(in);
        val.val.string.len = 24;
        val.type = jbvString;
      }
    } else {
      val.type = jbvString;
      size_t len;
      const char *v = JS_ToCStringLen(ctx, &len, value);

      val.val.string.val = palloc(len);
      memcpy(val.val.string.val, v, len);
      val.val.string.len = len;

      JS_FreeCString(ctx, v);
    }
  }

  // Push the result into the parse_state.
  return pushJsonbValue(parse_state, type, &val);
}

/**
 * @brief Converts a #JSValue `Array` to a #JsonbValue array.
 *
 * @param array #JSValue - `Array` to convert
 * @param parse_state #JsonbParseState - the parse state of the `JSONB` object
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JsonbValue of the `JSONB` array
 */
static JsonbValue *jsonb_array_from_array(JSValue array,
                                          JsonbParseState **parse_state,
                                          JSContext *ctx) {
  // Push the beginning of the array into the parse state.
  JsonbValue *value = pushJsonbValue(parse_state, WJB_BEGIN_ARRAY, NULL);

  // Get the length of the `Array`.
  int32_t array_length = pljs_js_array_length(array, ctx);

  // Iterate through the `Array`.
  for (int i = 0; i < array_length; i++) {
    // Get the current element.
    JSValue elem = JS_GetPropertyUint32(ctx, array, i);

    // For each type, set `value` to the result.
    if (JS_IsArray(ctx, elem)) {
      value = jsonb_array_from_array(elem, parse_state, ctx);
    } else if (JS_IsObject(elem)) {
      value = jsonb_object_from_object(elem, parse_state, ctx);
    } else {
      value = jsonb_from_value(elem, parse_state, WJB_ELEM, ctx);
    }

    // Free up the element.
    JS_FreeValue(ctx, elem);
  }

  // Set the value to the end of the array.
  value = pushJsonbValue(parse_state, WJB_END_ARRAY, NULL);

  return value;
}

/**
 * @brief Converts a #JSValue `Object` to a #JsonbValue object.
 *
 * @param object #JSValue - `Object` to convert
 * @param parse_state #JsonbParseState - the parse state of the `JSONB` object
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JsonbValue of the `JSONB` object
 */
static JsonbValue *jsonb_object_from_object(JSValue object,
                                            JsonbParseState **parse_state,
                                            JSContext *ctx) {
  // Push the beginning of the object intp the parse state.
  JsonbValue *value = pushJsonbValue(parse_state, WJB_BEGIN_OBJECT, NULL);

  uint32_t object_keys_length = 0;
  JSPropertyEnum *tab;

  // Get the keys of the `Object`.
  if (JS_GetOwnPropertyNames(ctx, &tab, &object_keys_length, object,
                             JS_GPN_STRING_MASK) < 0) {
    return false;
  }

  // Iterate through the `Object` keys.
  for (uint32_t object_key = 0; object_key < object_keys_length; object_key++) {
    // Get the value.
    JSValue o =
        JS_GetPropertyInternal(ctx, object, tab[object_key].atom, object, 0);

    value = jsonb_from_value(o, parse_state, WJB_KEY, ctx);

    // If the value is an `Array` the convert it.
    if (JS_IsArray(ctx, o)) {
      value = jsonb_array_from_array(o, parse_state, ctx);
    } else if (JS_IsObject(o)) {
      // Or convert an `Object`.
      value = jsonb_object_from_object(o, parse_state, ctx);
    } else {
      // Or anything else.
      value = jsonb_from_value(o, parse_state, WJB_VALUE, ctx);
    }

    // Free up the memory.
    JS_FreeValue(ctx, o);
  }

  // Push that we are at the end of an object.
  value = pushJsonbValue(parse_state, WJB_END_OBJECT, NULL);

  return value;
}

/**
 * @brief Converts a #JSValue `Object` to a #Jsonb value.
 *
 * @param object #JSValue - `Object` to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Jsonb the converted `JSONB` value
 */
static Jsonb *convert_object(JSValue object, JSContext *ctx) {
  // Create a new memory context for conversion.
  MemoryContext oldcontext = CurrentMemoryContext;
  MemoryContext conversion_context;

  conversion_context = AllocSetContextCreate(
      CurrentMemoryContext, "JSONB Conversion Context", ALLOCSET_SMALL_SIZES);

  MemoryContextSwitchTo(conversion_context);

  JsonbParseState *parse_state = NULL;
  JsonbValue *value;

  // Check the type and get its value.
  if (JS_IsArray(ctx, object)) {
    value = jsonb_array_from_array(object, &parse_state, ctx);
  } else if (JS_IsObject(object)) {
    value = jsonb_object_from_object(object, &parse_state, ctx);
  } else {
    pushJsonbValue(&parse_state, WJB_BEGIN_ARRAY, NULL);
    jsonb_from_value(object, &parse_state, WJB_ELEM, ctx);
    value = pushJsonbValue(&parse_state, WJB_END_ARRAY, NULL);
    value->val.array.rawScalar = true;
  }

  // Switch back to our old #MemoryContext.
  MemoryContextSwitchTo(oldcontext);

  // Create the #Jsonb object to return.
  Jsonb *ret = JsonbValueToJsonb(value);

  // Delete the conversion #MemoryContext.
  MemoryContextDelete(conversion_context);

  return ret;
}
#endif
