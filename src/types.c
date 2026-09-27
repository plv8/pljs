#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/inval.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

#include "deps/quickjs/quickjs.h"

#include "pljs.h"

#include <math.h>
#include <string.h>
#include <time.h>

/*
 * Error handling helper macros for consistent error patterns.
 */
#define PLJS_THROW_IF_NULL(ptr, msg, ctx)                                      \
  do {                                                                         \
    if ((ptr) == NULL) {                                                       \
      return js_throw((msg), (ctx));                                           \
    }                                                                          \
  } while (0)

#define PLJS_THROW_TYPE_ERROR(expected, ctx)                                   \
  js_throw("expected " expected " type", (ctx))

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

/*
 * Release a property-name enumeration obtained from JS_GetOwnPropertyNames().
 *
 * QuickJS hands the caller both the array and a reference on every atom in it,
 * and expects both back; its own js_free_prop_enum() is static, so this is the
 * public-API equivalent.  Without it every enumerated object leaked its keys
 * for the life of the backend.
 */
static void pljs_free_prop_enum(JSContext *ctx, JSPropertyEnum *tab,
                                uint32_t len) {
  if (tab == NULL) {
    return;
  }

  for (uint32_t i = 0; i < len; i++) {
    JS_FreeAtom(ctx, tab[i].atom);
  }

  js_free(ctx, tab);
}

/**
 * @brief Whether a value is a plain JavaScript object -- `{...}` -- as opposed
 * to an Array, Date, ArrayBuffer, typed array or any other branded builtin.
 *
 * This is a real brand check (see the Is_Date note above): every builtin has
 * its own class id, so only a bare object literal / `new Object` matches.
 * Callers use it to tell a `{column: value}` row object apart from a value that
 * legitimately *is* object-like (a Date for a timestamp, a typed array for a
 * bytea, an Array for an array type).
 */
bool pljs_jsvalue_is_plain_object(JSValueConst obj) {
  return JS_GetClassID(obj) == JS_CLASS_OBJECT;
}

#if JSONB_DIRECT_CONVERSION
static JSValue convert_jsonb(JsonbContainer *in, JSContext *ctx);
static JSValue get_jsonb_value(JsonbValue *scalarVal, JSContext *ctx);
static Jsonb *convert_object(JSValue object, JSContext *ctx);
#endif

static Datum pljs_string_to_datum_via_input(Oid typid, JSValueConst val,
                                            JSContext *ctx);
static Datum pljs_jsvalue_to_datum_typmod(Oid typid, int32 typmod, JSValue val,
                                          bool *is_null, JSContext *ctx,
                                          FunctionCallInfo fcinfo);
struct pljs_type_io;
static Datum pljs_jsvalue_to_datum_via_io(struct pljs_type_io *io,
                                          JSValueConst val, int32 typmod,
                                          JSContext *ctx);

/**
 * @brief What pljs needs to convert one type, looked up once per backend.
 *
 * plv8 keeps a type's input and output FmgrInfo on its type descriptor, so the
 * catalog lookups and fmgr_info() happen once rather than per value. pljs
 * rebuilt its type descriptor for every value, so every value paid for those
 * lookups again, and for a domain every value paid for domain_check()'s setup
 * as well: the typcache lookup, ExecInitExpr() of each CHECK constraint, and a
 * standalone ExprContext, all allocated in whatever memory context was
 * current. In a return_next() loop that context lives for the whole call, and
 * 200,000 rows of a domain with a CHECK constraint grew the backend by 2GB.
 *
 * The state here is built once per type and reused. A change to the type's
 * pg_type row -- or its base type's -- marks the entry stale and it is rebuilt
 * on next use; an entry still stale when the transaction ends, such as one for
 * a dropped type, is removed then. A constraint change marks every domain's
 * checking state stale; see pljs_domain_check().
 */
typedef struct pljs_type_io {
  Oid typid; /* hash key */
  bool valid;

  /*
   * Owns every allocation below.  Created on first use, since most types need
   * none, and replaced wholesale when the entry is rebuilt.
   */
  MemoryContext mcxt;
  uint32 hashvalue;      /* TYPEOID syscache hash of typid */
  uint32 base_hashvalue; /* ...and of basetype */

  /* typid itself unless it is a domain, in which case its concrete base. */
  Oid basetype;
  int32 basetypmod;
  bool is_domain;

  /*
   * A domain whose base type has no case of its own in pljs.  plv8 converts
   * every domain this way: through the domain's own input function,
   * domain_in(), which parses with the base type's input function using the
   * domain's typmod and then checks the domain's constraints.
   */
  bool domain_via_input;

  /* The type has a dedicated case in the conversion switches. */
  bool has_js_case;

  /*
   * What pljs_type_fill() reports: the type's own category and storage, and
   * for an array, or a domain over one, its element type's.  None of these
   * can change for an existing type.
   */
  char category;
  int16 length;
  bool byval;
  char align;
  Oid elemtype;
  bool elem_is_composite;
  int16 elem_length;
  bool elem_byval;
  char elem_align;

  /* Looked up on first use. */
  bool have_input;
  FmgrInfo input;
  Oid ioparam;

  bool have_output;
  FmgrInfo output;

  bool have_coercion;
  bool coercion_valid;
  FmgrInfo coercion;
  int coercion_nargs;

  /* domain_check()'s cached state. */
  void *domain_extra;

  /* A constraint has changed since the domain's checking state was built. */
  bool domain_stale;

  /* A check further up the stack is using the domain's checking state. */
  bool domain_busy;
} pljs_type_io;

static HTAB *pljs_type_io_hash = NULL;

/* Bumped by every pg_type invalidation; see pljs_type_io_lookup(). */
static uint64 pljs_type_io_invalidations = 0;

/* Some entry has gone stale since the last transaction ended. */
static bool pljs_type_io_have_stale = false;

/**
 * @brief Whether pljs has a dedicated conversion for a (non-domain) type.
 *
 * Must list exactly the scalar cases of pljs_datum_to_jsvalue() and
 * pljs_jsvalue_to_datum_internal(); everything else is converted by the
 * fallback, through the type's own text I/O functions.
 */
static bool pljs_type_has_js_case(Oid typid) {
  switch (typid) {
  case OIDOID:
  case BOOLOID:
  case INT2OID:
  case INT4OID:
  case INT8OID:
  case FLOAT4OID:
  case FLOAT8OID:
  case NUMERICOID:
  case NAMEOID:
  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID:
  case XMLOID:
  case JSONOID:
  case JSONBOID:
  case BYTEAOID:
  case DATEOID:
  case TIMESTAMPOID:
  case TIMESTAMPTZOID:
    return true;
  default:
    return false;
  }
}

/**
 * @brief Syscache callback: mark cached entries for a changed type stale.
 *
 * Only marks; never frees.  This can run in the middle of a conversion that is
 * using the entry, so the memory is released by pljs_type_io_lookup() when it
 * rebuilds the entry, or by pljs_type_io_xact_callback() if it never does.
 */
static void pljs_type_io_invalidate(Datum arg, int cacheid, uint32 hashvalue) {
  HASH_SEQ_STATUS status;
  pljs_type_io *entry;

  pljs_type_io_invalidations++;

  if (pljs_type_io_hash == NULL) {
    return;
  }

  hash_seq_init(&status, pljs_type_io_hash);

  while ((entry = (pljs_type_io *)hash_seq_search(&status)) != NULL) {
    if (hashvalue == 0 || entry->hashvalue == hashvalue ||
        entry->base_hashvalue == hashvalue) {
      entry->valid = false;
      pljs_type_io_have_stale = true;
    }
  }
}

/**
 * @brief Syscache callback: a constraint changed, so mark every domain's
 * checking state stale.
 *
 * The hash of a pg_constraint row does not say which domain, if any, it
 * belongs to; the typcache marks every domain for the same reason.
 */
static void pljs_type_io_invalidate_constraints(Datum arg, int cacheid,
                                                uint32 hashvalue) {
  HASH_SEQ_STATUS status;
  pljs_type_io *entry;

  if (pljs_type_io_hash == NULL) {
    return;
  }

  hash_seq_init(&status, pljs_type_io_hash);

  while ((entry = (pljs_type_io *)hash_seq_search(&status)) != NULL) {
    if (entry->is_domain) {
      entry->domain_stale = true;
    }
  }
}

/**
 * @brief Transaction callback: remove the entries that went stale.
 *
 * A stale entry is rebuilt only when its type is looked up again, which a
 * dropped type never is, so each one -- every temporary table's row type, for
 * instance -- kept its entry and its memory for the life of the backend.  No
 * conversion spans the end of a transaction, so nothing can still be using
 * one here.
 */
static void pljs_type_io_xact_callback(XactEvent event, void *arg) {
  HASH_SEQ_STATUS status;
  pljs_type_io *entry;

  switch (event) {
  case XACT_EVENT_COMMIT:
  case XACT_EVENT_PARALLEL_COMMIT:
  case XACT_EVENT_ABORT:
  case XACT_EVENT_PARALLEL_ABORT:
  case XACT_EVENT_PREPARE:
    break;
  default:
    return;
  }

  if (!pljs_type_io_have_stale || pljs_type_io_hash == NULL) {
    return;
  }

  pljs_type_io_have_stale = false;

  hash_seq_init(&status, pljs_type_io_hash);

  while ((entry = (pljs_type_io *)hash_seq_search(&status)) != NULL) {
    if (!entry->valid) {
      if (entry->mcxt != NULL) {
        MemoryContextDelete(entry->mcxt);
      }

      /* Removing the entry just returned is allowed during a scan. */
      hash_search(pljs_type_io_hash, &entry->typid, HASH_REMOVE, NULL);
    }
  }
}

/**
 * @brief Forgets everything a cached type looked up on first use.
 */
static void pljs_type_io_forget_state(pljs_type_io *io) {
  io->have_input = false;
  io->have_output = false;
  io->have_coercion = false;
  io->coercion_valid = false;
  io->domain_extra = NULL;
  io->domain_stale = false;
}

/**
 * @brief Returns the cached conversion state for a type, building it if needed.
 *
 * @param typid #Oid - the type
 * @returns #pljs_type_io - owned by the cache; valid until the next lookup
 * that finds it stale, and never freed while a caller up the stack could
 * still be using it
 */
static pljs_type_io *pljs_type_io_lookup(Oid typid) {
  pljs_type_io *entry;
  bool found;
  char typtype;
  char base_category;
  bool is_preferred;
  uint64 invalidations;

  if (pljs_type_io_hash == NULL) {
    HASHCTL ctl = {0};

    ctl.keysize = sizeof(Oid);
    ctl.entrysize = sizeof(pljs_type_io);
    ctl.hcxt = TopMemoryContext;

    pljs_type_io_hash = hash_create("PLJS Type I/O Cache", 64, &ctl,
                                    HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    CacheRegisterSyscacheCallback(TYPEOID, pljs_type_io_invalidate, (Datum)0);
    CacheRegisterSyscacheCallback(
        CONSTROID, pljs_type_io_invalidate_constraints, (Datum)0);
    RegisterXactCallback(pljs_type_io_xact_callback, NULL);
  }

  entry = (pljs_type_io *)hash_search(pljs_type_io_hash, &typid, HASH_ENTER,
                                      &found);

  if (found && entry->valid) {
    return entry;
  }

  if (!found) {
    entry->mcxt = NULL;
  } else if (entry->mcxt != NULL) {
    /*
     * A conversion further up the stack may still hold this entry's FmgrInfo
     * or domain_check() state -- a CHECK constraint can call a pljs function
     * that converts a value of the same domain.  So do not free the old state
     * here: hand it to the transaction, which frees it when it ends.
     */
    if (IsTransactionState()) {
      MemoryContextSetParent(entry->mcxt, TopTransactionContext);
    } else {
      MemoryContextDelete(entry->mcxt);
    }

    entry->mcxt = NULL;
  }

  /* Until the build below succeeds; an error leaves it for the sweep. */
  entry->valid = false;
  pljs_type_io_have_stale = true;

  entry->domain_busy = false;
  pljs_type_io_forget_state(entry);

  /*
   * The catalog lookups below can process invalidations, and one for this
   * type processed after its row was read would be lost if the entry were
   * then simply marked valid.  Count them instead: if any arrive, the entry
   * is still returned, but left stale to be rebuilt on its next use.
   */
  invalidations = pljs_type_io_invalidations;

  typtype = get_typtype(typid);

  if (typtype == '\0') {
    elog(ERROR, "cache lookup failed for type %u", typid);
  }

  entry->is_domain = (typtype == TYPTYPE_DOMAIN);
  entry->basetypmod = -1;
  entry->basetype = entry->is_domain
                        ? getBaseTypeAndTypmod(typid, &entry->basetypmod)
                        : typid;

  entry->hashvalue = GetSysCacheHashValue1(TYPEOID, ObjectIdGetDatum(typid));
  entry->base_hashvalue =
      GetSysCacheHashValue1(TYPEOID, ObjectIdGetDatum(entry->basetype));

  entry->has_js_case = pljs_type_has_js_case(entry->basetype);

  get_type_category_preferred(typid, &entry->category, &is_preferred);
  get_typlenbyvalalign(typid, &entry->length, &entry->byval, &entry->align);

  entry->elemtype = InvalidOid;
  entry->elem_is_composite = false;

  if (entry->category == TYPCATEGORY_ARRAY) {
    /*
     * A domain over an array has the array's category but no element type of
     * its own, so look through it to the array it is a domain over.
     */
    entry->elemtype = get_element_type(entry->basetype);

    if (OidIsValid(entry->elemtype)) {
      entry->elem_is_composite =
          (TypeCategory(entry->elemtype) == TYPCATEGORY_COMPOSITE);
      get_typlenbyvalalign(entry->elemtype, &entry->elem_length,
                           &entry->elem_byval, &entry->elem_align);
    }
  }

  if (entry->is_domain) {
    get_type_category_preferred(entry->basetype, &base_category, &is_preferred);
  } else {
    base_category = entry->category;
  }

  entry->domain_via_input = entry->is_domain && !entry->has_js_case &&
                            base_category != TYPCATEGORY_ARRAY &&
                            base_category != TYPCATEGORY_COMPOSITE;

  entry->valid = (pljs_type_io_invalidations == invalidations);

  return entry;
}

/**
 * @brief Returns a cached type's memory context, creating it on first use.
 */
static MemoryContext pljs_type_io_mcxt(pljs_type_io *io) {
  if (io->mcxt == NULL) {
    io->mcxt = AllocSetContextCreate(TopMemoryContext, "PLJS Type I/O",
                                     ALLOCSET_SMALL_SIZES);
  }

  return io->mcxt;
}

/**
 * @brief Returns the input function for a cached type, looking it up once.
 */
static FmgrInfo *pljs_type_io_input(pljs_type_io *io) {
  if (!io->have_input) {
    Oid typinput;

    getTypeInputInfo(io->typid, &typinput, &io->ioparam);
    fmgr_info_cxt(typinput, &io->input, pljs_type_io_mcxt(io));
    io->have_input = true;
  }

  return &io->input;
}

/**
 * @brief Returns the output function for a cached type, looking it up once.
 */
static FmgrInfo *pljs_type_io_output(pljs_type_io *io) {
  if (!io->have_output) {
    Oid typoutput;
    bool typisvarlena;

    getTypeOutputInfo(io->typid, &typoutput, &typisvarlena);
    fmgr_info_cxt(typoutput, &io->output, pljs_type_io_mcxt(io));
    io->have_output = true;
  }

  return &io->output;
}

/**
 * @brief Resolves a domain, or a chain of domains, to its concrete base type.
 *
 * The same as getBaseType(), answered from the type cache.
 *
 * @param typid #Oid - the type
 * @returns #Oid of the base type, or @p typid itself if it is not a domain
 */
Oid pljs_type_base(Oid typid) { return pljs_type_io_lookup(typid)->basetype; }

/**
 * @brief Applies a length/precision modifier to a value pljs has built itself.
 *
 * The fallback passes the typmod to the type's input function, but a type with
 * a case of its own -- varchar, bpchar, numeric, timestamp -- builds its datum
 * without one, so `char(2)` accepted 'Texas' and `numeric(5,2)` kept 3.14159.
 * This runs the type's length coercion function, the one an assignment cast
 * uses, so the value is rounded, padded or rejected exactly as it would be by
 * an INSERT or a PL/pgSQL assignment.
 *
 * @param io #pljs_type_io - the (non-domain) type of @p value
 * @param value #Datum - a non-null value of that type
 * @param typmod @c int32 - the modifier to apply; nothing is done if negative
 * @returns #Datum coerced to @p typmod
 */
static Datum pljs_apply_typmod(pljs_type_io *io, Datum value, int32 typmod) {
  if (typmod < 0) {
    return value;
  }

  if (!io->have_coercion) {
    Oid funcid;

    if (find_typmod_coercion_function(io->typid, &funcid) ==
        COERCION_PATH_FUNC) {
      fmgr_info_cxt(funcid, &io->coercion, pljs_type_io_mcxt(io));
      io->coercion_nargs = get_func_nargs(funcid);
      io->coercion_valid = true;
    }

    io->have_coercion = true;
  }

  if (!io->coercion_valid) {
    return value;
  }

  if (io->coercion_nargs == 2) {
    return FunctionCall2(&io->coercion, value, Int32GetDatum(typmod));
  }

  /* isExplicit = false: assignment semantics, so too long is an error. */
  return FunctionCall3(&io->coercion, value, Int32GetDatum(typmod),
                       BoolGetDatum(false));
}

/**
 * @brief Checks a value against a domain's constraints.
 *
 * A domain converted through its input function (domain_via_input) has @p str
 * parsed and checked by domain_in(), and the parsed value is returned; @p str
 * is NULL for a SQL NULL.  Any other domain has @p value, already built as its
 * base type, checked by domain_check() and returned unchanged.  domain_check()
 * needs the base type's binary receive function, which every type pljs builds
 * itself has.
 *
 * Both functions keep their state -- the constraints' ExprStates and the
 * ExprContext they run in -- in the entry's memory context, and two things
 * about that state need care:
 *
 *   - A check further up the stack may be using it.  A CHECK constraint can
 *     call a function that converts a value of the same domain, and the
 *     nested check then overwrote the value the outer one's expression reads
 *     as VALUE and reset the memory it was evaluating in: with
 *     `CHECK (f(VALUE) AND VALUE < 1000)`, where f() converts 5 to the same
 *     domain, 2000 passed.  A nested check gets state of its own, built and
 *     freed around that one call.
 *
 *   - When a constraint changes, both rebuild the constraints' ExprStates in
 *     that context and never free the old ones, since PostgreSQL expects the
 *     context to be short-lived.  This one lives as long as the backend, and
 *     every constraint change anywhere -- a temporary table's CHECK included
 *     -- grew it by another copy of every cached domain's constraints.  So a
 *     constraint change marks the state stale, and it is freed here, to be
 *     built again.
 *
 * @param io #pljs_type_io - the domain
 * @param str @c char* - the text to parse, or NULL; for a domain_via_input
 * domain
 * @param value #Datum - the value to check; for any other domain
 * @param isnull @c bool - whether @p value is SQL NULL
 * @returns #Datum of the domain
 */
static Datum pljs_domain_check(pljs_type_io *io, char *str, Datum value,
                               bool isnull) {
  MemoryContext nested = NULL;
  MemoryContext mcxt;
  FmgrInfo nested_input;
  FmgrInfo *input = NULL;
  Oid ioparam = InvalidOid;
  void *nested_extra = NULL;
  void **extra;
  Datum ret = value;

  if (io->domain_busy) {
    nested = AllocSetContextCreate(
        CurrentMemoryContext, "PLJS Nested Domain Check", ALLOCSET_SMALL_SIZES);
    mcxt = nested;
    extra = &nested_extra;

    if (io->domain_via_input) {
      Oid typinput;

      getTypeInputInfo(io->typid, &typinput, &ioparam);
      fmgr_info_cxt(typinput, &nested_input, nested);
      input = &nested_input;
    }
  } else {
    if (io->domain_stale && io->mcxt != NULL) {
      MemoryContextReset(io->mcxt);
      pljs_type_io_forget_state(io);
    }

    io->domain_stale = false;
    mcxt = pljs_type_io_mcxt(io);
    extra = &io->domain_extra;

    if (io->domain_via_input) {
      input = pljs_type_io_input(io);
      ioparam = io->ioparam;
    }

    io->domain_busy = true;
  }

  PG_TRY();
  {
    if (io->domain_via_input) {
      ret = InputFunctionCall(input, str, ioparam, -1);
    } else {
      domain_check(value, isnull, io->typid, extra, mcxt);
    }
  }
  PG_FINALLY();
  {
    if (nested != NULL) {
      MemoryContextDelete(nested);
    } else {
      io->domain_busy = false;
    }
  }
  PG_END_TRY();

  return ret;
}

/**
 * @brief Enforces a domain's constraints on a SQL NULL.
 *
 * NOT NULL, or a CHECK that rejects NULL, has to hold for a NULL from
 * JavaScript as much as for any other value.
 *
 * @param io #pljs_type_io - the type being assigned NULL; a no-op unless a
 * domain
 */
static void pljs_domain_check_null(pljs_type_io *io) {
  if (io->is_domain) {
    pljs_domain_check(io, NULL, (Datum)0, true);
  }
}

/**
 * @brief Converts a Javascript epoch to a Datum.
 *
 * @param @c double Javascript epoch
 * @returns #Datum of type `DATEADT`
 */
static Datum pljs_convert_epoch_to_date(double epoch) {
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
static Datum pljs_convert_epoch_to_timestamptz(double epoch) {
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
static double pljs_convert_date_to_epoch(DateADT date) {
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
static double pljs_convert_timestamptz_to_epoch(TimestampTz tm) {
  double epoch;

#ifdef HAVE_INT64_TIMESTAMP
  epoch = (double)tm / 1000.0;
#else
  epoch = (double)tm * 1000.0;
#endif

  return epoch + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400000.0;
}

/*
 * Free a detoasted copy, if detoasting made one.
 *
 * PG_DETOAST_DATUM() and its relatives return the original pointer when the
 * value was already a plain, uncompressed varlena, and a fresh palloc'd copy
 * otherwise -- when it was compressed, stored out of line, or carrying a
 * 1-byte short header, which is what PostgreSQL uses for any small value in a
 * tuple.  Nothing freed those copies, so converting a short-headered text,
 * array or jsonb column to JavaScript leaked one per row, for the length of
 * the enclosing function call.
 */
static void pljs_free_if_detoasted(void *detoasted, Datum original) {
  if (detoasted != NULL && detoasted != DatumGetPointer(original)) {
    pfree(detoasted);
  }
}

/**
 * @brief Converts a `NUMERIC` #Datum to a Javascript double.
 *
 * Does what `numeric_float8()` does -- render the value with `numeric_out()`
 * and parse it back with `float8in()` -- rather than calling it, for two
 * reasons.  As of PostgreSQL 19 `numeric_float8()` frees the string it renders
 * only on its error path, so every successful conversion leaked one.  And it
 * reaches its argument through `PG_GETARG_NUMERIC()`, which detoasts without
 * freeing: a `NUMERIC` column carries a 1-byte short header in a tuple, so
 * that leaked a copy per row on every server version.  Detoasting here instead
 * puts both allocations somewhere we can release them.
 *
 * A number inside a `JSONB` value needs no copy -- it is stored int-aligned
 * with a full header -- so detoasting returns it unchanged and frees nothing.
 *
 * Special values survive the round trip: `numeric_out()` writes `NaN`,
 * `Infinity` and `-Infinity`, all of which `float8in()` accepts.
 *
 * @param arg #Datum of type `Numeric`
 * @returns @c double value of the numeric
 */
static double pljs_numeric_to_double(Datum arg) {
  Numeric numeric = DatumGetNumeric(arg);
  char *str = DatumGetCString(
      DirectFunctionCall1(numeric_out, NumericGetDatum(numeric)));
  double value =
      DatumGetFloat8(DirectFunctionCall1(float8in, CStringGetDatum(str)));

  pfree(str);
  pljs_free_if_detoasted(numeric, arg);

  return value;
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
static char *pljs_util_dup_pgtext(text *what) {
  size_t len = VARSIZE(what) - VARHDRSZ;
  char *dup = palloc(len + 1);

  memcpy(dup, VARDATA(what), len);
  dup[len] = 0;

  return dup;
}

/**
 * @brief Converts an SPI status into static text.
 */
static const char *pljs_util_spi_status_string(int status) {
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
 * @brief Fills a `pljs_type` from a type's cached conversion state.
 *
 * @param type #pljs_type - the location to store the type data
 * @param io #pljs_type_io - the Postgres type to decode
 */
static void pljs_type_fill_io(pljs_type *type, pljs_type_io *io) {
  type->typid = io->typid;
  type->category = io->category;
  type->is_composite = (io->category == TYPCATEGORY_COMPOSITE);
  type->length = io->length;
  type->byval = io->byval;
  type->align = io->align;

  if (io->category == TYPCATEGORY_ARRAY) {
    if (!OidIsValid(io->elemtype)) {
      ereport(ERROR, (errmsg("cannot determine element type of array: %u",
                             io->typid)));
    }

    type->typid = io->elemtype;
    type->is_composite = io->elem_is_composite;
    type->length = io->elem_length;
    type->byval = io->elem_byval;
    type->align = io->elem_align;
  } else if (io->category == TYPCATEGORY_PSEUDOTYPE) {
    type->is_composite = true;
  }
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
  pljs_type_fill_io(type, pljs_type_io_lookup(typid));
}

/**
 * @brief Helper to get or lookup a TupleDesc with consistent ownership
 * semantics.
 *
 * If a TupleDesc is provided, it is used directly and needs_release is set to
 * false. If NULL is provided, a TupleDesc is looked up from the type OID and
 * needs_release is set to true, indicating the caller must call
 * ReleaseTupleDesc when done.
 *
 * @param typid #Oid - the type OID to look up if provided is NULL
 * @param provided #TupleDesc - optional pre-existing TupleDesc to use
 * @param needs_release @c bool* - output indicating if caller must release
 * @returns #TupleDesc the tuple descriptor to use
 */
static TupleDesc pljs_get_tupdesc(Oid typid, TupleDesc provided,
                                  bool *needs_release) {
  if (provided != NULL) {
    *needs_release = false;
    return provided;
  }

  *needs_release = true;
  return lookup_rowtype_tupdesc(typid, -1);
}

/**
 * @brief Converts a #Datum for a Javascript object.
 *
 * Takes a #Datum and converts it to a Javascript object.  If there is
 * an error, throws a Javascript exception.
 *
 * @param type #pljs_type - type information of the #Datum
 * @param arg #Datum - value to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the object or thrown exception in case of error
 */
JSValue pljs_datum_to_object(pljs_type *type, Datum arg, JSContext *ctx) {
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
    JSValue error = js_throw_error_data(edata, ctx);
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

      JS_SetPropertyStr(
          ctx, obj, colname,
          pljs_datum_to_jsvalue(TupleDescAttr(tupdesc, i)->atttypid, datum,
                                isnull, true, ctx));
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
 * @param type #pljs_type - type information for the array
 * @param arg #Datum - Postgres array to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the array
 */
JSValue pljs_datum_to_array(pljs_type *type, Datum arg, JSContext *ctx) {
  JSValue array = JS_NewArray(ctx);
  Datum *values;
  bool *nulls;
  int nelems;

  ArrayType *array_value = DatumGetArrayTypeP(arg);

  /*
   * pljs represents a SQL array as a flat JavaScript array. deconstruct_array()
   * would happily flatten a multidimensional one into a single JavaScript array
   * and discard the dimensionality -- {{1,2},{3,4}} becomes [1,2,3,4] -- so a
   * round trip silently changes the value. Say so instead of losing the shape.
   */
  if (ARR_NDIM(array_value) > 1) {
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot convert a multidimensional array to a JavaScript "
                    "array"),
             errdetail("pljs represents SQL arrays as one-dimensional "
                       "JavaScript arrays.")));
  }

  deconstruct_array(array_value, type->typid, type->length, type->byval,
                    type->align, &values, &nulls, &nelems);

  for (int i = 0; i < nelems; i++) {
    JSValue value =
        pljs_datum_to_jsvalue(type->typid, values[i], nulls[i], true, ctx);

    JS_SetPropertyUint32(ctx, array, i, value);
  }

  JSValue length = JS_NewInt32(ctx, nelems);
  JS_SetPropertyStr(ctx, array, "length", length);

  pfree(values);
  pfree(nulls);

  /*
   * Freed only here: for a by-reference element type, deconstruct_array()
   * hands back pointers into the array rather than copies, so it has to stay
   * alive until every element has been converted above.
   */
  pljs_free_if_detoasted(array_value, arg);

  return array;
}

/**
 * @brief Fallback type conversion from @Datum to @JSValue.
 *
 * Every type without a case of its own in pljs_datum_to_jsvalue() arrives
 * here, which is most of them: uuid, inet, cidr, interval, time, timetz,
 * point and the rest of the geometric types, bit and bit varying, money,
 * tsvector, every enum, and every extension type such as ltree.
 *
 * It used to hand JavaScript the datum's *internal* bytes -- the varlena
 * payload for a variable-length type, type.length bytes read straight off the
 * pointer for a fixed-length one, and a truncating JS_NewInt32() for anything
 * pass-by-value.  That is only the value the user meant for types whose
 * storage happens to be text, and it is wrong three separate ways for the
 * others:
 *
 *   - JS_NewStringLen() decodes its input as UTF-8, so every byte above 0x7F
 *     became U+FFFD.  A uuid arrived as 16 binary bytes, came back through
 *     the reverse fallback as the *replacement characters'* encoding, and
 *     round-tripped 0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f into
 *     01efbfbd-efbf-bd3a-4b7c-5defbfbd0a1b.  Through a BEFORE INSERT trigger
 *     returning NEW that silently corrupts a uuid primary key.
 *
 *   - The reverse fallback memcpy'd the string's bytes back over the type's
 *     internal representation without the type ever validating them, so the
 *     result was not merely wrong but malformed.  `SELECT $$ return v; $$` on
 *     a `bit varying` produced a varbit datum whose bit length disagreed with
 *     its allocation, and reading it back crashed the backend in varbit_out().
 *
 *   - Pass-by-value types were squeezed through int32.  money lost the high
 *     half ($92,233,720,368.54 came back as $20,772,523.42), `time` returned
 *     00:4294967264:28.640256, and an enum reached JavaScript as its pg_enum
 *     OID rather than its label.
 *
 * So use the type's own text output function, which is what psql, COPY and
 * to_json() all do, and what plv8 does for the same set of types.  The value
 * reaches JavaScript as the text the user would recognise, and
 * pljs_jsvalue_to_datum_fallback() parses it back through the matching input
 * function -- which validates, so a bad value raises instead of being stored.
 *
 * The output function detoasts its own argument, so no detoasting is needed
 * here.  It is looked up once per type and cached, as plv8 does.
 *
 * @param arg #Datum - Postgres datum to convert
 * @param io #pljs_type_io - cached conversion state for the datum's type
 * @param ctx #JSContext - Javascript context
 * @returns #JSValue conversion of the Datum
 */
static JSValue pljs_datum_to_jsvalue_fallback(Datum arg, pljs_type_io *io,
                                              JSContext *ctx) {
  char *str;
  JSValue ret;

  str = OutputFunctionCall(pljs_type_io_output(io), arg);
  ret = JS_NewString(ctx, str);
  pfree(str);

  return ret;
}

/**
 * @brief Converts a Postgres #Datum to a Javascript value.
 *
 * Takes a Postgres #Datum and type and converts it into a Javascript
 * value.  If the type is an array or is composite, then call out to
 * the correct functions.
 *
 * A domain is dispatched as its base type.  A domain has its own OID and its
 * own pg_type row, so it matches none of the cases below and would land in the
 * fallback -- which is right for a domain over uuid, and wrong for a domain
 * over anything pljs has a case for.  `CREATE DOMAIN dint AS int4` would
 * arrive as the string "5" rather than the number 5, so `return v + 1` on it
 * produced "51"; a domain over boolean arrived as "f", which is truthy.
 *
 * @param argtype #Oid - type information for the type
 * @param arg #Datum - Postgres value to convert
 * @param is_null @c bool - whether the datum is null
 * @param expand_composite @c bool - whether to expand composite types to
 * objects
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the value, or JS_NULL if null
 */
JSValue pljs_datum_to_jsvalue(Oid argtype, Datum arg, bool is_null,
                              bool expand_composite, JSContext *ctx) {
  // Handle null case explicitly
  if (is_null) {
    return JS_NULL;
  }

  JSValue return_result;
  char *str;

  /*
   * Walks a chain of domains down to the concrete type; a type that is not a
   * domain is its own base.  Its output function is the domain's too, so the
   * fallback can use the base type's cached state.
   */
  pljs_type_io *io = pljs_type_io_lookup(argtype);

  argtype = io->basetype;

  if (io->is_domain) {
    io = pljs_type_io_lookup(argtype);
  }

  pljs_type type;
  pljs_type_fill_io(&type, io);

  if (type.category == TYPCATEGORY_ARRAY) {
    return pljs_datum_to_array(&type, arg, ctx);
  }

  if (expand_composite && type.is_composite) {
    return pljs_datum_to_object(&type, arg, ctx);
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
    return_result = JS_NewFloat64(ctx, pljs_numeric_to_double(arg));
    break;

  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID:
  case XMLOID: {
    // Get a copy of the string.
    text *text_value = DatumGetTextP(arg);

    str = pljs_util_dup_pgtext(text_value);

    return_result = JS_NewString(ctx, str);

    // Free the memory allocated.
    pfree(str);
    pljs_free_if_detoasted(text_value, arg);
    break;
  }

  case NAMEOID:
    return_result = JS_NewString(ctx, DatumGetName(arg)->data);
    break;

  case JSONOID: {
    // Get a copy of the string.
    text *json_value = DatumGetTextP(arg);

    str = pljs_util_dup_pgtext(json_value);

    return_result = JS_ParseJSON(ctx, str, strlen(str), NULL);

    // free the memory allocated.
    pfree(str);
    pljs_free_if_detoasted(json_value, arg);
    break;
  }

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

    pljs_free_if_detoasted(jsonb, arg);
#else
    // Get the datum.
    Jsonb *jb = DatumGetJsonbP(arg);

    // Convert it to a string (takes some casting, but JsonbContainer is also
    // a varlena).
    str = JsonbToCString(NULL, (JsonbContainer *)VARDATA(jb), VARSIZE(jb));

    return_result = JS_ParseJSON(ctx, str, strlen(str), NULL);

    // Free the memory allocated.
    pfree(str);
    pljs_free_if_detoasted(jb, arg);
#endif
    break;
  }

  case BYTEAOID: {
    void *p = PG_DETOAST_DATUM_COPY(arg);
    char *buf = palloc(VARSIZE_ANY_EXHDR(p) + 1);

    memcpy(buf, VARDATA(p), VARSIZE_ANY_EXHDR(p));

    return_result = JS_NewStringLen(ctx, buf, VARSIZE_ANY_EXHDR(p));
    pfree(buf);

    // PG_DETOAST_DATUM_COPY always allocates, so this is never the original.
    pfree(p);
    break;
  }

  case DATEOID:
    return_result =
        JS_NewDate(ctx, pljs_convert_date_to_epoch(DatumGetDateADT(arg)));
    break;
  case TIMESTAMPOID:
  case TIMESTAMPTZOID:
    return_result = JS_NewDate(
        ctx, pljs_convert_timestamptz_to_epoch(DatumGetTimestampTz(arg)));
    break;

  default:
    return_result = pljs_datum_to_jsvalue_fallback(arg, io, ctx);
  }

  return return_result;
}

/**
 * @brief Converts a Javascript array to a Postgres array.
 *
 * Takes a Javascript #JSValue of an array and type and converts
 * it into a Postgres array.
 *
 * @param type #pljs_type - type information for the array
 * @param val #JSValue - Javascript array to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @param typmod @c int32 - the array's typmod, which applies to each element
 * @returns #Datum of the array
 */
Datum pljs_jsvalue_to_array(pljs_type *type, JSValue val, JSContext *ctx,
                            int32 typmod) {
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

    /*
     * Pass NULL rather than fcinfo: an element is not the function's result.
     * Every path in pljs_jsvalue_to_datum() that signals SQL NULL does it
     * through pljs_null_datum(), which sets fcinfo->isnull when it has an
     * fcinfo -- and that flag marks the *whole array* as SQL NULL. So one
     * null-producing element discarded every other element: [1, undefined, 4]
     * came back as NULL instead of {1,NULL,4}, as did a date[] holding an
     * invalid Date. With NULL fcinfo the null is reported through the
     * per-element is_null out-parameter, which is what nulls[i] is for.
     */
    values[i] = pljs_jsvalue_to_datum_typmod(type->typid, typmod, elem,
                                             &nulls[i], ctx, NULL);

    /* JS_GetPropertyUint32() returns an owned reference. */
    JS_FreeValue(ctx, elem);
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
                                                   TupleDesc tupdesc,
                                                   char **missing_colname,
                                                   char **provided_keys) {
  uint32_t object_keys_length = 0;
  JSPropertyEnum *tab;

  if (missing_colname != NULL) {
    *missing_colname = NULL;
  }

  if (provided_keys != NULL) {
    *provided_keys = NULL;
  }

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

      if (atom != NULL && strcmp(colname, atom) == 0) {
        found = true;
        JS_FreeCString(ctx, atom);
        break;
      }

      JS_FreeCString(ctx, atom);
    }

    if (!found) {
      /*
       * Report which column is missing, and what the object did offer, so the
       * caller can raise something actionable.  The bare "field name / property
       * name mismatch" left the author to guess, and the usual cause is a case
       * difference: JavaScript property names are case sensitive while
       * PostgreSQL folds unquoted identifiers to lower case.
       */
      if (missing_colname != NULL) {
        *missing_colname = pstrdup(colname);
      }

      if (provided_keys != NULL) {
        StringInfoData keys;
        uint32_t listed = 0;

        /*
         * Cap the list.  An object with ten thousand properties would otherwise
         * produce a ten-thousand-name error message, which goes to the server
         * log as well as to the client.  Ten names plus the total is enough to
         * diagnose a typo, which is what this message is for.
         */
        const uint32_t max_listed = 10;

        initStringInfo(&keys);

        for (uint32_t object_key = 0; object_key < object_keys_length;
             object_key++) {
          const char *atom;

          if (listed >= max_listed) {
            appendStringInfo(&keys, ", ... (%u properties in total)",
                             object_keys_length);
            break;
          }

          atom = JS_AtomToCString(ctx, tab[object_key].atom);

          if (atom == NULL) {
            continue;
          }

          if (keys.len > 0) {
            appendStringInfoString(&keys, ", ");
          }

          appendStringInfoString(&keys, atom);
          JS_FreeCString(ctx, atom);
          listed++;
        }

        *provided_keys = keys.data;
      }

      pljs_free_prop_enum(ctx, tab, object_keys_length);

      return false;
    }
  }

  pljs_free_prop_enum(ctx, tab, object_keys_length);

  return true;
}

/**
 * @brief Converts a composite Javascript object into an array of Datums.
 *
 * Takes a Javascript object and converts it into an array of Datums, setting
 * the null flag for each Datum if it is null.  Note that this function assumes
 * that `is_null` is allocated and initialized to `0` (`false`) for each
 * element.
 *
 * @param type #pljs_type - type information for the record (used if tupdesc is
 * NULL)
 * @param val #JSValue - the Javascript object to convert
 * @param is_null @c bool** - pointer to array of null flags for each element
 * @param tupdesc #TupleDesc - can be `NULL`, will be looked up from type if so
 * @param ctx #JSContext - Javascript context to execute in
 * @returns Array of #Datum of the Javascript object, or NULL if val is
 * null/undefined
 */
Datum *pljs_jsvalue_to_datums(pljs_type *type, JSValue val, bool **is_null,
                              TupleDesc tupdesc, JSContext *ctx) {
  // Check for null/undefined BEFORE any allocations to avoid memory leaks
  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    return NULL;
  }

  // Get the tuple descriptor, looking it up if not provided
  bool cleanup_tupdesc;
  tupdesc = pljs_get_tupdesc(type ? type->typid : InvalidOid, tupdesc,
                             &cleanup_tupdesc);

  // Allocate the values array now that we have the tuple descriptor
  Datum *values = (Datum *)palloc(sizeof(Datum) * tupdesc->natts);

  for (int16 c = 0; c < tupdesc->natts; c++) {
    // If this is a dropped column, we can skip it, and set the null flag to
    // true.
    if (TupleDescAttr(tupdesc, c)->attisdropped) {
      (*is_null)[c] = true;
      continue;
    }

    // Retrieve the column name of each attribute that we are expecting, we
    // only care about named tuples.
    char *colname = NameStr(TupleDescAttr(tupdesc, c)->attname);

    JSValue o = JS_GetPropertyStr(ctx, val, colname);

    // Set the value of each Datum, or set the `is_null` flag if it is
    // considered `NULL`.  The column's typmod applies: a trigger's NEW, or a
    // composite with a varchar(n) or bit(n) column, is not re-checked by the
    // executor.
    values[c] = pljs_jsvalue_to_datum_typmod(
        TupleDescAttr(tupdesc, c)->atttypid,
        TupleDescAttr(tupdesc, c)->atttypmod, o, &(*is_null)[c], ctx, NULL);

    /*
     * JS_GetPropertyStr() returns an owned reference, so it has to be released
     * whatever the column's value.  Leaking it costs one QuickJS reference per
     * column per row, on every composite return and every return_next() of a
     * row object, which is the hottest allocation path in the extension.
     * Because QuickJS runs on the libc allocator the loss is invisible to
     * pg_backend_memory_contexts; it counts against pljs.memory_limit and is
     * not returned until the backend exits.
     */
    JS_FreeValue(ctx, o);
  }

  if (cleanup_tupdesc) {
    ReleaseTupleDesc(tupdesc);
  }

  return values;
}

/**
 * @brief Converts a Javascript object into a Postgres record.
 *
 * Takes a Javascript object and converts it into a Postgres
 * record (composite Postgres type).
 *
 * @param type #pljs_type - type information for the record
 * @param val #JSValue - the Javascript object to convert
 * @param is_null @c bool - pointer to fill of whether the record is null
 * @param tupdesc #TupleDesc - can be `NULL`
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Datum of the Postgres record
 */
Datum pljs_jsvalue_to_record(pljs_type *type, JSValue val, bool *is_null,
                             TupleDesc tupdesc, JSContext *ctx) {
  Datum result = 0;

  // If the value is null or undefined, we can simply set the record to null
  // and return a `NULL` Datum.
  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    *is_null = true;
    return (Datum)0;
  }

  // Get the tuple descriptor, looking it up if not provided
  bool cleanup_tupdesc;
  tupdesc = pljs_get_tupdesc(type->typid, tupdesc, &cleanup_tupdesc);

  Datum *values = (Datum *)palloc0(sizeof(Datum) * tupdesc->natts);
  bool *nulls = (bool *)palloc0(sizeof(bool) * tupdesc->natts);

  for (int16 c = 0; c < tupdesc->natts; c++) {
    if (TupleDescAttr(tupdesc, c)->attisdropped) {
      nulls[c] = true;
      continue;
    }

    char *colname = NameStr(TupleDescAttr(tupdesc, c)->attname);

    JSValue o = JS_GetPropertyStr(ctx, val, colname);

    /* Owned reference: release it on both paths.  See pljs_jsvalue_to_datums().
     */
    if (TupleDescAttr(tupdesc, c)->attgenerated != '\0' &&
        (JS_IsNull(o) || JS_IsUndefined(o))) {
      /*
       * A generated column is NULL in a BEFORE trigger's NEW, since the
       * executor computes it after the trigger has run.  That NULL is not a
       * value to check against the column's domain: a NOT NULL domain
       * rejected it, so a trigger that returned NEW failed on every row.
       */
      JS_FreeValue(ctx, o);
      nulls[c] = true;
      continue;
    }

    values[c] = pljs_jsvalue_to_datum_typmod(
        TupleDescAttr(tupdesc, c)->atttypid,
        TupleDescAttr(tupdesc, c)->atttypmod, o, &nulls[c], ctx, NULL);

    JS_FreeValue(ctx, o);
  }

  // Form a Tuple from the values and nulls using the tuple descriptor
  // as the template for the tuple.
  result = HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls));

  pfree(nulls);
  pfree(values);

  if (cleanup_tupdesc) {
    ReleaseTupleDesc(tupdesc);
  }

  return result;
}

/**
 * @brief Fallback type conversion from @JSValue to @Datum.
 *
 * The reverse of pljs_datum_to_jsvalue_fallback(), and the reason that one
 * can be trusted: the value is parsed by the target type's own text input
 * function, so what lands in the column is a datum the type built and
 * validated itself.
 *
 * What this replaces wrote the JavaScript string's bytes directly over the
 * type's internal representation -- memcpy into a varlena for a
 * variable-length type, into a palloc0() of typlen for a fixed-length one,
 * and JS_ToInt32() for pass-by-value.  Nothing checked that those bytes were
 * a legal value of the type, and for anything whose storage is not text they
 * were not: see pljs_datum_to_jsvalue_fallback() for the uuid corruption and
 * the varbit_out() crash that came out of it.
 *
 * Handling the pass-by-value case the same way matters as much as the rest.
 * An enum is a 4-byte pass-by-value OID, so the old code round-tripped it
 * only because both directions agreed to use the raw OID; JavaScript saw
 * 16400 instead of 'happy'. Now it sees the label and the input function maps
 * it back, which also means a label that does not exist raises rather than
 * storing an OID from some other enum.
 *
 * A domain over such a type is converted here too, as plv8 converts every
 * domain: its input function is domain_in(), which parses with the base type's
 * input function using the domain's typmod and then checks the domain's
 * constraints.
 *
 * @param value #JSValue - Javascript to convert
 * @param is_null @c bool - pointer to fill of whether the value is null
 * @param io #pljs_type_io - cached conversion state for the target type
 * @param typmod @c int32 - the target's typmod, or -1
 * @param ctx #JSContext - Javascript context
 * @returns #Datum conversion of the JSValue
 */
static Datum pljs_jsvalue_to_datum_fallback(JSValue value, bool *is_null,
                                            pljs_type_io *io, int32 typmod,
                                            JSContext *ctx) {
  // Set whether the Datum is `NULL` or not.
  JSValue is_set_null_value = JS_GetPropertyStr(ctx, value, "is_null");

  *is_null = JS_ToBool(ctx, is_set_null_value);
  JS_FreeValue(ctx, is_set_null_value);

  // If the value's property of `null` is set to `true`, we return an empty
  // Datum.
  if (*is_null) {
    return (Datum)0;
  }

  return pljs_jsvalue_to_datum_via_io(io, value, typmod, ctx);
}

/*
 * Return a SQL NULL from a conversion without touching fcinfo.
 *
 * PG_RETURN_NULL() expands to `fcinfo->isnull = true; return (Datum) 0`, so it
 * only works where fcinfo is real.  pljs_jsvalue_to_datum() is also called for
 * every column of a composite -- from pljs_jsvalue_to_datums() and
 * pljs_jsvalue_to_record() -- and both of those pass fcinfo == NULL, reporting
 * the null through the is_null argument instead.  Using PG_RETURN_NULL() on
 * those paths writes through a null pointer.
 */
static inline Datum pljs_null_datum(bool *is_null, FunctionCallInfo fcinfo) {
  if (is_null != NULL) {
    *is_null = true;
  }

  if (fcinfo != NULL) {
    fcinfo->isnull = true;
  }

  return (Datum)0;
}

/**
 * @brief Converts a JavaScript value into a #Datum through a cached input
 * function.
 *
 * The value is stringified and parsed by the type's own text input function,
 * with @p typmod, so the type validates it -- and applies its length or
 * precision -- exactly as it would a literal.
 *
 * @param io #pljs_type_io - cached conversion state for the target type
 * @param val #JSValue - the JavaScript value to parse
 * @param typmod @c int32 - the target's typmod, or -1
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Datum parsed from the string
 */
static Datum pljs_jsvalue_to_datum_via_io(pljs_type_io *io, JSValueConst val,
                                          int32 typmod, JSContext *ctx) {
  size_t plen;
  const char *str = JS_ToCStringLen(ctx, &plen, val);
  FmgrInfo *input;
  Datum ret;

  if (str == NULL) {
    elog(ERROR, "could not convert JavaScript value to a string");
  }

  if (memchr(str, '\0', plen) != NULL) {
    JS_FreeCString(ctx, str);
    ereport(ERROR,
            (errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
             errmsg("null byte (\\u0000) is not allowed in a value of type %s",
                    format_type_be(io->typid))));
  }

  PG_TRY();
  {
    if (io->is_domain) {
      ret = pljs_domain_check(io, (char *)str, (Datum)0, false);
    } else {
      input = pljs_type_io_input(io);
      ret = InputFunctionCall(input, (char *)str, io->ioparam, typmod);
    }
  }
  PG_CATCH();
  {
    /* Do not leak the QuickJS C-string when the input function rejects it. */
    JS_FreeCString(ctx, str);
    PG_RE_THROW();
  }
  PG_END_TRY();

  JS_FreeCString(ctx, str);

  return ret;
}

/**
 * @brief Converts a JavaScript string into a #Datum through the target type's
 * text input function.
 *
 * The input function parses the full decimal text exactly, and raises on
 * malformed or out-of-range input.  That is the only correct way to turn a
 * *string* into a numeric datum: QuickJS's numeric coercion
 * (JS_ToInt32/JS_ToInt64/JS_ToFloat64) goes through an IEEE-754 double, which
 * silently loses precision above 2^53 -- "9223372036854775807" arrives as
 * INT64_MIN, and "123456789012345678" lands two off.
 *
 * It is also what plv8 does, so a procedure that binds a numeric string behaves
 * the same on both.
 *
 * @param typid #Oid - target type
 * @param val #JSValue - the JavaScript string to parse
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Datum parsed from the string
 */
static Datum pljs_string_to_datum_via_input(Oid typid, JSValueConst val,
                                            JSContext *ctx) {
  return pljs_jsvalue_to_datum_via_io(pljs_type_io_lookup(typid), val, -1, ctx);
}

/**
 * @brief Raises the standard out-of-range error for an integer target type.
 */
static void pljs_int_out_of_range(Oid typid) {
  ereport(ERROR,
          (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
           errmsg("value is out of range for type %s", format_type_be(typid))));
}

/**
 * @brief Converts a JavaScript number to an integer, rejecting values the
 * target type cannot represent.
 *
 * QuickJS's JS_ToInt32/JS_ToInt64 wrap modulo the word size, so 2147483648
 * silently became -2147483648, 40000 became -25536 for a smallint, and NaN and
 * Infinity both became 0.  PostgreSQL raises "integer out of range" for the
 * equivalent cast, and silently storing a different number is the worst outcome
 * for a data pipeline, so range-check instead.
 *
 * Fractions keep truncating toward zero, which is the established JavaScript
 * conversion behaviour; only the range is newly enforced.
 *
 * @param min @c double - lowest representable value
 * @param max_exclusive @c double - one past the highest.  Taken as a double
 *   because (double) INT64_MAX rounds *up* to 2^63, so an integer comparison
 *   would wrongly accept 2^63 itself.
 */
static int64 pljs_number_to_int_checked(JSContext *ctx, JSValueConst val,
                                        double min, double max_exclusive,
                                        Oid typid) {
  double d;

  if (JS_ToFloat64(ctx, &d, val) < 0) {
    elog(ERROR, "could not convert JavaScript value to a number");
  }

  if (isnan(d)) {
    ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("cannot convert NaN to type %s", format_type_be(typid))));
  }

  if (isinf(d)) {
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("cannot convert Infinity to type %s",
                           format_type_be(typid))));
  }

  d = trunc(d);

  if (d < min || d >= max_exclusive) {
    pljs_int_out_of_range(typid);
  }

  return (int64)d;
}

/**
 * @brief Converts a JavaScript BigInt to an int64, rejecting values that do not
 * fit.
 *
 * JS_ToBigInt64() truncates to the low 64 bits without reporting anything, so
 * 2n**70n arrived as 0.  Round-tripping the result through JS_NewBigInt64()
 * detects that silently discarded magnitude.
 */
static int64 pljs_bigint_to_int64_checked(JSContext *ctx, JSValueConst val,
                                          Oid typid) {
  int64_t v;
  JSValue back;
  bool same;

  if (JS_ToBigInt64(ctx, &v, val) < 0) {
    elog(ERROR, "could not convert JavaScript BigInt to an integer");
  }

  back = JS_NewBigInt64(ctx, v);
  same = JS_StrictEq(ctx, back, val);
  JS_FreeValue(ctx, back);

  if (!same) {
    pljs_int_out_of_range(typid);
  }

  return v;
}

/**
 * @brief Converts a Javascript value to a Postgres #Datum.
 *
 * Takes a Javascript value and converts it into a Postgres #Datum,
 * checking whether it is an array or record and converting it
 * properly.
 *
 * Called with the base type for a domain; see pljs_jsvalue_to_datum_typmod(),
 * which resolves the domain and then validates what this builds.
 *
 * @param io #pljs_type_io - cached conversion state for the target type
 * @param typmod @c int32 - passed to an array's elements and to the fallback's
 * input function; a type with a case of its own has it applied afterwards, by
 * pljs_jsvalue_to_datum_typmod()
 * @param val #JSValue - the Javascript object to convert
 * @param is_null @c bool* - pointer to fill with whether the result is null
 * @param ctx #JSContext - Javascript context to execute in
 * @param fcinfo #FunctionCallInfo - optional, can be NULL
 * @returns #Datum of the Postgres value
 */
static Datum pljs_jsvalue_to_datum_internal(pljs_type_io *io, int32 typmod,
                                            JSValue val, bool *is_null,
                                            JSContext *ctx,
                                            FunctionCallInfo fcinfo) {
  Oid rettype = io->typid;

  // Initialize is_null to false
  if (is_null) {
    *is_null = false;
  }

  pljs_type type;

  pljs_type_fill_io(&type, io);

  /*
   * Decide "build a SQL array" or "build a JSON array" from the SQL type's
   * category, not from type.typid: pljs_type_fill() has already rewritten
   * type.typid to the ELEMENT type for any array type, so comparing it against
   * JSONOID/JSONBOID here also matched jsonb[] and json[], whose element type
   * *is* json or jsonb.
   *
   * Those fell through to the scalar json branch, which stringified the whole
   * JavaScript array -- "[object Object],[object Object]" -- and handed that to
   * the json input function. The array construction below never ran, and what
   * surfaced was a cache lookup failure for a type OID read out of
   * uninitialised memory, so `RETURNS jsonb[]` has never worked for any element
   * shape.
   *
   * A bare json/jsonb target still turns a JavaScript array into a JSON array,
   * which is what the original condition was for.
   */
  if (JS_IsArray(ctx, val) && type.category == TYPCATEGORY_ARRAY) {
    return pljs_jsvalue_to_array(&type, val, ctx, typmod);
  }

  /*
   * A JavaScript array aimed at something that is neither an array type nor
   * json/jsonb.
   *
   * This dispatched into the array conversion anyway, because the condition
   * above used to read "not json/jsonb", which is true of every scalar. It
   * built an array Datum and returned it as the scalar. For a *nested* array
   * that is silent corruption rather than an error: the element loop converts
   * each element to the element type, so `return [[1,2],[3,4]]` for int[]
   * yielded {357119344,357119392} -- the ArrayType pointers of the two inner
   * arrays, reinterpreted as int4.
   *
   * The other direction already refuses a multidimensional array with a clear
   * message. This makes the output direction agree, rather than producing
   * numbers that look like data.
   */
  if (JS_IsArray(ctx, val) && type.typid != JSONOID && type.typid != JSONBOID) {
    ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("cannot convert a JavaScript array to %s",
                    format_type_be(rettype)),
             errdetail("pljs represents SQL arrays as one-dimensional "
                       "JavaScript arrays; a nested array is only valid for "
                       "json or jsonb.")));
  }

  if (type.category == TYPCATEGORY_ARRAY && !JS_IsArray(ctx, val)) {
    elog(ERROR, "value is not an Array");
  }

  if (type.is_composite) {
    return pljs_jsvalue_to_record(&type, val, is_null, NULL, ctx);
  }

  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    return pljs_null_datum(is_null, fcinfo);
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
    /*
     * A string is parsed by bool's input function, not coerced with
     * JS_ToBool(). JS_ToBool() reports every non-empty string as true, so
     * "false", "f", "no" and "0" all became true while "" became false —
     * the opposite of what the text means. The input function accepts
     * exactly what SQL accepts and raises on anything else.
     *
     * This is a behaviour change: a bind or return of the string "false"
     * used to store true. A JavaScript boolean, and the truthiness of any
     * non-string, are untouched.
     */
    if (JS_IsString(val)) {
      return pljs_string_to_datum_via_input(BOOLOID, val, ctx);
    }

    int8_t in = JS_ToBool(ctx, val);
    PG_RETURN_BOOL(in);
    break;
  }

  case INT2OID: {
    int64 v;

    /* A string carries exact decimal text; parse it, do not go via a double. */
    if (JS_IsString(val)) {
      return pljs_string_to_datum_via_input(INT2OID, val, ctx);
    }

    if (JS_IsBigInt(ctx, val)) {
      v = pljs_bigint_to_int64_checked(ctx, val, INT2OID);
    } else {
      v = pljs_number_to_int_checked(ctx, val, (double)PG_INT16_MIN,
                                     -(double)PG_INT16_MIN, INT2OID);
    }

    if (v < PG_INT16_MIN || v > PG_INT16_MAX) {
      pljs_int_out_of_range(INT2OID);
    }

    PG_RETURN_INT16((int16)v);
    break;
  }

  case INT4OID: {
    int64 v;

    if (JS_IsString(val)) {
      return pljs_string_to_datum_via_input(INT4OID, val, ctx);
    }

    if (JS_IsBigInt(ctx, val)) {
      v = pljs_bigint_to_int64_checked(ctx, val, INT4OID);
    } else {
      v = pljs_number_to_int_checked(ctx, val, (double)PG_INT32_MIN,
                                     -(double)PG_INT32_MIN, INT4OID);
    }

    if (v < PG_INT32_MIN || v > PG_INT32_MAX) {
      pljs_int_out_of_range(INT4OID);
    }

    PG_RETURN_INT32((int32)v);
    break;
  }

  case INT8OID: {
    int64 v;

    if (JS_IsString(val)) {
      return pljs_string_to_datum_via_input(INT8OID, val, ctx);
    }

    if (JS_IsBigInt(ctx, val)) {
      v = pljs_bigint_to_int64_checked(ctx, val, INT8OID);
    } else {
      /*
       * -(double) PG_INT64_MIN is exactly 2^63, one past INT64_MAX, and is the
       * correct exclusive bound: (double) PG_INT64_MAX rounds up to the same
       * value, so comparing against it would accept 2^63 itself.
       */
      v = pljs_number_to_int_checked(ctx, val, (double)PG_INT64_MIN,
                                     -(double)PG_INT64_MIN, INT8OID);
    }

    PG_RETURN_INT64(v);
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
    /*
     * A string carries exact decimal text, including a scale no double can
     * represent, so parse it with numeric's input function rather than routing
     * it through float8: "12345678901234567890.123456789" came back as
     * 12345678901234600000.
     */
    if (JS_IsString(val)) {
      return pljs_string_to_datum_via_input(NUMERICOID, val, ctx);
    }

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

  case NAMEOID: {
    /*
     * `name` is a fixed-length NameData -- NAMEDATALEN bytes, no varlena header
     * -- so it cannot be built the way text/varchar/bpchar are below. Doing
     * that writes a varlena length word into the first bytes of the name, and
     * every comparison against a real name then reads that as characters: a
     * catalog lookup by nspname, relname or typname matches nothing at all,
     * silently. namein() lays the value out correctly and applies the
     * truncation rule for anything longer than NAMEDATALEN - 1.
     */
    const char *str = JS_ToCString(ctx, val);
    Datum ret;

    if (str == NULL) {
      elog(ERROR, "could not convert JavaScript value to a string");
    }

    PG_TRY();
    {
      ret = DirectFunctionCall1(namein, CStringGetDatum(str));
    }
    PG_CATCH();
    {
      /* Do not leak the QuickJS C-string if namein() rejects the value. */
      JS_FreeCString(ctx, str);
      PG_RE_THROW();
    }
    PG_END_TRY();

    JS_FreeCString(ctx, str);

    return ret;
  }

  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID:
  case XMLOID: {
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, val);

    /*
     * JS_ToCStringLen() returns NULL when the value cannot be rendered -- an
     * out-of-memory under pljs.memory_limit, or a toString() that threw.
     * CStringGetTextDatum() would hand that straight to strlen().
     */
    if (str == NULL) {
      elog(ERROR, "could not convert JavaScript value to a string");
    }

    /*
     * A PostgreSQL text value cannot carry an embedded NUL, and
     * CStringGetTextDatum() measures with strlen(): "a\u0000b" was stored as
     * "a", three characters silently becoming one. Truncating a value at a
     * caller-supplied byte is how a string that passed an application's
     * validation becomes a different string in the table, so reject it rather
     * than write the prefix.
     */
    if (memchr(str, '\0', plen) != NULL) {
      JS_FreeCString(ctx, str);
      ereport(ERROR,
              (errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
               errmsg("null byte (\\u0000) is not allowed in a text value")));
    }

    Datum ret = PointerGetDatum(cstring_to_text_with_len(str, plen));
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
      PG_RETURN_JSONB_P(obj);
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

      for (size_t i = 0; i < length; i++) {
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

      /*
       * Not a string, ArrayBuffer, or typed array, so there is no byte
       * representation to store. This used to bind SQL NULL, which turns
       * passing the wrong thing -- a number, a plain object, a Float64Array --
       * into a row that looks like the caller meant to write nothing.
       */
      ereport(ERROR,
              (errcode(ERRCODE_DATATYPE_MISMATCH),
               errmsg("cannot convert JavaScript value to bytea"),
               errdetail("Expected a string, ArrayBuffer, or typed array.")));
    }
  }

  case DATEOID:
  case TIMESTAMPOID:
  case TIMESTAMPTZOID:
    if (Is_Date(val)) {
      double in;
      JS_ToFloat64(ctx, &in, val);

      /*
       * An invalid Date -- one whose getTime() is NaN -- has no epoch to
       * convert. It is not an exotic thing to hold: reading
       * 'infinity'::timestamptz back into JavaScript produces exactly that, so
       * a read-modify-write of a row with an infinite timestamp reaches here.
       *
       * The arithmetic below turns NaN into a finite number, and the value
       * stored was 2000-01-01 -- the PostgreSQL epoch, i.e. an offset of zero.
       * A real date, silently, where the caller had no date at all. Bind SQL
       * NULL instead.
       */
      if (isnan(in)) {
        return pljs_null_datum(is_null, fcinfo);
      }

      if (rettype == DATEOID) {
        return pljs_convert_epoch_to_date(in);
      }

      return pljs_convert_epoch_to_timestamptz(in);
    } else {
      /*
       * Not a Date object. Only Is_Date() was handled, so everything else --
       * including a perfectly valid ISO string like "2020-01-02 03:04:05" --
       * fell through and became SQL NULL, discarding a value the caller
       * plainly meant. Parse it with the target type's own input function, so
       * a valid string lands as the date it names and an invalid one raises
       * instead of vanishing.
       */
      return pljs_string_to_datum_via_input(rettype, val, ctx);
    }

  default:
    return pljs_jsvalue_to_datum_fallback(val, is_null, io, typmod, ctx);
  }

  /*
   * Reached only if a case above breaks out of the switch without returning.
   * The date/timestamp cases used to do exactly that for any non-Date value,
   * which is how a valid date string became SQL NULL; they now parse through
   * the type's input function instead, so nothing routes here today.
   *
   * Kept as a defined outcome rather than an assertion: a case added later that
   * breaks instead of returning gets SQL NULL, not a fall off the end of a
   * non-void function.
   */
  return pljs_null_datum(is_null, fcinfo);
}

/**
 * @brief Converts a Javascript value to a (non-domain) type and applies a
 * typmod.
 *
 * @param io #pljs_type_io - cached conversion state for the type
 * @param typmod @c int32 - the typmod to apply, or -1
 * @param val #JSValue - the Javascript value to convert
 * @param isnull @c bool* - set to whether the result is SQL NULL
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Datum of the Postgres value
 */
static Datum pljs_jsvalue_to_base(pljs_type_io *io, int32 typmod, JSValue val,
                                  bool *isnull, JSContext *ctx) {
  Datum ret =
      pljs_jsvalue_to_datum_internal(io, typmod, val, isnull, ctx, NULL);

  if (!*isnull && io->has_js_case) {
    ret = pljs_apply_typmod(io, ret, typmod);
  }

  return ret;
}

/**
 * @brief Converts a Javascript value to a Postgres #Datum of a given typmod.
 *
 * A type that is not a domain is converted directly.  A domain takes one of
 * two routes, depending on its base type:
 *
 *   - A base type with a case of its own -- int4, boolean, jsonb, bytea,
 *     timestamp, an array, a composite -- is built as that base type, so a
 *     domain over jsonb takes an object, a domain over bytea a typed array and
 *     a domain over timestamp a Date, none of which a domain's own OID would
 *     reach.  The base type's typmod is applied, and domain_check() then
 *     enforces the domain's NOT NULL and CHECK constraints.
 *
 *   - Any other base type goes through the domain's own input function,
 *     domain_in(), as plv8 does for every domain.  That parses the text with
 *     the base type's input function using the domain's typmod and checks the
 *     constraints, and unlike domain_check() it needs no binary receive
 *     function, which not every type has.
 *
 * A SQL NULL -- JavaScript's null or undefined -- is checked against a domain
 * too, and is handled here for every type, so no caller needs a null path of
 * its own.  An array type used to see a NULL result as "value is not an
 * Array".
 *
 * The result's nullness is always reported through both @p is_null and
 * @p fcinfo.  A composite, or the fallback's `{is_null: true}`, reported it
 * only through @p is_null, and call_function() -- which reads fcinfo -- then
 * returned a zero pointer as a non-null value, which crashed the backend.
 *
 * @param typid #Oid - the target type
 * @param typmod @c int32 - the target's typmod, or -1; ignored for a domain,
 * which carries its own
 * @param val #JSValue - the Javascript value to convert
 * @param is_null @c bool* - optional, set to whether the result is null
 * @param ctx #JSContext - Javascript context to execute in
 * @param fcinfo #FunctionCallInfo - optional, can be NULL
 * @returns #Datum of the Postgres value
 */
static Datum pljs_jsvalue_to_datum_typmod(Oid typid, int32 typmod, JSValue val,
                                          bool *is_null, JSContext *ctx,
                                          FunctionCallInfo fcinfo) {
  pljs_type_io *io;
  bool isnull = false;
  Datum ret = (Datum)0;

  /*
   * A void function returns void, never NULL, whatever it returned.  void is
   * a pseudotype, which the conversion treats as a composite: `return;` came
   * back as SQL NULL, and any other value raised "type void is not
   * composite".
   */
  if (typid == VOIDOID) {
    if (is_null != NULL) {
      *is_null = false;
    }

    return (Datum)0;
  }

  io = pljs_type_io_lookup(typid);

  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    pljs_domain_check_null(io);

    return pljs_null_datum(is_null, fcinfo);
  }

  if (!io->is_domain) {
    ret = pljs_jsvalue_to_base(io, typmod, val, &isnull, ctx);
  } else if (io->domain_via_input) {
    /* A non-null value is parsed and checked by domain_in(). */
    ret = pljs_jsvalue_to_datum_fallback(val, &isnull, io, -1, ctx);

    if (isnull) {
      pljs_domain_check_null(io);
    }
  } else {
    ret = pljs_jsvalue_to_base(pljs_type_io_lookup(io->basetype),
                               io->basetypmod, val, &isnull, ctx);

    pljs_domain_check(io, NULL, ret, isnull);
  }

  if (isnull) {
    return pljs_null_datum(is_null, fcinfo);
  }

  if (is_null != NULL) {
    *is_null = false;
  }

  return ret;
}

/**
 * @brief Converts a Javascript value to a Postgres #Datum.
 *
 * See pljs_jsvalue_to_datum_typmod(); this is the form for a target without a
 * typmod, which is every target except a column.
 *
 * @param rettype #Oid - the target type
 * @param val #JSValue - the Javascript object to convert
 * @param is_null @c bool* - pointer to fill with whether the result is null
 * @param ctx #JSContext - Javascript context to execute in
 * @param fcinfo #FunctionCallInfo - optional, can be NULL
 * @returns #Datum of the Postgres value
 */
Datum pljs_jsvalue_to_datum(Oid rettype, JSValue val, bool *is_null,
                            JSContext *ctx, FunctionCallInfo fcinfo) {
  return pljs_jsvalue_to_datum_typmod(rettype, -1, val, is_null, ctx, fcinfo);
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

    JS_SetPropertyStr(
        ctx, obj, name,
        pljs_datum_to_jsvalue(tuple_attrs->atttypid, datum, isnull, true, ctx));
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
    return js_throw(pljs_util_spi_status_string(status), ctx);
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
    return JS_NewFloat64(ctx, pljs_numeric_to_double(
                                  PointerGetDatum(scalar_value->val.numeric)));
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

#if PG_VERSION_NUM >= 190000
typedef JsonbInState JsonbBuildState;
static JsonbValue *jsonb_push(JsonbBuildState *pstate, JsonbIteratorToken seq,
                              JsonbValue *jbval) {
  pushJsonbValue(pstate, seq, jbval);
  return pstate->result;
}
#else
typedef JsonbParseState *JsonbBuildState;
static JsonbValue *jsonb_push(JsonbBuildState *pstate, JsonbIteratorToken seq,
                              JsonbValue *jbval) {
  return pushJsonbValue(pstate, seq, jbval);
}
#endif

// Forward declarations of the conversion functions.
static JsonbValue *jsonb_object_from_object(JSValue object,
                                            JsonbBuildState *pstate,
                                            JSContext *ctx);
static JsonbValue *
jsonb_array_from_array(JSValue array, JsonbBuildState *pstate, JSContext *ctx);

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
 * @param pstate #JsonbBuildState - current state of the `JSONB` parsing
 * @param value #JSValue - the value to convert
 * @param type #JsonbIteratorToken
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JsonbValue `JSONB` result from the conversion
 */
static JsonbValue *jsonb_from_value(JSValue value, JsonbBuildState *pstate,
                                    JsonbIteratorToken type, JSContext *ctx,
                                    const char *key) {
  JsonbValue val;

  // If the token type is a key, the only valid value is `jbvString`.
  if (type == WJB_KEY) {
    val.type = jbvString;
    size_t len = strlen(key);

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
  return jsonb_push(pstate, type, &val);
}

/**
 * @brief Converts a #JSValue `Array` to a #JsonbValue array.
 *
 * @param array #JSValue - `Array` to convert
 * @param pstate #JsonbBuildState - the parse state of the `JSONB` object
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JsonbValue of the `JSONB` array
 */
static JsonbValue *
jsonb_array_from_array(JSValue array, JsonbBuildState *pstate, JSContext *ctx) {
  // Push the beginning of the array into the parse state.
  JsonbValue *value = jsonb_push(pstate, WJB_BEGIN_ARRAY, NULL);

  // Get the length of the `Array`.
  int32_t array_length = pljs_js_array_length(array, ctx);

  // Iterate through the `Array`.
  for (int i = 0; i < array_length; i++) {
    // Get the current element.
    JSValue elem = JS_GetPropertyUint32(ctx, array, i);

    // For each type, set `value` to the result.
    if (JS_IsArray(ctx, elem)) {
      value = jsonb_array_from_array(elem, pstate, ctx);
    } else if (JS_IsObject(elem)) {
      value = jsonb_object_from_object(elem, pstate, ctx);
    } else {
      value = jsonb_from_value(elem, pstate, WJB_ELEM, ctx, NULL);
    }

    // Free up the element.
    JS_FreeValue(ctx, elem);
  }

  // Set the value to the end of the array.
  value = jsonb_push(pstate, WJB_END_ARRAY, NULL);

  return value;
}

/**
 * @brief Converts a #JSValue `Object` to a #JsonbValue object.
 *
 * @param object #JSValue - `Object` to convert
 * @param pstate #JsonbBuildState - the parse state of the `JSONB` object
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JsonbValue of the `JSONB` object
 */
static JsonbValue *jsonb_object_from_object(JSValue object,
                                            JsonbBuildState *pstate,
                                            JSContext *ctx) {
  // Push the beginning of the object intp the parse state.
  JsonbValue *value = jsonb_push(pstate, WJB_BEGIN_OBJECT, NULL);
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

    const char *key = JS_AtomToCString(ctx, tab[object_key].atom);

    value = jsonb_from_value(o, pstate, WJB_KEY, ctx, key);

    // If the value is an `Array` the convert it.
    if (JS_IsArray(ctx, o)) {
      value = jsonb_array_from_array(o, pstate, ctx);
    } else if (JS_IsObject(o)) {
      // Or convert an `Object`.
      value = jsonb_object_from_object(o, pstate, ctx);
    } else {
      // Or anything else.
      value = jsonb_from_value(o, pstate, WJB_VALUE, ctx, NULL);
    }

    // Free up the memory.
    JS_FreeValue(ctx, o);
  }

  pljs_free_prop_enum(ctx, tab, object_keys_length);

  // Push that we are at the end of an object.
  value = jsonb_push(pstate, WJB_END_OBJECT, NULL);

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

  JsonbBuildState parse_state = {0};
  JsonbValue *value;

  // Check the type and get its value.
  if (JS_IsArray(ctx, object)) {
    value = jsonb_array_from_array(object, &parse_state, ctx);
  } else if (JS_IsObject(object)) {
    value = jsonb_object_from_object(object, &parse_state, ctx);
  } else {
    jsonb_push(&parse_state, WJB_BEGIN_ARRAY, NULL);
    jsonb_from_value(object, &parse_state, WJB_ELEM, ctx, NULL);
    value = jsonb_push(&parse_state, WJB_END_ARRAY, NULL);
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
