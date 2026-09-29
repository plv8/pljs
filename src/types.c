#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/date.h"
#include "utils/datum.h"
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
#if JSONB_DIRECT_CONVERSION
static JSClassID JS_CLASS_NUMBER = 4;
static JSClassID JS_CLASS_STRING = 5;
static JSClassID JS_CLASS_BOOLEAN = 6;
#endif

/*
 * A BigInt object's -- Object(10n) -- which is not written down as the others
 * are: it comes after every typed array's in QuickJS's list of classes, and
 * moves when QuickJS adds one.  0 until the first JSContext is set up; see
 * pljs_type_classes_init().
 */
static JSClassID JS_CLASS_BIG_INT = 0;

/*
 * toString, valueOf and Symbol.toPrimitive, which an object converts itself
 * with; see pljs_converts_itself().  Set by pljs_type_classes_init().
 */
static JSAtom pljs_conversion_atoms[3] = {JS_ATOM_NULL, JS_ATOM_NULL,
                                          JS_ATOM_NULL};

/* A Proxy's, for the same reason; see pljs_row_column(). */
static JSClassID JS_CLASS_PROXY = 0;
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
/*
 * The brand checks below compare the object's class id.  They used
 * JS_GetOpaque(), which returns the class's payload rather than whether it
 * has one: a typed array's or ArrayBuffer's is a pointer, so that happened to
 * work, but a Date's is its time value, and the epoch's is all zero bits.  So
 * `new Date(0)` was not a Date -- a date or timestamp column parsed its
 * toString() instead, which fails outside UTC, and jsonb stored that string
 * rather than an ISO timestamp.
 */
inline static bool Is_ArrayType(JSValueConst obj, JSClassID class_id) {
  return JS_GetClassID(obj) == class_id;
}

// if given object is array buffer.
inline static bool Is_ArrayBuffer(JSValueConst obj) {
  return JS_GetClassID(obj) == JS_CLASS_ARRAY_BUFFER;
}

// if given object is shared array buffer.
inline static bool Is_SharedArrayBuffer(JSValueConst obj) {
  return JS_GetClassID(obj) == JS_CLASS_SHARED_ARRAY_BUFFER;
}

// if this is an actual object of any sort.
inline static bool Is_Object(JSValueConst obj) {
  return JS_GetClassID(obj) == JS_CLASS_OBJECT;
}

// if given object is a date.
inline static bool Is_Date(JSValueConst obj) {
  return JS_GetClassID(obj) == JS_CLASS_DATE;
}

/*
 * Release a property-name enumeration obtained from JS_GetOwnPropertyNames().
 *
 * QuickJS hands the caller both the array and a reference on every atom in it,
 * and expects both back; its own js_free_prop_enum() is static, so this is the
 * public-API equivalent.  Without it every enumerated object leaked its keys
 * for the life of the backend.
 */
void pljs_free_prop_enum(JSContext *ctx, JSPropertyEnum *tab, uint32_t len) {
  if (tab == NULL) {
    return;
  }

  for (uint32_t i = 0; i < len; i++) {
    JS_FreeAtom(ctx, tab[i].atom);
  }

  js_free(ctx, tab);
}

/**
 * @brief Reads the class ids that are not written down; see
 * JS_CLASS_BIG_INT.
 *
 * Class ids belong to the runtime, so once is enough.  From a context that no
 * JavaScript has run in yet, whose Object() is the builtin one.
 *
 * @param ctx #JSContext - a new context
 */
void pljs_type_classes_init(JSContext *ctx) {
  JSValue global, object, bigint, boxed, proxy_ctor, proxy;
  JSValue proxy_args[2];
  JSValue symbol, to_primitive;

  if (JS_CLASS_BIG_INT != 0) {
    return;
  }

  global = JS_GetGlobalObject(ctx);

  /*
   * The keys an object converts itself with, which are the runtime's; see
   * pljs_converts_itself().  Symbol.toPrimitive was read through the global
   * Symbol for every row, which user code can replace or delete.
   */
  symbol = JS_GetPropertyStr(ctx, global, "Symbol");
  to_primitive = JS_GetPropertyStr(ctx, symbol, "toPrimitive");
  pljs_conversion_atoms[0] = JS_NewAtom(ctx, "toString");
  pljs_conversion_atoms[1] = JS_NewAtom(ctx, "valueOf");
  pljs_conversion_atoms[2] = JS_IsSymbol(to_primitive)
                                 ? JS_ValueToAtom(ctx, to_primitive)
                                 : JS_ATOM_NULL;
  JS_FreeValue(ctx, to_primitive);
  JS_FreeValue(ctx, symbol);
  object = JS_GetPropertyStr(ctx, global, "Object");
  bigint = JS_NewBigInt64(ctx, 0);
  boxed = JS_Call(ctx, object, JS_UNDEFINED, 1, (JSValueConst *)&bigint);

  proxy_ctor = JS_GetPropertyStr(ctx, global, "Proxy");
  proxy_args[0] = JS_NewObject(ctx);
  proxy_args[1] = JS_NewObject(ctx);
  proxy = JS_CallConstructor(ctx, proxy_ctor, 2, (JSValueConst *)proxy_args);

  JS_FreeValue(ctx, proxy_args[0]);
  JS_FreeValue(ctx, proxy_args[1]);
  JS_FreeValue(ctx, proxy_ctor);
  JS_FreeValue(ctx, bigint);
  JS_FreeValue(ctx, object);
  JS_FreeValue(ctx, global);

  if (JS_IsException(boxed) || JS_IsException(proxy)) {
    JS_FreeValue(ctx, boxed);
    JS_FreeValue(ctx, proxy);
    pljs_ereport_js_exception(ctx);
  }

  JS_CLASS_PROXY = JS_GetClassID(proxy);
  JS_CLASS_BIG_INT = JS_GetClassID(boxed);

  JS_FreeValue(ctx, boxed);
  JS_FreeValue(ctx, proxy);
}

/**
 * @brief Raises the exception QuickJS left pending if it returned
 * JS_EXCEPTION.
 *
 * For a value QuickJS was asked to make or read: a getter or toJSON() that
 * threw, or a value it could not make.  The conversions from PostgreSQL report
 * a value they cannot make by raising, and QuickJS reports one by returning
 * JS_EXCEPTION: JSON nested past its stack limit, or a string, object or
 * bytea buffer that pljs.memory_limit has no room for.  Kept as the value --
 * an argument, a column, an array element -- it reached JavaScript as a value
 * that is not one, with the exception still pending: a json argument nested
 * too deep to parse had a typeof "unknown".
 *
 * @param ctx #JSContext - Javascript context
 * @param value #JSValue - what QuickJS returned
 * @returns #JSValue - @p value, which is not JS_EXCEPTION
 */
static JSValue pljs_checked_value(JSContext *ctx, JSValue value) {
  if (JS_IsException(value)) {
    pljs_ereport_js_exception(ctx);
  }

  return value;
}

/**
 * @brief Raises if QuickJS could not define a property of a value it is
 * building from a PostgreSQL one.
 *
 * Defining a property of a new object or array fails only for want of memory,
 * and the property was left out without a word; see pljs_checked_value().
 *
 * @param ctx #JSContext - Javascript context
 * @param ret @c int - what JS_DefinePropertyValue*() returned
 */
static void pljs_checked_define(JSContext *ctx, int ret) {
  if (ret < 0) {
    pljs_ereport_js_exception(ctx);
  }
}

/**
 * @brief Makes a JavaScript property key of a column's name.
 *
 * A key is UTF-8, as QuickJS reads it, and a column's name is in the
 * database's encoding.  In a LATIN1 database a row read from a query had a
 * key with U+FFFD for each accented letter of its column's name, and an
 * object written back had no property for that column.
 *
 * @param ctx #JSContext - Javascript context
 * @param attr #Form_pg_attribute - the column
 * @returns #JSAtom - the key, which the caller frees, or JS_ATOM_NULL with an
 * exception pending
 */
JSAtom pljs_column_atom(JSContext *ctx, Form_pg_attribute attr) {
  return pljs_name_atom(ctx, NameStr(attr->attname));
}

/**
 * @brief Returns the key for a name in the database's encoding; see
 * pljs_column_atom().
 *
 * @param ctx #JSContext - Javascript context
 * @param name @c char* - the name
 * @returns #JSAtom - owned, or JS_ATOM_NULL with an exception pending
 */
JSAtom pljs_name_atom(JSContext *ctx, const char *name) {
  char *key = pljs_server_to_utf8(name, strlen(name));
  JSAtom atom = JS_NewAtom(ctx, key);

  if (key != name) {
    pfree(key);
  }

  return atom;
}

/**
 * @brief Defines a row object's property for a column.
 *
 * Defined, not set; see pljs_datum_to_object().  Keyed in UTF-8; see
 * pljs_column_atom().
 *
 * @param ctx #JSContext - Javascript context
 * @param obj #JSValueConst - the row object
 * @param attr #Form_pg_attribute - the column
 * @param value #JSValue - the column's value, which is taken
 */
static void pljs_define_column(JSContext *ctx, JSValueConst obj,
                               Form_pg_attribute attr, JSValue value) {
  JSAtom atom;
  int ret;

  pljs_checked_value(ctx, value);
  atom = pljs_column_atom(ctx, attr);

  if (atom == JS_ATOM_NULL) {
    JS_FreeValue(ctx, value);
    pljs_ereport_js_exception(ctx);
  }

  ret = JS_DefinePropertyValue(ctx, obj, atom, value, JS_PROP_C_W_E);
  JS_FreeAtom(ctx, atom);
  pljs_checked_define(ctx, ret);
}

/**
 * @brief Reads a row object's property for a column.
 *
 * Keyed in UTF-8; see pljs_column_atom().
 *
 * @param ctx #JSContext - Javascript context
 * @param obj #JSValueConst - the row object
 * @param attr #Form_pg_attribute - the column
 * @returns #JSValue - an owned reference, or JS_EXCEPTION if a getter threw
 */
static JSValue pljs_get_column(JSContext *ctx, JSValueConst obj,
                               Form_pg_attribute attr) {
  JSAtom atom = pljs_column_atom(ctx, attr);
  JSValue value;

  if (atom == JS_ATOM_NULL) {
    return JS_EXCEPTION;
  }

  value = JS_GetProperty(ctx, obj, atom);
  JS_FreeAtom(ctx, atom);

  return value;
}

/**
 * @brief Whether a value is a plain JavaScript object -- `{...}` -- as opposed
 * to an Array, Date, ArrayBuffer, typed array or any other branded builtin.
 *
 * This is a real brand check (see the note on Is_Date() above): every builtin
 * has its own class id, so only a bare object literal / `new Object` matches.
 * Callers use it to tell a `{column: value}` row object apart from a value that
 * legitimately *is* object-like (a Date for a timestamp, a typed array for a
 * bytea, an Array for an array type).
 */
bool pljs_jsvalue_is_plain_object(JSValueConst obj) {
  return JS_GetClassID(obj) == JS_CLASS_OBJECT;
}

/**
 * @brief Whether a value is a Proxy, which a row can be; see
 * pljs_row_column().
 */
bool pljs_jsvalue_is_proxy(JSValueConst obj) {
  return JS_GetClassID(obj) == JS_CLASS_PROXY;
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
 * @brief What pljs needs to convert one type.
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
 * The state here is built once per type and reused for as long as the pljs
 * function being called keeps its FmgrInfo; see pljs_type_io_enter().
 */
typedef struct pljs_type_io {
  Oid typid;

  /* The function's cache context, which owns everything below. */
  MemoryContext mcxt;

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
   * The input function the type is converted with checks a domain inside it:
   * range_in() for a range over a domain runs the subtype's domain_in().  Or
   * can come to, having a composite type inside it, which a column of a
   * domain can be added to.  See pljs_domain_check().
   */
  bool inner_domain;

  /*
   * For an inner_domain, the types inside it, which are noted again whenever
   * its checking state is built; see pljs_domain_invalidate().
   */
  List *noted_types;

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

  /*
   * A domain's checking state: domain_check()'s, or for a domain_via_input
   * domain domain_in()'s, which keeps it in its FmgrInfo -- or for a type with
   * an inner_domain, its input function's.  Built on first use in a context
   * of its own, and again whenever the typcache has rebuilt the domain's
   * constraints, or for an inner_domain, whenever any domain's constraints
   * can have changed; see pljs_domain_check().
   */
  MemoryContext domain_mcxt;
  void *domain_extra;
  FmgrInfo domain_in;
  DomainConstraintCache *domain_constraints;
  uint64 domain_generation;

  /*
   * Whether checking runs a CHECK constraint, worked out with the state; see
   * pljs_type_domain_watch_start().
   */
  bool domain_may_check;

  /* A check further up the stack is using the domain's checking state. */
  bool domain_busy;
} pljs_type_io;

/*
 * A pljs function's type cache, kept in its FmgrInfo's fn_extra; see
 * pljs_type_io_enter().
 */
/* An entry of a pljs_type_io_cache's hash table. */
typedef struct pljs_type_io_slot {
  Oid typid; /* the key */
  char status;
  pljs_type_io *io;
} pljs_type_io_slot;

#define SH_PREFIX pljs_type_io_hash
#define SH_ELEMENT_TYPE pljs_type_io_slot
#define SH_KEY_TYPE Oid
#define SH_KEY typid
#define SH_HASH_KEY(tb, key) murmurhash32(key)
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

struct pljs_type_io_cache {
  MemoryContext mcxt;

  /*
   * The types converted so far, by OID.  Every value converted looks its type
   * up here, and a function that reads wide rows converts dozens of types: a
   * list was scanned from the front for every value.
   */
  pljs_type_io_hash_hash *types;

  /* See pljs_type_io_scratch_begin(). */
  MemoryContext scratch;
  bool scratch_busy;
};

/* The cache of the pljs function running now, or NULL outside of one. */
static pljs_type_io_cache *pljs_type_io_current = NULL;

/*
 * How many times any domain's constraints can have changed; see
 * pljs_type_io_init().
 */
static uint64 pljs_domain_generation = 0;

/*
 * The transaction nesting level a return_next() is watching for domain checks
 * that run CHECK constraints at, or 0, and how many have; see
 * pljs_type_domain_watch_start().
 */
static int pljs_domain_watch_level = 0;
static uint64 pljs_domain_watch_hits = 0;

/*
 * The types pljs_type_has_domain() has looked through, by the hash value of
 * their pg_type rows: those whose change can change what a conversion checks.
 */
typedef struct pljs_domain_type {
  uint32 hashvalue; /* the key */
  char status;
} pljs_domain_type;

#define SH_PREFIX pljs_domain_types
#define SH_ELEMENT_TYPE pljs_domain_type
#define SH_KEY_TYPE uint32
#define SH_KEY hashvalue
#define SH_HASH_KEY(tb, key) murmurhash32(key)
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static pljs_domain_types_hash *pljs_noted_types = NULL;

/*
 * A composite type pljs_type_has_domain() has looked through, by the OID of
 * its relation, with the types of its columns as it found them: a change to
 * a column changes the relation, and not the type's pg_type row.
 */
typedef struct pljs_domain_relation {
  Oid relid; /* the key */
  char status;
  Oid typid;        /* the composite type */
  int ncolumns;     /* how many columns it had */
  Oid *columns;     /* each one's type, InvalidOid if dropped */
  bool invalidated; /* since it was last compared; see below */
} pljs_domain_relation;

#define SH_PREFIX pljs_domain_relations
#define SH_ELEMENT_TYPE pljs_domain_relation
#define SH_KEY_TYPE Oid
#define SH_KEY relid
#define SH_HASH_KEY(tb, key) murmurhash32(key)
#define SH_EQUAL(tb, a, b) ((a) == (b))
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static pljs_domain_relations_hash *pljs_noted_relations = NULL;

/* Whether any of them has been invalidated since it was last compared. */
static bool pljs_relations_invalidated = false;

/**
 * @brief Counts an invalidation that can change a domain's constraints.
 *
 * Any change to pg_constraint, since its hash value names a constraint and
 * not the type it is on, and nothing here can read the catalogs to find out.
 * The typcache rebuilds every domain's constraints for one too, so a
 * temporary table with a constraint -- a primary key, or on PostgreSQL 18 a
 * NOT NULL column -- still costs a rebuild.  Of pg_type, only a change to a
 * type that a conversion has looked through, or to all of them at once.
 * Every change to pg_type counted, and every temporary table has a row type:
 * a function that made one for each row it returned rebuilt the checking
 * state of every range over a domain it converted, and worked out again for
 * each row whether converting it needed a subtransaction.
 */
static void pljs_domain_invalidate(Datum arg, int cacheid, uint32 hashvalue) {
  /*
   * A noted type is forgotten once it has changed, and what depended on it
   * is worked out again, which notes it again; see pljs_domain_check().  So a
   * type that is dropped -- a temporary table's row type -- is not kept for
   * the life of the backend.
   */
  if (cacheid == TYPEOID) {
    if (pljs_noted_types == NULL) {
      return;
    }

    if (hashvalue == 0) {
      pljs_domain_types_reset(pljs_noted_types);
    } else if (!pljs_domain_types_delete(pljs_noted_types, hashvalue)) {
      return;
    }
  }

  pljs_domain_generation++;
}

/**
 * @brief Notes that a change to a type's pg_type row can change what a
 * conversion checks; see pljs_domain_invalidate().
 *
 * @param typid #Oid - the type
 */
static void pljs_domain_type_note(Oid typid) {
  bool found;

  if (pljs_noted_types == NULL) {
    pljs_noted_types = pljs_domain_types_create(TopMemoryContext, 64, NULL);
  }

  pljs_domain_types_insert(
      pljs_noted_types, GetSysCacheHashValue1(TYPEOID, ObjectIdGetDatum(typid)),
      &found);
}

/**
 * @brief Notes an invalidation of a composite type's relation, which can
 * change what converting a value of the type checks.
 *
 * Adding a column of a domain to a row type, or changing a column's type to
 * one, left a set that had worked out it needed no subtransaction for its
 * rows converting them without one, while the new column's CHECK constraint
 * ran SQL; see pljs_return_next().
 *
 * But a relation is invalidated for much else -- VACUUM, ANALYZE, a new
 * index -- and counting every one had every function rebuild the checking
 * state it depended on, and every set work out its rows again, whenever a
 * table whose row type one had looked through was vacuumed.  Nothing here can
 * read the catalogs to see what changed, so the relation is only marked, and
 * its columns compared with what they were when next the count is asked for;
 * see pljs_domain_relations_compare().
 */
static void pljs_domain_relation_invalidate(Datum arg, Oid relid) {
  pljs_domain_relation *entry;

  if (pljs_noted_relations == NULL) {
    return;
  }

  if (!OidIsValid(relid)) {
    pljs_domain_relations_iterator iterator;

    pljs_domain_relations_start_iterate(pljs_noted_relations, &iterator);

    while ((entry = pljs_domain_relations_iterate(pljs_noted_relations,
                                                  &iterator)) != NULL) {
      entry->invalidated = true;
    }
  } else {
    entry = pljs_domain_relations_lookup(pljs_noted_relations, relid);

    if (entry == NULL) {
      return;
    }

    entry->invalidated = true;
  }

  pljs_relations_invalidated = true;
}

/**
 * @brief Returns the types of a row type's columns, InvalidOid for a dropped
 * one, as they are noted; see pljs_domain_relation.
 *
 * @param tupdesc #TupleDesc - the row type
 * @returns #Oid* - one for each of its columns, in TopMemoryContext, for
 * pljs_domain_relation_set() to keep
 */
static Oid *pljs_tupdesc_columns(TupleDesc tupdesc) {
  Oid *columns = MemoryContextAlloc(TopMemoryContext,
                                    sizeof(Oid) * Max(tupdesc->natts, 1));

  for (int i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

    columns[i] = attr->attisdropped ? InvalidOid : attr->atttypid;
  }

  return columns;
}

/**
 * @brief Reads the types of a relation's columns, as they are noted, from
 * the catalog caches.
 *
 * Which take no lock on the relation.  Reading its row type through the
 * typcache opens it, and it was read so for a conversion of any domain at
 * all, after the relation had been invalidated: a function that returned a
 * domain over int4 waited on another session's lock on a table it never
 * used, and failed once that session had dropped it.  All of its columns in
 * one lookup; one for each was a thousand for a table of a thousand.
 *
 * @param relid #Oid - the relation
 * @param ncolumns @c int* - set to how many columns it has
 * @returns #Oid* - one for each column, in TopMemoryContext, for
 * pljs_domain_relation_set() to keep; NULL if the relation is not there
 */
static Oid *pljs_relation_columns(Oid relid, int *ncolumns) {
  CatCList *list = SearchSysCacheList1(ATTNUM, ObjectIdGetDatum(relid));
  Oid *columns;
  int natts = 0;

  for (int i = 0; i < list->n_members; i++) {
    Form_pg_attribute attr =
        (Form_pg_attribute)GETSTRUCT(&list->members[i]->tuple);

    natts = Max(natts, attr->attnum);
  }

  /* No columns at all, which a relation can have, or no relation. */
  if (natts == 0 && !SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid))) {
    ReleaseSysCacheList(list);
    return NULL;
  }

  columns =
      MemoryContextAllocZero(TopMemoryContext, sizeof(Oid) * Max(natts, 1));

  for (int i = 0; i < list->n_members; i++) {
    Form_pg_attribute attr =
        (Form_pg_attribute)GETSTRUCT(&list->members[i]->tuple);

    /* The system columns come in the same list. */
    if (attr->attnum > 0) {
      columns[attr->attnum - 1] =
          attr->attisdropped ? InvalidOid : attr->atttypid;
    }
  }

  ReleaseSysCacheList(list);
  *ncolumns = natts;

  return columns;
}

/**
 * @brief Whether a composite type's columns are still the types noted.
 *
 * @param entry #pljs_domain_relation - the type, as noted
 * @param ncolumns @c int - how many columns it has now
 * @param columns #Oid* - their types now
 * @returns @c bool
 */
static bool pljs_domain_relation_matches(pljs_domain_relation *entry,
                                         int ncolumns, const Oid *columns) {
  return entry->columns != NULL && entry->ncolumns == ncolumns &&
         memcmp(entry->columns, columns, sizeof(Oid) * ncolumns) == 0;
}

/**
 * @brief Notes a composite type's columns' types, which it keeps.
 *
 * @param entry #pljs_domain_relation - the type
 * @param ncolumns @c int - how many columns it has
 * @param columns #Oid* - their types, in TopMemoryContext
 */
static void pljs_domain_relation_set(pljs_domain_relation *entry, int ncolumns,
                                     Oid *columns) {
  if (entry->columns != NULL) {
    pfree(entry->columns);
  }

  entry->columns = columns;
  entry->ncolumns = ncolumns;
}

/**
 * @brief Notes that a change to a composite type's columns can change what
 * a conversion checks; see pljs_domain_relation_invalidate().
 *
 * One noted before whose columns have changed since, which nothing has
 * compared yet, counts now: what was worked out from them before is stale.
 *
 * @param relid #Oid - the type's relation
 * @param typid #Oid - the type
 * @param tupdesc #TupleDesc - its columns, as they are now
 */
static void pljs_domain_relation_note(Oid relid, Oid typid, TupleDesc tupdesc) {
  pljs_domain_relation *entry;
  Oid *columns;
  bool found;

  if (pljs_noted_relations == NULL) {
    pljs_noted_relations =
        pljs_domain_relations_create(TopMemoryContext, 64, NULL);
  }

  entry = pljs_domain_relations_insert(pljs_noted_relations, relid, &found);

  /*
   * A new entry has only its key set, and is complete before anything that
   * can raise: running out of memory left the rest of it as a deleted entry
   * had left it.
   */
  if (!found) {
    entry->ncolumns = -1;
    entry->columns = NULL;
    entry->invalidated = false;
  }

  entry->typid = typid;

  /*
   * Once the entry is there to keep them: made first, running out of memory
   * in the hash table lost them for the life of the backend.
   */
  columns = pljs_tupdesc_columns(tupdesc);

  if (found && !pljs_domain_relation_matches(entry, tupdesc->natts, columns)) {
    pljs_domain_generation++;
  }

  pljs_domain_relation_set(entry, tupdesc->natts, columns);
}

/**
 * @brief Compares the columns of each composite type whose relation has been
 * invalidated with what they were; see pljs_domain_relation_invalidate().
 *
 * Any change counts, and a type that is no longer there -- a temporary
 * table's row type -- is forgotten, as a changed pg_type row is.  Read from
 * the catalog caches; see pljs_relation_columns().
 */
static void pljs_domain_relations_compare(void) {
  PG_TRY();
  {
    /*
     * Until none is left: reading the catalogs here can take invalidations,
     * of relations compared already too.
     */
    while (pljs_relations_invalidated) {
      pljs_domain_relations_iterator iterator;
      pljs_domain_relation *entry;
      List *gone = NIL;
      ListCell *lc;

      pljs_relations_invalidated = false;
      pljs_domain_relations_start_iterate(pljs_noted_relations, &iterator);

      while ((entry = pljs_domain_relations_iterate(pljs_noted_relations,
                                                    &iterator)) != NULL) {
        HeapTuple tuple;
        bool present;
        Oid *columns = NULL;
        int ncolumns = 0;

        if (!entry->invalidated) {
          continue;
        }

        /*
         * Before the columns are read.  Reading them can take invalidations
         * -- a cache miss that is the transaction's first lock on the
         * catalog -- and one of this relation's taken then was lost when this
         * was cleared after, with the columns read before it.
         */
        entry->invalidated = false;

        tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(entry->typid));
        present =
            HeapTupleIsValid(tuple) &&
            ((Form_pg_type)GETSTRUCT(tuple))->typtype == TYPTYPE_COMPOSITE &&
            ((Form_pg_type)GETSTRUCT(tuple))->typrelid == entry->relid;

        if (HeapTupleIsValid(tuple)) {
          ReleaseSysCache(tuple);
        }

        if (present) {
          columns = pljs_relation_columns(entry->relid, &ncolumns);
        }

        if (columns == NULL) {
          pljs_domain_generation++;
          gone = lappend_oid(gone, entry->relid);
          continue;
        }

        if (!pljs_domain_relation_matches(entry, ncolumns, columns)) {
          pljs_domain_generation++;
          pljs_domain_relation_set(entry, ncolumns, columns);
        } else {
          pfree(columns);
        }
      }

      foreach (lc, gone) {
        entry =
            pljs_domain_relations_lookup(pljs_noted_relations, lfirst_oid(lc));

        if (entry != NULL) {
          if (entry->columns != NULL) {
            pfree(entry->columns);
          }

          pljs_domain_relations_delete_item(pljs_noted_relations, entry);
        }
      }

      list_free(gone);
    }
  }
  PG_CATCH();
  {
    /*
     * Every one is compared again next time, as for an invalidation of every
     * relation: the one being compared had its mark cleared before its
     * columns were read, and those found gone were not deleted yet.  An error
     * -- a lock timeout, running out of memory -- lost the one's change for
     * good, and kept the others for the life of the backend.  Only an error
     * costs this.
     */
    pljs_domain_relation_invalidate((Datum)0, InvalidOid);
    PG_RE_THROW();
  }
  PG_END_TRY();
}

/**
 * @brief Sets up what the type conversions need for the life of the backend.
 *
 * The typcache rebuilds a domain's constraints when pg_constraint changes, or
 * pg_type does, and says so only to a caller that asks about that domain.
 * What converts a range over a domain holds that domain's checking state
 * deep inside range_in()'s, where nothing can ask; and whether a conversion
 * can run a CHECK constraint can change with any of the domains in a row.
 * So the same invalidations are counted here, and anything that depends on
 * them notes the count it was built at.
 */
void pljs_type_io_init(void) {
  CacheRegisterSyscacheCallback(CONSTROID, pljs_domain_invalidate, (Datum)0);
  CacheRegisterSyscacheCallback(TYPEOID, pljs_domain_invalidate, (Datum)0);
  CacheRegisterRelcacheCallback(pljs_domain_relation_invalidate, (Datum)0);
}

/**
 * @brief Returns the count kept by pljs_domain_invalidate(), once any
 * composite type's relation that has been invalidated has been compared; see
 * pljs_domain_relation_invalidate().
 *
 * @returns @c uint64 - it has changed whenever any domain's constraints can
 * have
 */
uint64 pljs_type_domain_generation(void) {
  if (pljs_relations_invalidated) {
    pljs_domain_relations_compare();
  }

  return pljs_domain_generation;
}

/**
 * @brief Starts watching for domain checks that run CHECK constraints, which
 * can run any SQL, at the transaction nesting level current now.
 *
 * For return_next(), which converts a row without a subtransaction when no
 * CHECK constraint can run, and has to know when one ran all the same --
 * added by a getter while the row was converted -- and so left what it held
 * when it raised.  pljs_domain_check() counts each one that begins at the
 * level watched, loading its constraints included, which runs their IMMUTABLE
 * functions.  One deeper -- in a getter's pljs.execute(), or a function it
 * calls -- has a subtransaction of its own to roll it back, and does not
 * count.  It was worked out from invalidations first, which a nested
 * return_next() hid and an ANALYZE anywhere made, and then from a count of
 * every check anywhere, which a getter's own pljs.execute() made.
 *
 * @param saved #pljs_domain_watch - set to the watch this one replaces, for
 * pljs_type_domain_watch_end()
 */
void pljs_type_domain_watch_start(pljs_domain_watch *saved) {
  saved->level = pljs_domain_watch_level;
  saved->hits = pljs_domain_watch_hits;

  pljs_domain_watch_level = GetCurrentTransactionNestLevel();
  pljs_domain_watch_hits = 0;
}

/**
 * @brief Stops watching, and puts back the watch this one replaced; see
 * pljs_type_domain_watch_start().
 *
 * A nested watch at the same level passes what it saw to the one it
 * replaced, since what it saw ran without a subtransaction for that one too.
 *
 * @param saved #pljs_domain_watch - what pljs_type_domain_watch_start() set
 * @returns @c bool - whether a check that runs a CHECK constraint began
 */
bool pljs_type_domain_watch_end(pljs_domain_watch *saved) {
  uint64 hits = pljs_domain_watch_hits;
  bool same_level = saved->level == pljs_domain_watch_level;

  pljs_domain_watch_level = saved->level;
  pljs_domain_watch_hits = saved->hits + (same_level ? hits : 0);

  return hits != 0;
}

/**
 * @brief Whether a domain has a CHECK constraint, of its own or of a domain
 * it is over.
 *
 * NOT NULL runs no code, and a CHECK constraint can call any function.
 *
 * @param typid #Oid - the domain
 * @returns @c bool
 */
static bool pljs_domain_has_check(Oid typid) {
  MemoryContext mcxt = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS Domain Constraints", ALLOCSET_SMALL_SIZES);
  DomainConstraintRef *ref = MemoryContextAlloc(mcxt, sizeof(*ref));
  ListCell *lc;
  bool found = false;

  InitDomainConstraintRef(typid, ref, mcxt, false);

  foreach (lc, ref->constraints) {
    DomainConstraintState *constraint = (DomainConstraintState *)lfirst(lc);

    if (constraint->constrainttype == DOM_CONSTRAINT_CHECK) {
      found = true;
      break;
    }
  }

  /* Also gives back the reference to the constraints that ref took. */
  MemoryContextDelete(mcxt);

  return found;
}

/**
 * @brief Returns the type a domain is directly over, which can be a domain.
 *
 * getBaseType() goes to the end of a chain of domains, past the ones between.
 *
 * @param typid #Oid - the domain
 * @returns #Oid of its typbasetype
 */
static Oid pljs_domain_direct_base(Oid typid) {
  HeapTuple tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
  Oid base;

  if (!HeapTupleIsValid(tuple)) {
    elog(ERROR, "cache lookup failed for type %u", typid);
  }

  base = ((Form_pg_type)GETSTRUCT(tuple))->typbasetype;
  ReleaseSysCache(tuple);

  return base;
}

/**
 * @brief Whether converting a value to a type can check a domain.
 *
 * That is a domain, or an array, composite, range or multirange type with one
 * inside it.  A range's input function runs its subtype's, which for a domain
 * is domain_in().
 *
 * Asked about any domain, it looks through every type inside, even once the
 * answer is known, and notes each one, in @p noted as well: a change to any
 * of them can change what converting the type checks; see
 * pljs_domain_invalidate().  It stopped at the first domain, so a second one
 * in the same row, and each domain a chain of them passes through, went
 * unnoted, and ALTER DOMAIN on it unseen.  Asked only about a CHECK
 * constraint, it stops at the first, and notes no type: a constraint changes
 * pg_constraint, every change to which counts.  Either way it notes the
 * relation of each composite type it looks through, since a column added to
 * one, or changed, changes neither.
 *
 * @param typid #Oid - the type
 * @param checks_only @c bool - count only a domain with a CHECK constraint
 * @param noted @c List** - to append the types looked through to, or NULL;
 * only for !checks_only
 * @returns @c bool
 */
static bool pljs_type_has_domain(Oid typid, bool checks_only, List **noted) {
  bool found = false;
  Oid inner;

  if (!checks_only) {
    /* Before the catalogs are read, so that a change while they are is seen. */
    pljs_domain_type_note(typid);

    if (noted != NULL) {
      *noted = lappend_oid(*noted, typid);
    }
  }

  switch (get_typtype(typid)) {
  case TYPTYPE_DOMAIN:
    /* A domain's constraints include those of the domains it is over. */
    if (checks_only) {
      return pljs_domain_has_check(typid) ||
             pljs_type_has_domain(getBaseType(typid), true, NULL);
    }

    /* Each domain of a chain in turn, so that each is noted. */
    pljs_type_has_domain(pljs_domain_direct_base(typid), false, noted);

    return true;

  case TYPTYPE_RANGE:
    return pljs_type_has_domain(get_range_subtype(typid), checks_only, noted);

  case TYPTYPE_MULTIRANGE:
    return pljs_type_has_domain(get_multirange_range(typid), checks_only,
                                noted);

  case TYPTYPE_COMPOSITE: {
    Oid relid = get_typ_typrelid(typid);

    /* A copy, so that nothing is left pinned if a lookup below raises. */
    TupleDesc tupdesc = lookup_rowtype_tupdesc_copy(typid, -1);

    /* With the columns it has now, before theirs are read. */
    pljs_domain_relation_note(relid, typid, tupdesc);

    for (int i = 0; i < tupdesc->natts && !(found && checks_only); i++) {
      Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

      if (!attr->attisdropped &&
          pljs_type_has_domain(attr->atttypid, checks_only, noted)) {
        found = true;
      }
    }

    FreeTupleDesc(tupdesc);

    return found;
  }

  default:
    inner = get_element_type(typid);

    return OidIsValid(inner) && pljs_type_has_domain(inner, checks_only, noted);
  }
}

/**
 * @brief Whether a type has a composite type inside it: is one, or a domain,
 * range, multirange or array over one.
 *
 * @param typid #Oid - the type
 * @returns @c bool
 */
static bool pljs_type_has_composite(Oid typid) {
  Oid inner;

  switch (get_typtype(typid)) {
  case TYPTYPE_COMPOSITE:
    return true;

  case TYPTYPE_DOMAIN:
    return pljs_type_has_composite(getBaseType(typid));

  case TYPTYPE_RANGE:
    return pljs_type_has_composite(get_range_subtype(typid));

  case TYPTYPE_MULTIRANGE:
    return pljs_type_has_composite(get_multirange_range(typid));

  default:
    inner = get_element_type(typid);

    return OidIsValid(inner) && pljs_type_has_composite(inner);
  }
}

/**
 * @brief Whether pljs has a dedicated conversion for a (non-domain) type.
 *
 * Everything else is converted by the fallback, through the type's own text
 * I/O functions.  This list decides it, for pljs_datum_to_jsvalue() and
 * pljs_jsvalue_to_datum_internal() as much as for a domain's route and a
 * typmod: neither goes into its switch of scalar cases for a type not listed
 * here, and each raises for a type listed here that it has no case for.  It
 * was a copy of those switches' case labels, which only a comment kept in
 * step: a case added to them alone would have been taken for a fallback type
 * everywhere else, and a domain over it parsed by domain_in() from a value
 * built for the case, with no warning at all.
 */
static bool pljs_type_has_js_case(Oid typid) {
  switch (typid) {
  case VOIDOID:
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
 * @brief Makes a pljs function's type cache the one conversions use.
 *
 * A pljs function keeps its types' conversion state in its FmgrInfo's
 * fn_extra, allocated in fn_mcxt, which is where PostgreSQL's own I/O
 * functions keep theirs and where plv8 keeps its type descriptors.  It is
 * built as each type is first converted, and lasts as long as the FmgrInfo:
 * the query calling the function, or for a CALL or a DO block the whole call,
 * COMMITs included.
 *
 * Nothing frees any of it before then.  A conversion can run JavaScript -- a
 * getter, or toString() -- which can COMMIT, or call a function that converts
 * the same type, and the conversion further up the stack still holds its entry
 * when that returns.  A cache kept for the life of the backend has to notice
 * changes and free what they made stale, and every place it did so could free
 * an entry that was still in use.  Here nothing goes stale: an entry holds
 * only what cannot change for an existing type, read through the typcache,
 * and a domain's constraints are checked against the typcache on every use.
 * A nested pljs call converts with its own function's cache.
 *
 * Every entry point that can run JavaScript, and so convert, calls this, and
 * restores the cache it returns with pljs_type_io_exit() however it leaves.
 *
 * @param flinfo #FmgrInfo - the pljs function being called
 * @returns #pljs_type_io_cache - the cache to restore afterwards
 */
pljs_type_io_cache *pljs_type_io_enter(FmgrInfo *flinfo) {
  pljs_type_io_cache *previous = pljs_type_io_current;
  pljs_type_io_cache *cache = (pljs_type_io_cache *)flinfo->fn_extra;

  if (cache == NULL) {
    MemoryContext mcxt = AllocSetContextCreate(flinfo->fn_mcxt, "PLJS Type I/O",
                                               ALLOCSET_SMALL_SIZES);

    cache = MemoryContextAllocZero(mcxt, sizeof(pljs_type_io_cache));
    cache->mcxt = mcxt;
    cache->types = pljs_type_io_hash_create(mcxt, 16, NULL);

    flinfo->fn_extra = cache;
  }

  pljs_type_io_current = cache;

  return previous;
}

/**
 * @brief Restores the type cache that pljs_type_io_enter() replaced.
 *
 * @param previous #pljs_type_io_cache - what pljs_type_io_enter() returned
 */
void pljs_type_io_exit(pljs_type_io_cache *previous) {
  pljs_type_io_current = previous;
}

/**
 * @brief Returns a context for what one call of a type's I/O function
 * allocates.
 *
 * An I/O function frees at most the value it returns, and not what it
 * allocated on the way there, so the text I/O fallback runs it in a context
 * that is emptied once the value has been copied out; see
 * pljs_datum_to_jsvalue_fallback().  The running function's cache keeps one
 * for the purpose.  An input function can run a domain's CHECK constraints,
 * which can call the same pljs function again, so a call made while that one
 * is in use gets a context of its own.
 *
 * @returns #MemoryContext - to hand to pljs_type_io_scratch_end()
 */
static MemoryContext pljs_type_io_scratch_begin(void) {
  pljs_type_io_cache *cache = pljs_type_io_current;

  if (cache->scratch_busy) {
    return AllocSetContextCreate(CurrentMemoryContext, "PLJS Type I/O",
                                 ALLOCSET_DEFAULT_SIZES);
  }

  if (cache->scratch == NULL) {
    cache->scratch = AllocSetContextCreate(cache->mcxt, "PLJS Type I/O",
                                           ALLOCSET_DEFAULT_SIZES);
  }

  cache->scratch_busy = true;

  return cache->scratch;
}

/**
 * @brief Empties a context from pljs_type_io_scratch_begin().
 *
 * @param scratch #MemoryContext - what pljs_type_io_scratch_begin() returned
 */
static void pljs_type_io_scratch_end(MemoryContext scratch) {
  pljs_type_io_cache *cache = pljs_type_io_current;

  if (scratch == cache->scratch) {
    MemoryContextReset(scratch);
    cache->scratch_busy = false;
  } else {
    MemoryContextDelete(scratch);
  }
}

/**
 * @brief Returns the conversion state for a type, building it if needed.
 *
 * @param typid #Oid - the type
 * @returns #pljs_type_io - owned by the running function's cache, and valid
 * for as long as that is
 */
static pljs_type_io *pljs_type_io_lookup(Oid typid) {
  pljs_type_io_cache *cache = pljs_type_io_current;
  pljs_type_io_slot *slot;
  pljs_type_io built;
  pljs_type_io *entry = &built;
  List *noted = NIL;
  TypeCacheEntry *typentry;
  char base_category;
  bool is_preferred;
  bool found;

  if (cache == NULL) {
    elog(ERROR, "pljs type conversion outside of a function call");
  }

  slot = pljs_type_io_hash_lookup(cache->types, typid);

  if (slot != NULL) {
    return slot->io;
  }

  /*
   * Built here, and copied into the cache only once it is complete: the
   * lookups below read the catalogs, and can raise -- for a type that does
   * not exist, say -- and an entry allocated first stayed in the cache's
   * context, which can last a whole procedure, for every one that did.
   */
  MemSet(entry, 0, sizeof(pljs_type_io));
  entry->typid = typid;
  entry->mcxt = cache->mcxt;

  typentry = lookup_type_cache(typid, TYPECACHE_DOMAIN_BASE_INFO);

  entry->is_domain = (typentry->typtype == TYPTYPE_DOMAIN);
  entry->basetype = entry->is_domain ? typentry->domainBaseType : typid;
  entry->basetypmod = entry->is_domain ? typentry->domainBaseTypmod : -1;
  entry->length = typentry->typlen;
  entry->byval = typentry->typbyval;
  entry->align = typentry->typalign;

  entry->has_js_case = pljs_type_has_js_case(entry->basetype);

  get_type_category_preferred(typid, &entry->category, &is_preferred);

  /*
   * record[] is a pseudo-type, but an array for all that, of anonymous rows:
   * ARRAY[ROW(1, 'a')].  Taken for a row, its array was read as a tuple
   * header, which named no type -- "type with OID 0 does not exist" -- and a
   * second one crashed the backend.
   */
  if (entry->category == TYPCATEGORY_PSEUDOTYPE &&
      OidIsValid(get_element_type(typid))) {
    entry->category = TYPCATEGORY_ARRAY;
  }

  if (entry->category == TYPCATEGORY_ARRAY) {
    /*
     * A domain over an array has the array's category but no element type of
     * its own, so look through it to the array it is a domain over.
     */
    entry->elemtype = get_element_type(entry->basetype);

    if (OidIsValid(entry->elemtype)) {
      TypeCacheEntry *elementry = lookup_type_cache(entry->elemtype, 0);

      entry->elem_is_composite =
          (TypeCategory(entry->elemtype) == TYPCATEGORY_COMPOSITE ||
           entry->elemtype == RECORDOID);
      entry->elem_length = elementry->typlen;
      entry->elem_byval = elementry->typbyval;
      entry->elem_align = elementry->typalign;
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

  /*
   * Only a type converted through an input function -- the fallback's, or a
   * domain_via_input domain's domain_in() -- can reach a domain inside it
   * that way.  An array or a composite is converted element by element, or
   * column by column, and a type with a case of its own by that case.
   */
  /*
   * Or a composite type inside it -- a range over one -- which a column of a
   * domain can be added to while a set is being returned.  Its input function
   * checks that column's domain without pljs_domain_check(), so a CHECK
   * constraint ran there that nothing knew of: return_next() let JavaScript
   * catch its error, with what it had held left.  Such a type is checked as
   * one with a domain inside it, whose checking state is worked out again
   * once the composite type changes; see pljs_domain_relation_invalidate().
   */
  if (entry->is_domain) {
    entry->inner_domain =
        entry->domain_via_input &&
        (pljs_type_has_domain(entry->basetype, false, &noted) ||
         pljs_type_has_composite(entry->basetype));
  } else {
    entry->inner_domain = !entry->has_js_case &&
                          entry->category != TYPCATEGORY_ARRAY &&
                          entry->category != TYPCATEGORY_COMPOSITE &&
                          (pljs_type_has_domain(typid, false, &noted) ||
                           pljs_type_has_composite(typid));
  }

  if (entry->inner_domain) {
    MemoryContext old_context = MemoryContextSwitchTo(cache->mcxt);

    entry->noted_types = list_copy(noted);
    MemoryContextSwitchTo(old_context);
  }

  list_free(noted);

  /* Entered once built, so a build that raised leaves nothing half done. */
  entry = MemoryContextAlloc(cache->mcxt, sizeof(pljs_type_io));
  *entry = built;

  slot = pljs_type_io_hash_insert(cache->types, typid, &found);
  slot->io = entry;

  return entry;
}

/**
 * @brief Returns the input function for a cached type, looking it up once.
 */
static FmgrInfo *pljs_type_io_input(pljs_type_io *io) {
  if (!io->have_input) {
    Oid typinput;

    getTypeInputInfo(io->typid, &typinput, &io->ioparam);
    fmgr_info_cxt(typinput, &io->input, io->mcxt);
    io->have_input = true;
  }

  return &io->input;
}

/**
 * @brief Returns the type whose output function converts a type to text.
 *
 * regproc and regoper are written as regprocedure and regoperator write them,
 * as a signature -- `abs(integer)` -- rather than as the bare name their own
 * output functions write.  Those are the same OIDs, and a bare name is not
 * enough to read one back: regprocin() rejects the name of an overloaded
 * function, so a trigger that returned NEW failed for every row of a table
 * with a regproc column naming one.  pljs_signature_to_oid_text() reads the
 * signature back.
 *
 * @param typid #Oid - the (non-domain) type
 * @returns #Oid of the type to use the output function of
 */
static Oid pljs_output_type(Oid typid) {
  switch (typid) {
  case REGPROCOID:
    return REGPROCEDUREOID;
  case REGOPEROID:
    return REGOPERATOROID;
  default:
    return typid;
  }
}

/**
 * @brief Reads back a regproc or regoper written as a signature.
 *
 * See pljs_output_type().  A signature is resolved to its OID, which the
 * type's own input function, and a domain's, take as digits.  Anything else
 * is left for them as it is.
 *
 * @param io #pljs_type_io - the target type
 * @param str @c char* - the text to read
 * @returns @c char* - the text to give the input function
 */
static char *pljs_signature_to_oid_text(pljs_type_io *io, char *str) {
  Datum oid;

  if ((io->basetype != REGPROCOID && io->basetype != REGOPEROID) ||
      strchr(str, '(') == NULL) {
    return str;
  }

  if (io->basetype == REGPROCOID) {
    oid = DirectFunctionCall1(regprocedurein, CStringGetDatum(str));
  } else {
    oid = DirectFunctionCall1(regoperatorin, CStringGetDatum(str));
  }

  return psprintf("%u", DatumGetObjectId(oid));
}

/*
 * The functions that convert between UTF-8 and the database's encoding, looked
 * up once; see pljs_encoding_init().
 */
static bool pljs_encoding_ready = false;
static bool pljs_have_to_server = false;
static bool pljs_have_to_utf8 = false;
static FmgrInfo pljs_to_server_proc;
static FmgrInfo pljs_to_utf8_proc;

/**
 * @brief Whether text is all ASCII, which every server encoding holds as it is.
 */
static bool pljs_is_ascii(const char *str, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if ((unsigned char)str[i] >= 0x80) {
      return false;
    }
  }

  return true;
}

/**
 * @brief Converts text with a function pljs_encoding_init() looked up,
 * raising for any character it cannot convert.
 *
 * Allocated and trimmed as pg_do_encoding_conversion() does it.  Enough for
 * any conversion is four times the text, which a plain palloc() refuses past
 * a gigabyte: a text of 270MB in a LATIN1 database raised "invalid memory
 * alloc request size", where pg_server_to_any() converted it.  And a large
 * one is trimmed to what it holds, rather than keeping four times the text
 * for as long as it lives.
 *
 * @param proc #FmgrInfo - the conversion function
 * @param from @c int - the text's encoding
 * @param to @c int - the encoding to convert it to
 * @param str @c char* - the text
 * @param len @c size_t - its length
 * @returns @c char* - a palloc'd conversion of @p str
 */
static char *pljs_convert_strict(FmgrInfo *proc, int from, int to,
                                 const char *str, size_t len) {
  char *out;

  if (len > (size_t)PG_INT32_MAX ||
      len >= MaxAllocHugeSize / MAX_CONVERSION_GROWTH) {
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("out of memory"),
             errdetail("String of %zu bytes is too long for encoding "
                       "conversion.",
                       len)));
  }

  out = MemoryContextAllocHuge(CurrentMemoryContext,
                               len * MAX_CONVERSION_GROWTH + 1);

  FunctionCall6(proc, Int32GetDatum(from), Int32GetDatum(to),
                CStringGetDatum(str), CStringGetDatum(out),
                Int32GetDatum((int)len), BoolGetDatum(false));

  if (len > 1000000) {
    size_t out_len = strlen(out);

    if (out_len >= MaxAllocSize) {
      ereport(ERROR,
              (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("out of memory"),
               errdetail("String of %zu bytes is too long for encoding "
                         "conversion.",
                         len)));
    }

    out = repalloc(out, out_len + 1);
  }

  return out;
}

/**
 * @brief Converts text in the database's encoding to UTF-8, for QuickJS.
 *
 * QuickJS reads every C string as UTF-8, so the text a type's output function
 * writes has to be converted when the database has another encoding: in a
 * LATIN1 database, an enum label with an accented letter became U+FFFD, and
 * its input function then rejected it.  SQL_ASCII has no encoding to convert
 * from, so its bytes are passed as they are.
 *
 * @param str @c char* - the text
 * @param len @c size_t - its length
 * @returns @c char* - @p str, or a palloc'd conversion of it
 */
char *pljs_server_to_utf8(const char *str, size_t len) {
  int encoding = GetDatabaseEncoding();

  if (encoding == PG_UTF8 || encoding == PG_SQL_ASCII ||
      pljs_is_ascii(str, len)) {
    return (char *)str;
  }

  /*
   * With the function looked up once; see pljs_encoding_init().
   * pg_server_to_any() looked it up for every column's name and every text
   * value when the client's encoding was not UTF-8.
   */
  if (!pljs_encoding_ready || !pljs_have_to_utf8) {
    return pg_server_to_any(str, len, PG_UTF8);
  }

  return pljs_convert_strict(&pljs_to_utf8_proc, encoding, PG_UTF8, str, len);
}

/**
 * @brief Makes a JavaScript string of text in the database's encoding.
 *
 * For every value that reaches JavaScript as text -- text, varchar, char,
 * name, json, and a jsonb string or key -- as well as the fallback's.  Those
 * were given to QuickJS as they were stored, so in a LATIN1 database every
 * accented letter became U+FFFD; see pljs_server_to_utf8().
 *
 * @param ctx #JSContext - Javascript context
 * @param str @c char* - the text
 * @param len @c size_t - its length
 * @returns #JSValue of the string
 */
JSValue pljs_new_server_string(JSContext *ctx, const char *str, size_t len) {
  char *utf8 = pljs_server_to_utf8(str, len);
  JSValue ret;

  if (utf8 == str) {
    return JS_NewStringLen(ctx, str, len);
  }

  ret = JS_NewString(ctx, utf8);
  pfree(utf8);

  return ret;
}

/**
 * @brief Converts UTF-8 text from QuickJS to the database's encoding.
 *
 * The reverse of pljs_server_to_utf8(), except that the text is validated
 * even when there is nothing to convert.  QuickJS writes a lone surrogate --
 * '\uD800' -- as the three bytes ED A0 80, which are not UTF-8, and in a UTF8
 * database they were passed on unchecked: citext stored them, and a dump of
 * the table could not be restored.  pg_any_to_server() validates it, and the
 * conversion functions do, as they convert.
 *
 * With the function pljs_server_to_utf8() converts the other way with, from
 * pg_catalog: pg_any_to_server() looked one up through the caller's
 * search_path, so a default conversion of a schema on it converted values
 * written to PostgreSQL, and pg_catalog's the values read, and a value did
 * not come back as it went.
 *
 * @param str @c char* - the text, which QuickJS owns
 * @param len @c size_t - its length
 * @returns @c char* - @p str, or a palloc'd conversion of it
 */
char *pljs_utf8_to_server(const char *str, size_t len) {
  int encoding = GetDatabaseEncoding();

  if (encoding == PG_UTF8 || encoding == PG_SQL_ASCII || !pljs_encoding_ready ||
      !pljs_have_to_server) {
    return pg_any_to_server(str, len, PG_UTF8);
  }

  /*
   * ASCII, which every server encoding holds as it is, unless it has a NUL,
   * which none takes.
   */
  if (pljs_is_ascii(str, len) && memchr(str, '\0', len) == NULL) {
    return (char *)str;
  }

  return pljs_convert_strict(&pljs_to_server_proc, PG_UTF8, encoding, str, len);
}

/**
 * @brief Looks up the functions a message is converted with.
 *
 * Once, and where an error is harmless: at the start of a call, a DO block or
 * a validation.  A message is converted while an error is being reported, or
 * in a builtin QuickJS called, where the lookup's catalog access can raise --
 * even a cancel, while it waits for a lock -- and an error there replaced the
 * one being reported, or unwound past QuickJS's live frames.  The database's
 * encoding cannot change, so neither can they.
 *
 * The built-in ones, in pg_catalog.  They were looked up through the
 * search_path of the first call, so a default conversion in a schema of the
 * first caller's was kept for every later call in the backend, another
 * user's, and a SECURITY DEFINER function's, included.
 */
void pljs_encoding_init(void) {
  int encoding = GetDatabaseEncoding();

  if (pljs_encoding_ready) {
    return;
  }

  if (encoding != PG_UTF8 && encoding != PG_SQL_ASCII) {
    Oid to_server =
        FindDefaultConversion(PG_CATALOG_NAMESPACE, PG_UTF8, encoding);
    Oid to_utf8 =
        FindDefaultConversion(PG_CATALOG_NAMESPACE, encoding, PG_UTF8);

    if (OidIsValid(to_server)) {
      fmgr_info_cxt(to_server, &pljs_to_server_proc, TopMemoryContext);
      pljs_have_to_server = true;
    }

    if (OidIsValid(to_utf8)) {
      fmgr_info_cxt(to_utf8, &pljs_to_utf8_proc, TopMemoryContext);
      pljs_have_to_utf8 = true;
    }
  }

  pljs_encoding_ready = true;
}

/**
 * @brief Converts a message between encodings, never raising.
 *
 * For an error's message and detail, a log message, and the names in a
 * message about a row, both ways.  A strict conversion raises for a lone
 * surrogate, or a character the other encoding has no equivalent for, and
 * when that happened while an error was being reported, its error replaced
 * the one being reported: a function's division_by_zero became an encoding
 * error, and a NOTICE failed the function.  Nor can a builtin QuickJS called
 * raise, since an error there unwinds past QuickJS's live frames.
 *
 * So a character that cannot be converted is written as '?', and only that
 * character.  The conversion function, looked up beforehand, is asked not to
 * raise -- conversions can stop at the first character they cannot convert --
 * and nothing is caught.  Catching the error and flushing it, as this did,
 * threw away whatever error was being built around it: a message converted
 * among ereport()'s arguments came back as "errstart was not called".
 *
 * @param str @c char* - the text
 * @param len @c size_t - its length
 * @param from @c int - its encoding
 * @param to @c int - the encoding to convert it to
 * @returns @c char* - @p str, or a palloc'd conversion of it
 */
static char *pljs_convert_message(const char *str, size_t len, int from,
                                  int to) {
  FmgrInfo *proc = NULL;
  char *out;
  size_t pos = 0;
  size_t i = 0;

  /* The usual cases, which have nothing to convert. */
  if (from == PG_SQL_ASCII || to == PG_SQL_ASCII || pljs_is_ascii(str, len)) {
    return (char *)str;
  }

  /*
   * One too long for the conversion functions, which count in an int, is
   * left as it is: a message of hundreds of megabytes, which there is no
   * reporting sensibly either way.
   */
  if (len > (size_t)(PG_INT32_MAX - 1) / MAX_CONVERSION_GROWTH) {
    return (char *)str;
  }

  if (from == to && pg_verify_mbstr(from, str, (int)len, true)) {
    return (char *)str;
  }

  if (from != to && pljs_encoding_ready) {
    if (to == PG_UTF8 && pljs_have_to_utf8) {
      proc = &pljs_to_utf8_proc;
    } else if (from == PG_UTF8 && pljs_have_to_server) {
      proc = &pljs_to_server_proc;
    }
  }

  /*
   * Enough for any conversion, and allocated without raising, which plain
   * palloc() does past a gigabyte: a message of 256MB raised "invalid memory
   * alloc request size" in place of the error being reported.  Without the
   * memory, the message is left as it is.
   */
  out = palloc_extended(len * MAX_CONVERSION_GROWTH + 1,
                        MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);

  if (out == NULL) {
    return (char *)str;
  }

  while (i < len) {
    int width;

    /* As much as converts, up to the first character that does not. */
    if (proc != NULL) {
      int done = DatumGetInt32(
          FunctionCall6(proc, Int32GetDatum(from), Int32GetDatum(to),
                        CStringGetDatum(str + i), CStringGetDatum(out + pos),
                        Int32GetDatum((int)(len - i)), BoolGetDatum(true)));

      pos += strlen(out + pos);
      i += done;

      if (i >= len) {
        break;
      }
    } else if ((unsigned char)str[i] < 0x80) {
      out[pos++] = str[i++];
      continue;
    }

    width = pg_encoding_verifymbchar(from, str + i, (int)(len - i));

    if (width > 0 && from == to) {
      memcpy(out + pos, str + i, width);
      pos += width;
      i += width;
      continue;
    }

    /* A character that cannot be converted, or not a character at all. */
    out[pos++] = '?';

    if (width > 0) {
      i += width;
    } else {
      /*
       * A lone surrogate, say, which QuickJS writes as three bytes: one '?'
       * for the bytes it claims, but never an ASCII one.
       */
      int claimed = pg_encoding_mblen(from, str + i);

      i++;

      while (--claimed > 0 && i < len && (unsigned char)str[i] >= 0x80) {
        i++;
      }
    }
  }

  out[pos] = '\0';

  return out;
}

/**
 * @brief Converts text from QuickJS to the database's encoding for a message,
 * never raising; see pljs_convert_message().
 *
 * @param str @c char* - the UTF-8 text
 * @param len @c size_t - its length
 * @returns @c char* - @p str, or a palloc'd conversion of it
 */
char *pljs_utf8_to_server_lossy(const char *str, size_t len) {
  return pljs_convert_message(str, len, PG_UTF8, GetDatabaseEncoding());
}

/**
 * @brief Makes a JavaScript string of a message in the database's encoding,
 * without raising; see pljs_convert_message().
 *
 * For an error's message, detail and hint, which a builtin hands JavaScript
 * from a function QuickJS called.
 *
 * @param ctx #JSContext - Javascript context
 * @param str @c char* - the text
 * @param len @c size_t - its length
 * @returns #JSValue of the string
 */
JSValue pljs_new_message_string(JSContext *ctx, const char *str, size_t len) {
  char *utf8 = pljs_convert_message(str, len, GetDatabaseEncoding(), PG_UTF8);
  JSValue ret;

  if (utf8 == str) {
    return JS_NewStringLen(ctx, str, len);
  }

  ret = JS_NewString(ctx, utf8);
  pfree(utf8);

  return ret;
}

/**
 * @brief pljs_utf8_to_server() for a string QuickJS owns, which is released
 * if the text is not valid.
 *
 * For every value that reaches PostgreSQL as text -- text, varchar, char,
 * name, json, and a jsonb string or key -- as well as the fallback's.  Those
 * were stored as QuickJS wrote them: a lone surrogate as bytes that are not
 * UTF-8, and in a LATIN1 database every accented letter as the two bytes of
 * its UTF-8.
 *
 * @param ctx #JSContext - Javascript context that owns @p str
 * @param str @c char* - the string, which the caller frees unless this raises
 * @param len @c size_t - its length
 * @returns @c char* - @p str, or a palloc'd conversion of it, which the
 * caller frees
 */
static char *pljs_js_string_to_server(JSContext *ctx, const char *str,
                                      size_t len) {
  char *server;

  PG_TRY();
  {
    server = pljs_utf8_to_server(str, len);
  }
  PG_CATCH();
  {
    JS_FreeCString(ctx, str);
    PG_RE_THROW();
  }
  PG_END_TRY();

  return server;
}

/**
 * @brief Returns the output function for a cached type, looking it up once.
 */
static FmgrInfo *pljs_type_io_output(pljs_type_io *io) {
  if (!io->have_output) {
    Oid typoutput;
    bool typisvarlena;

    getTypeOutputInfo(pljs_output_type(io->typid), &typoutput, &typisvarlena);
    fmgr_info_cxt(typoutput, &io->output, io->mcxt);
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
 * @brief Whether converting a value to a type can run a domain's CHECK
 * constraint.
 *
 * A CHECK constraint can call any function, so converting a value to a
 * domain with one can run any SQL, which pljs_return_next() has to know.
 * That is such a domain, or an array, composite, range or multirange type
 * with one inside it; see pljs_type_has_domain().  Ranges were missed once,
 * and a failed CHECK in a range of a domain left its SPI connection on the
 * stack as any other did.  A domain with no CHECK constraint -- none at all,
 * or NOT NULL alone -- runs nothing, and does not count.
 *
 * The answer changes when a constraint is added or dropped, so it is only
 * good for as long as pljs_type_domain_generation() is unchanged.
 *
 * @param typid #Oid - the type
 * @returns @c bool
 */
bool pljs_type_may_check_domain(Oid typid) {
  return pljs_type_has_domain(typid, true, NULL);
}

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
      fmgr_info_cxt(funcid, &io->coercion, io->mcxt);
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
 * @brief Takes back a domain check counted for a watching return_next() once
 * it is known to run no CHECK constraint; see pljs_domain_check().
 *
 * As soon as that is known, and not after the rest of the check's setup: an
 * error there -- a range whose type a getter dropped -- was taken for a
 * constraint's, and ended the call.  Loading a domain's constraints stays
 * counted, errors that are not a constraint's included: one for a domain a
 * getter dropped ends the call still; see docs/FUNCTIONS.md.
 *
 * @param watched @c bool* - whether the check is counted, cleared once taken
 * back
 * @param may_check @c bool - whether it runs a CHECK constraint
 */
static void pljs_domain_check_unwatch(bool *watched, bool may_check) {
  if (*watched && !may_check) {
    pljs_domain_watch_hits--;
    *watched = false;
  }
}

/**
 * @brief Lets go of what pljs_domain_check() set up for one check.
 *
 * @param io #pljs_type_io - the domain
 * @param nested #MemoryContext - a nested check's own state, or NULL
 */
static void pljs_domain_check_done(pljs_type_io *io, MemoryContext nested) {
  if (nested != NULL) {
    MemoryContextDelete(nested);
  } else {
    io->domain_busy = false;
  }
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
 * ExprContext they run in -- in the entry's domain_mcxt, and two things about
 * that state need care:
 *
 *   - A check further up the stack may be using it.  A CHECK constraint can
 *     call a function that converts a value of the same domain, and when that
 *     shared this state the nested check overwrote the value the outer one's
 *     expression reads as VALUE and reset the memory it was evaluating in:
 *     with `CHECK (f(VALUE) AND VALUE < 1000)`, where f() converts 5 to the
 *     same domain, 2000 passed.  A pljs f() converts with its own function's
 *     cache, but nothing stops the same FmgrInfo being called again from
 *     inside a check, so a nested check still gets state of its own, built and
 *     freed around that one call.
 *
 *   - The typcache rebuilds a domain's constraints after any constraint change
 *     anywhere -- a temporary table's CHECK included -- and both functions
 *     then build their ExprStates again in the memory they were given without
 *     freeing the old ones, since PostgreSQL expects that memory to be
 *     short-lived.  This lasts as long as the function's FmgrInfo, which can
 *     be a whole procedure or a long return_next() loop, so once the typcache
 *     has rebuilt the domain's constraints the state is freed here and built
 *     again.
 *
 * A type with an inner_domain -- a range over a domain, which is not a domain
 * itself -- is converted here too, through its own input function, since that
 * keeps the inner domain's domain_in() state in the same way and has the same
 * two problems.  The typcache says nothing of that domain to a caller asking
 * about the range, so its state is built again whenever any domain's
 * constraints can have changed; see pljs_type_io_init().
 *
 * @param io #pljs_type_io - the domain, or the type with an inner_domain
 * @param str @c char* - the text to parse, or NULL; for a domain_via_input
 * domain or an inner_domain
 * @param value #Datum - the value to check; for any other domain
 * @param isnull @c bool - whether @p value is SQL NULL
 * @param typmod @c int32 - the typmod to parse @p str with; -1 for a domain,
 * which carries its own
 * @returns #Datum of the domain
 */
static Datum pljs_domain_check(pljs_type_io *io, char *str, Datum value,
                               bool isnull, int32 typmod) {
  MemoryContext nested = NULL;
  MemoryContext mcxt;
  FmgrInfo nested_input;
  FmgrInfo *input = NULL;
  Oid ioparam = InvalidOid;
  void *nested_extra = NULL;
  void **extra;
  Datum ret = value;
  bool via_input = io->domain_via_input || io->inner_domain;
  bool may_check;

  /*
   * Counted for a watching return_next() before anything that can run a
   * constraint -- loading them runs their IMMUTABLE functions -- and let go
   * once there is none to run; see pljs_type_domain_watch_start().  Counted
   * only once they had loaded, an error loading them was taken for one that
   * ran none.
   */
  bool watched = pljs_domain_watch_level != 0 &&
                 GetCurrentTransactionNestLevel() == pljs_domain_watch_level;

  if (watched) {
    pljs_domain_watch_hits++;
  }

  if (io->domain_busy) {
    /* Before the context, which an error here left behind. */
    may_check = pljs_type_may_check_domain(io->typid);
    pljs_domain_check_unwatch(&watched, may_check);

    nested = AllocSetContextCreate(
        CurrentMemoryContext, "PLJS Nested Domain Check", ALLOCSET_SMALL_SIZES);
    mcxt = nested;
    extra = &nested_extra;

    if (via_input) {
      Oid typinput;

      getTypeInputInfo(io->typid, &typinput, &ioparam);
      fmgr_info_cxt(typinput, &nested_input, nested);
      input = &nested_input;
    }
  } else {
    TypeCacheEntry *typentry =
        io->is_domain
            ? lookup_type_cache(io->typid, TYPECACHE_DOMAIN_CONSTR_INFO)
            : NULL;
    /*
     * Only a type with an inner_domain depends on the count, which compares
     * any relation invalidated since; see pljs_domain_relation_invalidate().
     */
    uint64 generation = io->inner_domain ? pljs_type_domain_generation() : 0;

    /*
     * The typcache has rebuilt the domain's constraints since the state was
     * built.  The state holds a reference to the ones it was built from, so
     * a new set cannot turn up at the same address.  Or for an inner_domain,
     * any domain's constraints can have changed.
     */
    if (io->domain_mcxt != NULL &&
        ((typentry != NULL && io->domain_constraints != typentry->domainData) ||
         (io->inner_domain && io->domain_generation != generation))) {
      MemoryContextDelete(io->domain_mcxt);
      io->domain_mcxt = NULL;
    }

    if (io->domain_mcxt == NULL) {
      MemoryContext domain_mcxt;
      bool rebuilt_may_check;
      Oid typinput = InvalidOid;

      /*
       * The types inside are noted again, since a change to one forgets it;
       * see pljs_domain_invalidate().  From the list the type's lookup made,
       * rather than by reading the catalogs again, which cost every rebuild a
       * walk of the whole type -- and every change to pg_constraint anywhere
       * is one.
       */
      if (io->inner_domain) {
        ListCell *lc;

        foreach (lc, io->noted_types) {
          pljs_domain_type_note(lfirst_oid(lc));
        }
      }

      /*
       * Before the context, which an error here lost for as long as the
       * function's type cache lived, one for each retry.
       */
      rebuilt_may_check = pljs_type_may_check_domain(io->typid);
      pljs_domain_check_unwatch(&watched, rebuilt_may_check);

      /*
       * Looked up before the context too, and the context deleted if the rest
       * fails: a type dropped while a function used it -- which JavaScript
       * can catch -- lost one for each retry.
       */
      if (via_input) {
        getTypeInputInfo(io->typid, &typinput, &ioparam);
      }

      domain_mcxt = AllocSetContextCreate(io->mcxt, "PLJS Domain Check",
                                          ALLOCSET_SMALL_SIZES);

      if (via_input) {
        PG_TRY();
        {
          fmgr_info_cxt(typinput, &io->domain_in, domain_mcxt);
        }
        PG_CATCH();
        {
          MemoryContextDelete(domain_mcxt);
          PG_RE_THROW();
        }
        PG_END_TRY();

        io->ioparam = ioparam;
      }

      io->domain_extra = NULL;
      io->domain_constraints = typentry != NULL ? typentry->domainData : NULL;
      io->domain_generation = generation;
      io->domain_may_check = rebuilt_may_check;
      io->domain_mcxt = domain_mcxt;
    }

    may_check = io->domain_may_check;
    pljs_domain_check_unwatch(&watched, may_check);
    mcxt = io->domain_mcxt;
    extra = &io->domain_extra;
    input = &io->domain_in;
    ioparam = io->ioparam;

    io->domain_busy = true;
  }

  PG_TRY();
  {
    if (via_input) {
      ret = InputFunctionCall(input, str, ioparam, typmod);
    } else {
      domain_check(value, isnull, io->typid, extra, mcxt);
    }
  }
  PG_FINALLY();
  {
    pljs_domain_check_done(io, nested);
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
    pljs_domain_check(io, NULL, (Datum)0, true, -1);
  }
}

/**
 * @brief Checks a value built as a domain's base type against the domain's
 * constraints; see pljs_domain_check().
 *
 * For a row of a set of a domain over a composite type, which is built with
 * the set's descriptor; see pljs_put_domain_row().  A composite has a case of
 * its own, so the domain is never converted through its input function.
 *
 * @param typid #Oid - the domain
 * @param value #Datum - the value, of the base type
 * @param isnull @c bool - whether it is SQL NULL
 */
void pljs_type_domain_check(Oid typid, Datum value, bool isnull) {
  pljs_type_io *io = pljs_type_io_lookup(typid);

  Assert(io->is_domain && !io->domain_via_input);

  pljs_domain_check(io, NULL, value, isnull, -1);
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
 * Reading `length` can run JavaScript -- a Proxy, or a getter -- and when that
 * threw, the length came back as whatever was on the stack.  It now returns
 * -1 and leaves the exception pending, for the caller to raise or to hand back
 * to JavaScript.
 *
 * The length is read as JavaScript reads one, ToLength(): a fraction is cut
 * off, and a negative length or NaN is 0.  One too long for an int32_t throws
 * a RangeError.  It was read with ToInt32(), which wrapped a length of 2^31 or
 * more to a negative number, and every caller took that for a length that had
 * thrown, with no exception pending to report.
 *
 * @param obj JSValueConst - Javascript array to check the length of
 * @param ctx #JSContext - Javascript context to execute in
 * @returns @c int32_t length, or -1 with an exception pending
 */
int32_t pljs_js_array_length(JSValueConst obj, JSContext *ctx) {
  JSValue length = JS_GetPropertyStr(ctx, obj, "length");
  double array_length;
  int failed;

  if (JS_IsException(length)) {
    return -1;
  }

  failed = JS_ToFloat64(ctx, &array_length, length);
  JS_FreeValue(ctx, length);

  if (failed < 0) {
    return -1;
  }

  if (isnan(array_length) || array_length <= 0) {
    return 0;
  }

  if (array_length > PG_INT32_MAX) {
    JS_ThrowRangeError(ctx, "array length %.0f is too large", array_length);
    return -1;
  }

  return (int32_t)array_length;
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
  } else if (io->typid == RECORDOID) {
    /*
     * An anonymous row.  Only record: every pseudo-type was taken for one,
     * so a cstring -- `SELECT textout('a')` -- or an anyarray had its datum
     * read as a tuple header.  The others are converted by their output and
     * input functions, which say plainly when a type cannot be.
     */
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
  TupleDesc tupdesc;
  HeapTupleData tuple;

  /*
   * Extract type info from the tuple itself.  A failure is raised, as any
   * other conversion error is.  It was caught and thrown into JavaScript, and
   * the JS_EXCEPTION that returned was then kept as the value -- a column, an
   * argument, an array element -- which corrupted the runtime.
   */
  tupdesc = lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(rec),
                                   HeapTupleHeaderGetTypMod(rec));

  obj = JS_NewObject(ctx);

  if (tupdesc) {
    /*
     * A column that does not convert raises, and the builtins hand that to
     * JavaScript, which can catch it and try again: release the row built so
     * far, and the descriptor, rather than leave them behind.  Only the
     * outermost row was released, so a row inside an array or another row
     * stayed in the runtime.
     */
    PG_TRY();
    {
      pljs_checked_value(ctx, obj);

      for (int16 i = 0; i < tupdesc->natts; i++) {
        Datum datum;
        bool isnull = false;

        if (TupleDescAttr(tupdesc, i)->attisdropped) {
          continue;
        }

        tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
        ItemPointerSetInvalid(&(tuple.t_self));
        tuple.t_tableOid = InvalidOid;
        tuple.t_data = rec;

        datum = heap_getattr(&tuple, i + 1, tupdesc, &isnull);

        /*
         * Defined, not set: setting runs any setter an object inherits, so a
         * setter on Object.prototype ran -- while the arguments were being
         * converted, before the call had an SPI connection -- and took the
         * column's value instead of the row, and a column named __proto__
         * replaced the row's prototype.  So for every object and array built
         * from a PostgreSQL value, as JSON.parse() builds its own.
         */
        pljs_define_column(
            ctx, obj, TupleDescAttr(tupdesc, i),
            pljs_datum_to_jsvalue(TupleDescAttr(tupdesc, i)->atttypid, datum,
                                  isnull, true, ctx));
      }
    }
    PG_CATCH();
    {
      ReleaseTupleDesc(tupdesc);
      JS_FreeValue(ctx, obj);
      pljs_free_if_detoasted(rec, arg);
      PG_RE_THROW();
    }
    PG_END_TRY();

    ReleaseTupleDesc(tupdesc);
  }

  /*
   * A row stored compressed, or out of line, was detoasted and never freed:
   * a copy per row for as long as the function ran, and one per retry of a
   * row that did not convert.  Only once every column has been, since a
   * by-reference column points into it.
   */
  pljs_free_if_detoasted(rec, arg);

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
  JSValue array;
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
    pljs_free_if_detoasted(array_value, arg);

    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot convert a multidimensional array to a JavaScript "
                    "array"),
             errdetail("pljs represents SQL arrays as one-dimensional "
                       "JavaScript arrays.")));
  }

  deconstruct_array(array_value, type->typid, type->length, type->byval,
                    type->align, &values, &nulls, &nelems);

  /*
   * Created only once nothing above can raise.  If an element does not
   * convert, everything is released -- the array built so far, and the
   * elements and detoasted copy it was built from -- since the builtins hand
   * the error to JavaScript, which can catch it and try again.  Only the
   * JavaScript array was, so each retry left a whole detoasted array behind,
   * for as long as the function ran; see pljs_datum_to_object().
   */
  array = JS_NewArray(ctx);

  PG_TRY();
  {
    pljs_checked_value(ctx, array);

    for (int i = 0; i < nelems; i++) {
      JSValue value =
          pljs_datum_to_jsvalue(type->typid, values[i], nulls[i], true, ctx);

      /* Defined, not set; see pljs_datum_to_object(). */
      pljs_checked_define(ctx, JS_DefinePropertyValueUint32(
                                   ctx, array, i, value, JS_PROP_C_W_E));
    }
  }
  PG_CATCH();
  {
    JS_FreeValue(ctx, array);
    pfree(values);
    pfree(nulls);
    pljs_free_if_detoasted(array_value, arg);
    PG_RE_THROW();
  }
  PG_END_TRY();

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
 * It runs in a context of its own, which is emptied once the text has been
 * copied into JavaScript; see pljs_type_io_scratch_begin().  An output function
 * frees at most the string it returns, if the caller does, and not what it
 * allocated on the way there -- range_out()'s buffers, a detoasted copy of its
 * argument -- so in the caller's context those piled up for as long as that
 * lasted: 2kB for every tstzrange, and a pljs.execute() of 50,000 of them grew
 * the backend by 112MB.
 *
 * @param arg #Datum - Postgres datum to convert
 * @param io #pljs_type_io - cached conversion state for the datum's type
 * @param ctx #JSContext - Javascript context
 * @returns #JSValue conversion of the Datum
 */
static JSValue pljs_datum_to_jsvalue_fallback(Datum arg, pljs_type_io *io,
                                              JSContext *ctx) {
  FmgrInfo *output = pljs_type_io_output(io);
  MemoryContext scratch = pljs_type_io_scratch_begin();
  MemoryContext old_context = MemoryContextSwitchTo(scratch);
  volatile JSValue ret;

  PG_TRY();
  {
    char *str = OutputFunctionCall(output, arg);

    ret = pljs_new_server_string(ctx, str, strlen(str));
  }
  PG_FINALLY();
  {
    MemoryContextSwitchTo(old_context);
    pljs_type_io_scratch_end(scratch);
  }
  PG_END_TRY();

  return ret;
}

/**
 * @brief Returns the array type of an array from its elements' type.
 *
 * For an array of a type that names no element type of its own; see
 * pljs_datum_to_jsvalue().
 *
 * @param arg #Datum - an array
 * @returns #Oid of its array type
 */
static Oid pljs_array_type_of(Datum arg) {
  /*
   * Only the header, which names the element type, is detoasted.
   * pljs_datum_to_array() detoasts the whole array, and doing that here as
   * well decompressed every one twice.
   */
  ArrayType *header =
      (ArrayType *)PG_DETOAST_DATUM_SLICE(arg, 0, sizeof(ArrayType) - VARHDRSZ);
  Oid elemtype = ARR_ELEMTYPE(header);
  Oid arraytype = get_array_type(elemtype);

  pljs_free_if_detoasted(header, arg);

  if (!OidIsValid(arraytype)) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cannot convert an array of type %s",
                           format_type_be(elemtype))));
  }

  return arraytype;
}

/**
 * @brief Converts a `JSONB` #Datum to a Javascript value.
 *
 * In a context of its own, which is emptied however the conversion ends; see
 * pljs_type_io_scratch_begin().  The value is detoasted, and a number too
 * large for a double raises part of the way through: the detoasted copy was
 * freed only once the conversion had returned, so a loop that caught the
 * error left a whole document behind each time.
 *
 * @param arg #Datum - the jsonb value
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the value, or JS_EXCEPTION
 */
static JSValue pljs_jsonb_to_jsvalue(Datum arg, JSContext *ctx) {
  MemoryContext scratch = pljs_type_io_scratch_begin();
  MemoryContext old_context = MemoryContextSwitchTo(scratch);
  volatile JSValue ret;

  PG_TRY();
  {
    Jsonb *jsonb = DatumGetJsonbP(arg);

#if JSONB_DIRECT_CONVERSION
    if (JB_ROOT_IS_SCALAR(jsonb)) {
      JsonbValue jb;

      JsonbExtractScalar(&jsonb->root, &jb);
      ret = get_jsonb_value(&jb, ctx);
    } else {
      ret = convert_jsonb(&jsonb->root, ctx);
    }
#else
    // Convert it to a string (takes some casting, but JsonbContainer is also
    // a varlena).
    char *str =
        JsonbToCString(NULL, (JsonbContainer *)VARDATA(jsonb), VARSIZE(jsonb));
    char *utf8 = pljs_server_to_utf8(str, strlen(str));

    ret = JS_ParseJSON(ctx, utf8, strlen(utf8), NULL);
#endif
  }
  PG_FINALLY();
  {
    MemoryContextSwitchTo(old_context);
    pljs_type_io_scratch_end(scratch);
  }
  PG_END_TRY();

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
   * anyarray -- the type of pg_stats' histogram_bounds, and of a polymorphic
   * argument that could not be resolved -- names no element type, but the
   * array itself does.
   */
  if (argtype == ANYARRAYOID || argtype == ANYCOMPATIBLEARRAYOID) {
    argtype = pljs_array_type_of(arg);
  }

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

  /* The cases below are those of pljs_type_has_js_case(), and only those. */
  if (!io->has_js_case) {
    return pljs_checked_value(ctx,
                              pljs_datum_to_jsvalue_fallback(arg, io, ctx));
  }

  switch (type.typid) {
  /*
   * void has no value.  Its output function writes an empty string, which is
   * what a void column -- `SELECT pg_sleep(0) AS v` -- arrived as once only
   * record was taken for a row.
   */
  case VOIDOID:
    return JS_UNDEFINED;

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
    text *text_value = DatumGetTextPP(arg);

    return_result = pljs_new_server_string(ctx, VARDATA_ANY(text_value),
                                           VARSIZE_ANY_EXHDR(text_value));

    pljs_free_if_detoasted(text_value, arg);
    break;
  }

  case NAMEOID: {
    const char *name = NameStr(*DatumGetName(arg));

    return_result = pljs_new_server_string(ctx, name, strlen(name));
    break;
  }

  case JSONOID: {
    // Get a copy of the string.
    text *json_value = DatumGetTextP(arg);
    char *utf8;

    str = pljs_util_dup_pgtext(json_value);
    utf8 = pljs_server_to_utf8(str, strlen(str));

    return_result = JS_ParseJSON(ctx, utf8, strlen(utf8), NULL);

    // free the memory allocated.
    if (utf8 != str) {
      pfree(utf8);
    }

    pfree(str);
    pljs_free_if_detoasted(json_value, arg);
    break;
  }

  case JSONBOID:
    return_result = pljs_jsonb_to_jsvalue(arg, ctx);
    break;

  case BYTEAOID: {
    /*
     * A Uint8Array of the bytes, as plv8 gives, and which the reverse
     * conversion takes back unchanged.  bytea arrived as a String built with
     * JS_NewStringLen(), which reads its input as UTF-8: every invalid
     * sequence became U+FFFD, and the byte after it was lost, so a value that
     * went back to PostgreSQL -- NEW from a trigger, even one that changed
     * some other column -- was rewritten.  '\xdeadbeef' was stored as
     * '\xdeadefbfbd'.
     */
    bytea *value = DatumGetByteaPP(arg);
    JSValue buffer = JS_NewArrayBufferCopy(
        ctx, (const uint8_t *)VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));

    pljs_free_if_detoasted(value, arg);

    if (JS_IsException(buffer)) {
      return_result = buffer;
      break;
    }

    /*
     * new Uint8Array(buffer, undefined, undefined): the constructor reads its
     * offset and length whatever argc says, as a JavaScript call pads them.
     */
    JSValueConst typed_array_args[3] = {buffer, JS_UNDEFINED, JS_UNDEFINED};

    return_result =
        JS_NewTypedArray(ctx, 3, typed_array_args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, buffer);
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
    elog(ERROR, "pljs has no conversion to JavaScript for type %s",
         format_type_be(type.typid));
  }

  return pljs_checked_value(ctx, return_result);
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

  if (array_length < 0) {
    pljs_ereport_js_exception(ctx);
  }

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
     *
     * JS_GetPropertyUint32() returns an owned reference, which the conversion
     * releases.
     */
    values[i] = pljs_jsvalue_to_datum_typmod_free(type->typid, typmod, elem,
                                                  &nulls[i], ctx);
  }

  result = construct_md_array(values, nulls, 1, ndims, lbs, type->typid,
                              type->length, type->byval, type->align);
  pfree(values);
  pfree(nulls);

  return PointerGetDatum(result);
}

/**
 * @brief Reads a column of a row that has to have every column.
 *
 * The column is a property of the row, its own or one it inherits -- a getter
 * of its class, say -- but not one every object inherits from
 * Object.prototype.  Asking whether the row had one at all found those, so a
 * row with no "toString" or "constructor" property was taken to have that
 * column, and the builtin function was stored as its value.  The value is
 * read with the same lookup: the row was looked up once to see whether it had
 * each column and again to read it.
 *
 * @param ctx #JSContext - Javascript context
 * @param row #JSValueConst - the row, an object
 * @param atom #JSAtom - the column's key
 * @param object_proto #JSValueConst - Object.prototype, where the search stops
 * @param found @c bool* - set to whether the row has the column
 * @returns #JSValue - the value, owned; JS_UNDEFINED if it was not found; or
 * JS_EXCEPTION
 */
static JSValue pljs_row_column(JSContext *ctx, JSValueConst row, JSAtom atom,
                               JSValueConst object_proto, bool *found) {
  JSValue holder = JS_DupValue(ctx, row);

  *found = false;

  for (;;) {
    JSPropertyDescriptor desc;
    JSValue proto;
    int has;

    /*
     * A Proxy says what reading a property gives in its get trap, and has the
     * column if that is anything but undefined -- a trap can make columns
     * up -- or if it has the property as its own.  So the value is read as
     * reading the property reads it, as a record's is: it was taken from the
     * property's descriptor, and a Proxy row stored what its target held,
     * where the same object returned as a record stored what the trap gave.
     *
     * Except what every object inherits: a trap that forwards to a plain
     * object gives Object.prototype's constructor() and valueOf() for those
     * keys, which are no more a column of the Proxy than of its target.  They
     * were stored as the column's value.
     */
    if (JS_GetClassID(holder) == JS_CLASS_PROXY) {
      JSValue value = JS_GetProperty(ctx, row, atom);

      if (JS_IsException(value)) {
        JS_FreeValue(ctx, holder);
        return JS_EXCEPTION;
      }

      if (JS_IsUndefined(value)) {
        has = JS_GetOwnProperty(ctx, NULL, holder, atom);
      } else {
        JSValue inherited = JS_GetProperty(ctx, object_proto, atom);

        if (JS_IsException(inherited)) {
          has = -1;
        } else if (JS_SameValue(ctx, value, inherited)) {
          has = JS_GetOwnProperty(ctx, NULL, holder, atom);
        } else {
          has = 1;
        }

        JS_FreeValue(ctx, inherited);
      }

      JS_FreeValue(ctx, holder);

      if (has <= 0) {
        JS_FreeValue(ctx, value);
        return has < 0 ? JS_EXCEPTION : JS_UNDEFINED;
      }

      *found = true;

      return value;
    }

    has = JS_GetOwnProperty(ctx, &desc, holder, atom);

    if (has < 0) {
      JS_FreeValue(ctx, holder);
      return JS_EXCEPTION;
    }

    if (has) {
      JSValue value = JS_UNDEFINED;

      JS_FreeValue(ctx, holder);
      *found = true;

      /* A getter is called on the row, as reading the property calls it. */
      if (desc.flags & JS_PROP_GETSET) {
        if (JS_IsFunction(ctx, desc.getter)) {
          value = JS_Call(ctx, desc.getter, row, 0, NULL);
        }

        JS_FreeValue(ctx, desc.value);
      } else {
        value = desc.value;
      }

      JS_FreeValue(ctx, desc.getter);
      JS_FreeValue(ctx, desc.setter);

      return value;
    }

    proto = JS_GetPrototype(ctx, holder);
    JS_FreeValue(ctx, holder);

    if (JS_IsException(proto)) {
      return JS_EXCEPTION;
    }

    if (!JS_IsObject(proto) ||
        JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(object_proto)) {
      JS_FreeValue(ctx, proto);
      return JS_UNDEFINED;
    }

    holder = proto;
  }
}

/**
 * @brief Whether an object converts itself to a value.
 *
 * That is, whether it has a toString(), valueOf() or [Symbol.toPrimitive]()
 * other than every object's -- Object.prototype's, whatever that is now --
 * its own, or its class's.  Only an object whose one property was its own
 * toString() or valueOf() counted, so an instance of a class that converts
 * itself -- Money, with valueOf() giving cents / 100 -- was read as a row, and
 * its one field stored in its place.
 *
 * @param ctx #JSContext - Javascript context
 * @param obj #JSValueConst - the object
 * @returns @c int - 1 if it does, 0 if not, or -1 with an exception pending
 */
int pljs_converts_itself(JSContext *ctx, JSValueConst obj) {
  JSValue object_proto = JS_GetClassProto(ctx, JS_CLASS_OBJECT);
  int ret = 0;

  for (int i = 0; i < 3 && ret == 0; i++) {
    JSAtom atom = pljs_conversion_atoms[i];
    JSValue method;

    if (atom == JS_ATOM_NULL) {
      continue;
    }

    method = JS_GetProperty(ctx, obj, atom);

    if (JS_IsException(method)) {
      ret = -1;
    } else if (JS_IsFunction(ctx, method)) {
      JSValue inherited = JS_GetProperty(ctx, object_proto, atom);

      if (JS_IsException(inherited)) {
        ret = -1;
      } else {
        ret = !JS_SameValue(ctx, method, inherited);
      }

      JS_FreeValue(ctx, inherited);
    }

    JS_FreeValue(ctx, method);
  }

  JS_FreeValue(ctx, object_proto);

  return ret;
}

/**
 * @brief Whether a type is json or jsonb, or a domain over either, which
 * takes any JavaScript value as a document.
 *
 * @param typid #Oid - the type
 * @returns @c bool
 */
bool pljs_type_is_json(Oid typid) {
  Oid base = pljs_type_base(typid);

  return base == JSONOID || base == JSONBOID;
}

/**
 * @brief Whether a row object's column refuses the value the row gives for
 * it: a function.
 *
 * A method -- a class's, found as a getter is -- is not a column's value, and
 * no column takes one: its source was stored as text, and NaN as a number.
 * Except a json or jsonb column, where a function is SQL NULL, as it is
 * anywhere in a document; see docs/TYPES.md.  Only for the columns a row
 * object names: a value converted any other way -- a set's bare value, a
 * field of a composite column -- converts a function as it always has.
 *
 * @param ctx #JSContext - Javascript context
 * @param value #JSValueConst - the value
 * @param typid #Oid - the column's type
 * @returns @c bool
 */
bool pljs_column_refuses(JSContext *ctx, JSValueConst value, Oid typid) {
  return JS_IsFunction(ctx, value) && !pljs_type_is_json(typid);
}

/**
 * @brief Raises that a row gave a function for a column; see
 * pljs_column_refuses().
 *
 * @param name @c char* - the column's name
 * @param caller @c char* - what the row was given to
 */
pg_noreturn void pljs_function_column_error(const char *name,
                                            const char *caller) {
  ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                  errmsg("%s: the object's \"%s\" is a function, which no "
                         "result column takes",
                         caller, name)));
}

/**
 * @brief Reads a column of a row by its key; see pljs_row_column().
 *
 * For a single-column set's row, which is read as a composite set's is; see
 * pljs_single_column_value().
 *
 * @param ctx #JSContext - Javascript context
 * @param row #JSValueConst - the row, an object
 * @param atom #JSAtom - the column's key
 * @param found @c bool* - set to whether the row has the column
 * @returns #JSValue - the value, owned; JS_UNDEFINED if it was not found; or
 * JS_EXCEPTION
 */
JSValue pljs_row_get_column(JSContext *ctx, JSValueConst row, JSAtom atom,
                            bool *found) {
  JSValue object_proto = JS_GetClassProto(ctx, JS_CLASS_OBJECT);
  JSValue value = pljs_row_column(ctx, row, atom, object_proto, found);

  JS_FreeValue(ctx, object_proto);

  return value;
}

/**
 * @brief Raises that a row has no property for a column.
 *
 * Naming the column, and what the row does have, since the usual cause is a
 * case difference: JavaScript's property names are case sensitive, and
 * PostgreSQL folds an unquoted identifier to lower case, so a `MixedCol` key
 * never matches a `mixedcol` column.  The bare "field name / property name
 * mismatch" left the author to guess.
 *
 * @param ctx #JSContext - Javascript context
 * @param row #JSValueConst - the row
 * @param attr #Form_pg_attribute - the column
 * @param caller @c char* - what the row was given to
 */
pg_noreturn static void pljs_missing_column(JSContext *ctx, JSValueConst row,
                                            Form_pg_attribute attr,
                                            const char *caller) {
  uint32_t object_keys_length = 0;
  JSPropertyEnum *tab;
  StringInfoData keys;
  uint32_t listed = 0;

  /*
   * Cap the list.  An object with ten thousand properties would otherwise
   * produce a ten-thousand-name error message, which goes to the server log as
   * well as to the client.  Ten names plus the total is enough to diagnose a
   * typo, which is what this message is for.
   */
  const uint32_t max_listed = 10;

  /* A Proxy's trap that threw: report that, and not the column. */
  if (JS_GetOwnPropertyNames(ctx, &tab, &object_keys_length, row,
                             JS_GPN_STRING_MASK) < 0) {
    pljs_ereport_js_exception(ctx);
  }

  initStringInfo(&keys);

  for (uint32_t object_key = 0; object_key < object_keys_length; object_key++) {
    const char *key;

    if (listed >= max_listed) {
      appendStringInfo(&keys, ", ... (%u properties in total)",
                       object_keys_length);
      break;
    }

    key = JS_AtomToCString(ctx, tab[object_key].atom);

    if (key == NULL) {
      JS_FreeValue(ctx, JS_GetException(ctx));
      continue;
    }

    if (keys.len > 0) {
      appendStringInfoString(&keys, ", ");
    }

    appendStringInfoString(&keys, key);
    JS_FreeCString(ctx, key);
    listed++;
  }

  pljs_free_prop_enum(ctx, tab, object_keys_length);

  /*
   * In the database's encoding, as the rest is.  Before ereport(), whose
   * arguments are evaluated once it has begun its message.
   */
  const char *provided = keys.len > 0
                             ? pljs_utf8_to_server_lossy(keys.data, keys.len)
                             : "no properties";

  ereport(ERROR,
          (errcode(ERRCODE_DATATYPE_MISMATCH),
           errmsg("%s: result column \"%s\" has no matching property (object "
                  "has: %s; property names are case sensitive)",
                  caller, NameStr(attr->attname), provided)));
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
                              TupleDesc tupdesc, JSContext *ctx,
                              const char *caller) {
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

  /*
   * A row of a set has to have every column, which is checked as each is
   * read; see pljs_row_column().  Any other row -- a record, a trigger's NEW
   * -- takes a column it does not have as NULL.
   */
  JSValue object_proto =
      caller != NULL ? JS_GetClassProto(ctx, JS_CLASS_OBJECT) : JS_UNDEFINED;

  /*
   * A column that does not convert raises, and return_next() hands that to
   * JavaScript without a subtransaction to release what this pinned: each
   * caught error left a reference to the row type's descriptor, and COMMIT
   * warned of every one.
   */
  PG_TRY();
  {
    for (int16 c = 0; c < tupdesc->natts; c++) {
      // If this is a dropped column, we can skip it, and set the null flag to
      // true.
      if (TupleDescAttr(tupdesc, c)->attisdropped) {
        (*is_null)[c] = true;
        continue;
      }

      // Retrieve the column name of each attribute that we are expecting, we
      // only care about named tuples.
      Form_pg_attribute attr = TupleDescAttr(tupdesc, c);
      JSValue o;

      if (caller == NULL) {
        o = pljs_get_column(ctx, val, attr);
      } else {
        JSAtom atom = pljs_column_atom(ctx, attr);
        bool found;

        if (atom == JS_ATOM_NULL) {
          pljs_ereport_js_exception(ctx);
        }

        o = pljs_row_column(ctx, val, atom, object_proto, &found);
        JS_FreeAtom(ctx, atom);

        if (!JS_IsException(o) && !found) {
          pljs_missing_column(ctx, val, attr, caller);
        }

        /* See pljs_column_refuses(). */
        if (pljs_column_refuses(ctx, o, attr->atttypid)) {
          JS_FreeValue(ctx, o);
          pljs_function_column_error(NameStr(attr->attname), caller);
        }
      }

      // Set the value of each Datum, or set the `is_null` flag if it is
      // considered `NULL`.  The column's typmod applies: a trigger's NEW, or a
      // composite with a varchar(n) or bit(n) column, is not re-checked by the
      // executor.
      //
      // JS_GetPropertyStr() returns an owned reference, so it has to be
      // released whatever the column's value, and whether or not it converts.
      // Leaking it costs one QuickJS reference per column per row, on every
      // composite return and every return_next() of a row object, which is the
      // hottest allocation path in the extension.  Because QuickJS runs on the
      // libc allocator the loss is invisible to pg_backend_memory_contexts; it
      // counts against pljs.memory_limit and is not returned until the backend
      // exits.
      values[c] = pljs_jsvalue_to_datum_typmod_free(
          TupleDescAttr(tupdesc, c)->atttypid,
          TupleDescAttr(tupdesc, c)->atttypmod, o, &(*is_null)[c], ctx);
    }
  }
  PG_FINALLY();
  {
    JS_FreeValue(ctx, object_proto);

    if (cleanup_tupdesc) {
      ReleaseTupleDesc(tupdesc);
    }
  }
  PG_END_TRY();

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

  /* Release the descriptor however this ends; see pljs_jsvalue_to_datums(). */
  PG_TRY();
  {
    for (int16 c = 0; c < tupdesc->natts; c++) {
      if (TupleDescAttr(tupdesc, c)->attisdropped) {
        nulls[c] = true;
        continue;
      }

      JSValue o = pljs_get_column(ctx, val, TupleDescAttr(tupdesc, c));

      /*
       * Owned reference: release it on both paths.  See
       * pljs_jsvalue_to_datums().
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

      values[c] = pljs_jsvalue_to_datum_typmod_free(
          TupleDescAttr(tupdesc, c)->atttypid,
          TupleDescAttr(tupdesc, c)->atttypmod, o, &nulls[c], ctx);
    }

    // Form a Tuple from the values and nulls using the tuple descriptor
    // as the template for the tuple.
    result = HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls));
  }
  PG_FINALLY();
  {
    if (cleanup_tupdesc) {
      ReleaseTupleDesc(tupdesc);
    }
  }
  PG_END_TRY();

  pfree(nulls);
  pfree(values);

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

  pljs_checked_value(ctx, is_set_null_value);

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
  MemoryContext caller_context = CurrentMemoryContext;
  MemoryContext scratch;
  Datum ret;

  /* toString() threw, or QuickJS ran out of memory. */
  if (str == NULL) {
    pljs_ereport_js_exception(ctx);
  }

  if (memchr(str, '\0', plen) != NULL) {
    JS_FreeCString(ctx, str);
    ereport(ERROR,
            (errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
             errmsg("null byte (\\u0000) is not allowed in a value of type %s",
                    format_type_be(io->typid))));
  }

  /*
   * The input function runs in a context of its own, and only the value it
   * returns is copied out of it; see pljs_datum_to_jsvalue_fallback().
   * range_in() leaves its parse state behind, and 500,000 rows of a daterange
   * set grew the backend by more than a gigabyte.
   */
  scratch = pljs_type_io_scratch_begin();

  PG_TRY();
  {
    char *text;

    MemoryContextSwitchTo(scratch);

    text = pljs_signature_to_oid_text(io, pljs_utf8_to_server(str, plen));

    if (io->is_domain || io->inner_domain) {
      ret = pljs_domain_check(io, text, (Datum)0, false, typmod);
    } else {
      /* Looked up first: it is what sets io->ioparam. */
      FmgrInfo *input = pljs_type_io_input(io);

      ret = InputFunctionCall(input, text, io->ioparam, typmod);
    }

    MemoryContextSwitchTo(caller_context);
    ret = datumCopy(ret, io->byval, io->length);
  }
  PG_FINALLY();
  {
    MemoryContextSwitchTo(caller_context);
    pljs_type_io_scratch_end(scratch);

    /* Do not leak the QuickJS C-string when the input function rejects it. */
    JS_FreeCString(ctx, str);
  }
  PG_END_TRY();

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

  /* valueOf() threw. */
  if (JS_ToFloat64(ctx, &d, val) < 0) {
    pljs_ereport_js_exception(ctx);
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
    pljs_ereport_js_exception(ctx);
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

  /* The cases below are those of pljs_type_has_js_case(), and only those. */
  if (!io->has_js_case) {
    return pljs_jsvalue_to_datum_fallback(val, is_null, io, typmod, ctx);
  }

  switch (rettype) {
  case VOIDOID:
    PG_RETURN_VOID();
    break;

  case OIDOID: {
    int64_t in;

    /*
     * Each JS_To*() below runs valueOf() or toString() on an object, and each
     * used to ignore that it threw, converting whatever was on the stack: an
     * oid became 0 and a float8 or numeric NaN.
     */
    if (JS_ToInt64(ctx, &in, val) < 0) {
      pljs_ereport_js_exception(ctx);
    }

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

    if (JS_ToFloat64(ctx, &in, val) < 0) {
      pljs_ereport_js_exception(ctx);
    }

    PG_RETURN_FLOAT4((float4)in);
    break;
  }

  case FLOAT8OID: {
    double in;

    if (JS_ToFloat64(ctx, &in, val) < 0) {
      pljs_ereport_js_exception(ctx);
    }

    PG_RETURN_FLOAT8(in);
    break;
  }

  case NUMERICOID: {
    /*
     * A string carries exact decimal text, including a scale no double can
     * represent, so parse it with numeric's input function rather than routing
     * it through float8: "12345678901234567890.123456789" came back as
     * 12345678901234600000.
     *
     * A BigInt's decimal text is parsed the same way.  It was rendered with
     * JS_ToString() and JS_ToCString(), neither of which was freed or checked,
     * so every value leaked both, and a string QuickJS had no memory for was
     * handed to numeric_in() as NULL.
     */
    if (JS_IsString(val) || JS_IsBigInt(ctx, val)) {
      return pljs_string_to_datum_via_input(NUMERICOID, val, ctx);
    }

    double in;

    if (JS_ToFloat64(ctx, &in, val) < 0) {
      pljs_ereport_js_exception(ctx);
    }

    return DirectFunctionCall1(float8_numeric, Float8GetDatum((float8)in));
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
      pljs_ereport_js_exception(ctx);
    }

    PG_TRY();
    {
      char *server = pljs_utf8_to_server(str, strlen(str));

      ret = DirectFunctionCall1(namein, CStringGetDatum(server));

      if (server != str) {
        pfree(server);
      }
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

  case XMLOID:
    /*
     * Parsed by xml's input function, which rejects what is not XML.  It was
     * built the way text is, and stored whatever it was given.
     */
    return pljs_string_to_datum_via_input(XMLOID, val, ctx);

  case TEXTOID:
  case VARCHAROID:
  case BPCHAROID: {
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, val);

    /*
     * JS_ToCStringLen() returns NULL when the value cannot be rendered -- an
     * out-of-memory under pljs.memory_limit, or a toString() that threw.
     * CStringGetTextDatum() would hand that straight to strlen().
     */
    if (str == NULL) {
      pljs_ereport_js_exception(ctx);
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

    char *server = pljs_js_string_to_server(ctx, str, plen);
    Datum ret;

    if (server == str) {
      ret = PointerGetDatum(cstring_to_text_with_len(str, plen));
    } else {
      ret = PointerGetDatum(cstring_to_text(server));
      pfree(server);
    }

    JS_FreeCString(ctx, str);

    return ret;
    break;
  }

  case JSONOID: {
    JSValueConst *argv = &val;
    JSValue js = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    size_t plen;
    const char *str;

    /*
     * A toJSON() or getter that threw.  Its JS_EXCEPTION rendered to a NULL
     * string, which CStringGetTextDatum() passed to strlen() and crashed the
     * backend.
     */
    pljs_checked_value(ctx, js);

    /*
     * JSON.stringify() has no JSON for a function or a Symbol, and returns
     * undefined, which was stored as the text "undefined": not JSON, so the
     * value broke json_typeof() and a cast to jsonb later.  It is SQL NULL, as
     * undefined itself is.
     */
    if (JS_IsUndefined(js)) {
      return pljs_null_datum(is_null, fcinfo);
    }

    str = JS_ToCStringLen(ctx, &plen, js);
    JS_FreeValue(ctx, js);

    if (str == NULL) {
      pljs_ereport_js_exception(ctx);
    }

    // return it as a text Datum, in the database's encoding.
    char *server = pljs_js_string_to_server(ctx, str, plen);
    Datum ret = CStringGetTextDatum(server);

    if (server != str) {
      pfree(server);
    }

    JS_FreeCString(ctx, str);

    return ret;
    break;
  }

  case JSONBOID: {
    JSValueConst *argv = &val;
#if JSONB_DIRECT_CONVERSION
    {
      Jsonb *obj = convert_object(argv[0], ctx);

      /* As JSONOID: JSON.stringify() has no JSON for a function, say. */
      if (obj == NULL) {
        return pljs_null_datum(is_null, fcinfo);
      }

      PG_RETURN_JSONB_P(obj);
    }
#else // JSONB_DIRECT_CONVERSION
    JSValue js = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);

    pljs_checked_value(ctx, js);

    /* See JSONOID. */
    if (JS_IsUndefined(js)) {
      return pljs_null_datum(is_null, fcinfo);
    }

    const char *str = JS_ToCString(ctx, js);

    JS_FreeValue(ctx, js);

    if (str == NULL) {
      pljs_ereport_js_exception(ctx);
    }

    // return it as a Datum, since there is no direct CStringGetJsonb exposed.
    char *server = pljs_js_string_to_server(ctx, str, strlen(str));
    Datum ret;

    PG_TRY();
    {
      ret = (Datum)DatumGetJsonbP(
          DirectFunctionCall1(jsonb_in, CStringGetDatum(server)));
    }
    PG_FINALLY();
    {
      if (server != str) {
        pfree(server);
      }

      JS_FreeCString(ctx, str);
    }
    PG_END_TRY();

    return ret;
#endif
    break;
  }

  case BYTEAOID: {
    size_t psize;
    uint8_t *buffer;

    if (Is_ArrayType(val, JS_CLASS_UINT8_ARRAY) ||
        Is_ArrayType(val, JS_CLASS_INT8_ARRAY) ||
        Is_ArrayType(val, JS_CLASS_UINT16_ARRAY) ||
        Is_ArrayType(val, JS_CLASS_INT16_ARRAY) ||
        Is_ArrayType(val, JS_CLASS_UINT32_ARRAY) ||
        Is_ArrayType(val, JS_CLASS_INT32_ARRAY)) {
      /*
       * The bytes the view covers, in the machine's byte order, as they were
       * copied element by element.  A bytea now reaches JavaScript as a
       * Uint8Array, so this is also the way back for every one read from the
       * database, and reading it a property at a time cost a lookup per byte.
       */
      size_t offset, length;
      JSValue array_buffer =
          JS_GetTypedArrayBuffer(ctx, val, &offset, &length, NULL);

      /* A detached buffer. */
      pljs_checked_value(ctx, array_buffer);

      uint8_t *data = JS_GetArrayBuffer(ctx, &psize, array_buffer);

      JS_FreeValue(ctx, array_buffer);

      if (data == NULL) {
        pljs_ereport_js_exception(ctx);
      }

      buffer = palloc(VARHDRSZ + length);

      SET_VARSIZE(buffer, length + VARHDRSZ);
      memcpy(VARDATA(buffer), data + offset, length);

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

      if (str == NULL) {
        pljs_ereport_js_exception(ctx);
      }

      buffer = palloc(str_length + VARHDRSZ);

      SET_VARSIZE(buffer, str_length + VARHDRSZ);
      memcpy(VARDATA(buffer), str, str_length);

      JS_FreeCString(ctx, str);

      return PointerGetDatum(buffer);
    } else {
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

      /* A Date's valueOf() can be replaced, and the replacement can throw. */
      if (JS_ToFloat64(ctx, &in, val) < 0) {
        pljs_ereport_js_exception(ctx);
      }

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
    elog(ERROR, "pljs has no conversion from JavaScript for type %s",
         format_type_be(rettype));
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
  /*
   * A getter that threw while this value was read -- a column of a record or
   * row, or an element of an array.  Its JS_EXCEPTION was converted like any
   * other value, so the error said "could not convert JavaScript value to a
   * number" instead of what the getter threw, or a text column stored an
   * empty string.
   */
  pljs_checked_value(ctx, val);

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

    pljs_domain_check(io, NULL, ret, isnull, -1);
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
 * typmod, which is every target except a column or a plan parameter declared
 * with one.
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
 * @brief Converts a value of a given typmod, and releases it.
 *
 * For a value read from an array or object -- an element, a column, a
 * parameter -- whose reference the caller owns.  It is released whether the
 * conversion returns or raises.  Released only after a conversion returned, a
 * value that could not be converted stayed in the runtime for the life of the
 * backend, and a return_next() in a loop that caught its errors ran
 * pljs.memory_limit out; the domain and typmod checks raise for ordinary bad
 * input.
 *
 * @param typid #Oid - the target type
 * @param typmod @c int32 - the target's typmod, or -1
 * @param val #JSValue - the value, which is released
 * @param is_null @c bool* - set to whether the result is null
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Datum of the Postgres value
 */
Datum pljs_jsvalue_to_datum_typmod_free(Oid typid, int32 typmod, JSValue val,
                                        bool *is_null, JSContext *ctx) {
  Datum ret;

  PG_TRY();
  {
    ret = pljs_jsvalue_to_datum_typmod(typid, typmod, val, is_null, ctx, NULL);
  }
  PG_FINALLY();
  {
    JS_FreeValue(ctx, val);
  }
  PG_END_TRY();

  return ret;
}

/**
 * @brief Converts a value, and releases it.
 *
 * See pljs_jsvalue_to_datum_typmod_free(); this is the form for a target
 * without a typmod.
 *
 * @param rettype #Oid - the target type
 * @param val #JSValue - the value, which is released
 * @param is_null @c bool* - set to whether the result is null
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Datum of the Postgres value
 */
Datum pljs_jsvalue_to_datum_free(Oid rettype, JSValue val, bool *is_null,
                                 JSContext *ctx) {
  return pljs_jsvalue_to_datum_typmod_free(rettype, -1, val, is_null, ctx);
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
    /*
     * JS_DefinePropertyValueUint32() takes ownership of the value, and these
     * are the caller's arguments, borrowed from QuickJS.  Without the dup,
     * freeing the array released references it never held:
     * `pljs.prepare(sql, 'int8')` freed a string constant of the calling
     * function, and the backend crashed when that function's bytecode was
     * freed.
     */
    JS_DefinePropertyValueUint32(ctx, ret, current, JS_DupValue(ctx, array[i]),
                                 JS_PROP_C_W_E);
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

  /*
   * A column that cannot be converted raises, and pljs.execute() and the
   * other builtins hand that to JavaScript, which can catch it and try again:
   * release the row built so far rather than leave it in the runtime.
   */
  PG_TRY();
  {
    pljs_checked_value(ctx, obj);

    for (int i = 0; i < tupledesc->natts; i++) {
      FormData_pg_attribute *tuple_attrs = TupleDescAttr(tupledesc, i);
      if (tuple_attrs->attisdropped) {
        continue;
      }

      bool isnull;
      Datum datum = heap_getattr(heap_tuple, i + 1, tupledesc, &isnull);

      /* Defined, not set; see pljs_datum_to_object(). */
      pljs_define_column(ctx, obj, tuple_attrs,
                         pljs_datum_to_jsvalue(tuple_attrs->atttypid, datum,
                                               isnull, true, ctx));
    }
  }
  PG_CATCH();
  {
    JS_FreeValue(ctx, obj);
    PG_RE_THROW();
  }
  PG_END_TRY();

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

    /* Release the rows built so far; see pljs_tuple_to_jsvalue(). */
    PG_TRY();
    {
      pljs_checked_value(ctx, obj);

      for (int r = 0; r < nrows; r++) {
        JSValue value =
            pljs_tuple_to_jsvalue(tupdesc, SPI_tuptable->vals[r], ctx);

        pljs_checked_define(ctx, JS_DefinePropertyValueUint32(
                                     ctx, obj, r, value, JS_PROP_C_W_E));
      }
    }
    PG_CATCH();
    {
      JS_FreeValue(ctx, obj);
      PG_RE_THROW();
    }
    PG_END_TRY();

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
    // A `String`, or an object's key.
    return pljs_new_server_string(ctx, scalar_value->val.string.val,
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

/*
 * A conversion of a jsonb value to JavaScript: the iterator, and an object's
 * key that has been read while its value has not.
 */
typedef struct jsonb_to_js {
  JSContext *ctx;
  JsonbIterator *it;
  JSAtom key;
} jsonb_to_js;

/**
 * @brief Puts a value into the array or object being built from jsonb.
 *
 * The value is an element of an array, or the value of an object's pending
 * key.  A value QuickJS could not make, or had no memory to store, raises;
 * each was kept, and an object's key it could not make was passed on as a
 * NULL name.
 *
 * @param conversion #jsonb_to_js - the conversion
 * @param container #JSValueConst - the array or object
 * @param is_array @c bool - whether @p container is an array
 * @param count @c uint32_t* - the array's length so far
 * @param value #JSValue - an owned reference, which is taken
 */
static void jsonb_to_js_put(jsonb_to_js *conversion, JSValueConst container,
                            bool is_array, uint32_t *count, JSValue value) {
  JSContext *ctx = conversion->ctx;
  JSAtom key = conversion->key;
  int ret;

  pljs_checked_value(ctx, value);

  // Defined, not set, throughout; see pljs_datum_to_object().
  if (is_array) {
    ret = JS_DefinePropertyValueUint32(ctx, container, (*count)++, value,
                                       JS_PROP_C_W_E);
  } else {
    conversion->key = JS_ATOM_NULL;
    ret = JS_DefinePropertyValue(ctx, container, key, value, JS_PROP_C_W_E);
    JS_FreeAtom(ctx, key);
  }

  pljs_checked_define(ctx, ret);
}

/**
 * @brief Fills an array or object from a #JsonbIterator.
 *
 * Iterates through a `JSONB` array or object and creates the proper
 * Javascript type for each value: `Number`, `String`, `Bool`, `Array`,
 * `Object`, recursing into each array and object.
 *
 * A nested array or object is put into its container before it is filled, so
 * that the outermost holds everything built so far, and convert_jsonb()
 * releases that if a value does not convert: a number too large for a
 * double, or a string pljs.memory_limit has no room for.  Each level was
 * leaked when one raised.
 *
 * @param conversion #jsonb_to_js - the conversion
 * @param container #JSValueConst - the array or object to fill
 * @param is_array @c bool - whether @p container is an array
 */
static void jsonb_iterate(jsonb_to_js *conversion, JSValueConst container,
                          bool is_array) {
  JSContext *ctx = conversion->ctx;
  JsonbValue value;
  JsonbIteratorToken token;
  uint32_t count = 0;

  /* A jsonb value can be nested as deep as its parser's stack allowed. */
  check_stack_depth();

  while ((token = JsonbIteratorNext(&conversion->it, &value, false)) !=
         WJB_DONE) {
    switch (token) {
    case WJB_BEGIN_OBJECT:
    case WJB_BEGIN_ARRAY: {
      bool inner_is_array = (token == WJB_BEGIN_ARRAY);
      JSValue inner = inner_is_array ? JS_NewArray(ctx) : JS_NewObject(ctx);

      /* The container holds the only reference from here on. */
      jsonb_to_js_put(conversion, container, is_array, &count, inner);
      jsonb_iterate(conversion, inner, inner_is_array);
      break;
    }

    case WJB_END_OBJECT:
    case WJB_END_ARRAY:
      return;

    case WJB_KEY: {
      JSValue key = pljs_checked_value(ctx, get_jsonb_value(&value, ctx));

      /* As an atom, so that a key holding "\u0000" is not cut short. */
      conversion->key = JS_ValueToAtom(ctx, key);
      JS_FreeValue(ctx, key);

      if (conversion->key == JS_ATOM_NULL) {
        pljs_ereport_js_exception(ctx);
      }

      break;
    }

    case WJB_VALUE:
    case WJB_ELEM:
      jsonb_to_js_put(conversion, container, is_array, &count,
                      get_jsonb_value(&value, ctx));
      break;

    default:
      elog(ERROR, "unknown jsonb iterator value");
    }
  }
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
  /*
   * Not on the stack: the PG_CATCH reads the key the conversion holds.  In
   * pljs_jsonb_to_jsvalue()'s context, which releases it however this ends.
   */
  jsonb_to_js *conversion = palloc0(sizeof(jsonb_to_js));
  JsonbValue val;
  JsonbIteratorToken token;
  bool is_array;

  // `JSONB` objects always need to be an `Array` or `Object`.
  JSValue container;

  conversion->ctx = ctx;
  conversion->key = JS_ATOM_NULL;
  conversion->it = JsonbIteratorInit(in);
  token = JsonbIteratorNext(&conversion->it, &val, false);

  // If this is an array, then create an `Array`, and otherwise an `Object`.
  is_array = (token == WJB_BEGIN_ARRAY);
  container =
      pljs_checked_value(ctx, is_array ? JS_NewArray(ctx) : JS_NewObject(ctx));

  PG_TRY();
  {
    jsonb_iterate(conversion, container, is_array);
  }
  PG_CATCH();
  {
    JS_FreeAtom(ctx, conversion->key);
    JS_FreeValue(ctx, container);
    PG_RE_THROW();
  }
  PG_END_TRY();

  pfree(conversion);

  return container;
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

/*
 * A QuickJS value that a jsonb conversion holds a reference to, with an
 * object's property names while its properties are converted.
 */
typedef struct jsonb_held {
  JSValue value;
  JSPropertyEnum *tab;
  uint32_t tab_length;
  bool entered; // an array or object being converted
} jsonb_held;

/*
 * A conversion of a JavaScript value to jsonb.
 *
 * It holds a reference to every array and object it is inside, and to the
 * element or property it is converting in each.  Converting one can raise at
 * any depth -- a getter or toJSON() that threw, a NUL character, a cycle --
 * and each level released what it held only once the level below it had
 * returned, so every level above the error leaked, and return_next() in a
 * loop that caught its errors ran pljs.memory_limit out.  What the conversion
 * holds is kept here instead, and convert_object() releases it however the
 * conversion ends.
 */
typedef struct jsonb_build {
  JSContext *ctx;
  JsonbBuildState pstate;
  JSAtom to_json;
  jsonb_held *held; // the last held last
  int nheld;
  int maxheld;
  JsonbValue *result;
} jsonb_build;

// Forward declarations of the conversion functions.
static JsonbValue *jsonb_object_from_object(jsonb_build *build,
                                            JSValueConst object);
static JsonbValue *jsonb_array_from_array(jsonb_build *build,
                                          JSValueConst array);

/**
 * @brief Holds a reference to a value until jsonb_release().
 *
 * @param build #jsonb_build - the conversion
 * @param value #JSValue - an owned reference, which the conversion takes
 * @returns @c int - the value's place in build->held, which can move
 */
static int jsonb_hold(jsonb_build *build, JSValue value) {
  if (build->nheld == build->maxheld) {
    build->maxheld *= 2;
    build->held =
        repalloc(build->held, sizeof(jsonb_held) * (Size)build->maxheld);
  }

  build->held[build->nheld] = (jsonb_held){.value = value};

  return build->nheld++;
}

/**
 * @brief Releases the value held last, with its property names.
 *
 * @param build #jsonb_build - the conversion
 */
static void jsonb_release(jsonb_build *build) {
  jsonb_held held = build->held[--build->nheld];

  pljs_free_prop_enum(build->ctx, held.tab, held.tab_length);
  JS_FreeValue(build->ctx, held.value);
}

/**
 * @brief Fills a #JsonbValue with a string, refusing a NUL character.
 *
 * jsonb cannot hold "\u0000" -- jsonb_in() rejects it -- and a value that
 * held one anyway was stored, and then failed a cast to text, COPY and a
 * restore.  The same error jsonb_in() raises is raised here.  The string is
 * converted to the database's encoding, and so validated, as jsonb_in()
 * would have it; see pljs_utf8_to_server().
 *
 * @param val #JsonbValue - the value to fill
 * @param str @c char* - the string, which the caller frees unless this raises
 * @param len @c size_t - its length
 * @param ctx #JSContext - Javascript context that owns @p str
 */
static void jsonb_string_value(JsonbValue *val, const char *str, size_t len,
                               JSContext *ctx) {
  char *server;

  if (memchr(str, '\0', len) != NULL) {
    JS_FreeCString(ctx, str);
    ereport(ERROR, (errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
                    errmsg("unsupported Unicode escape sequence"),
                    errdetail("\\u0000 cannot be converted to text.")));
  }

  server = pljs_js_string_to_server(ctx, str, len);

  if (server != str) {
    len = strlen(server);
  }

  val->type = jbvString;
  val->val.string.val = palloc(len);
  memcpy(val->val.string.val, server, len);
  val->val.string.len = len;

  if (server != str) {
    pfree(server);
  }
}

/**
 * @brief Raises for a BigInt, which JSON has no value for.
 *
 * JSON.stringify() throws for one, unless it has a toJSON(), which
 * jsonb_json_value() has already called.  It was written as a string -- 10n
 * as "10" -- where a json result raised, and every int8 a query returns is a
 * BigInt.  A data error, as jsonb's others are, and not an internal one.
 */
pg_noreturn static void jsonb_bigint_error(void) {
  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("cannot convert a BigInt to jsonb"),
                  errhint("Convert it with Number() or String(), or define "
                          "BigInt.prototype.toJSON().")));
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
                                    JsonbIteratorToken type, JSContext *ctx) {
  JsonbValue val;

  // Make the conversion based on the #JSValue type.
  if (JS_IsBool(value)) {
    val.type = jbvBool;
    val.val.boolean = JS_ToBool(ctx, value);
  } else if (JS_IsNull(value)) {
    val.type = jbvNull;
  } else if (JS_IsString(value)) {
    size_t len;
    const char *v = JS_ToCStringLen(ctx, &len, value);

    if (v == NULL) {
      pljs_ereport_js_exception(ctx);
    }

    jsonb_string_value(&val, v, len, ctx);
    JS_FreeCString(ctx, v);
  } else if (JS_IsNumber(value)) {
    double in;

    if (JS_ToFloat64(ctx, &in, value) < 0) {
      pljs_ereport_js_exception(ctx);
    }

    /*
     * NaN and the infinities are null, as JSON.stringify() writes them.  They
     * were stored as numeric NaN and Infinity, which jsonb cannot otherwise
     * hold: a cast to text and back, COPY and a restore all failed on them.
     */
    if (isfinite(in)) {
      val.val.numeric = DatumGetNumeric(
          DirectFunctionCall1(float8_numeric, Float8GetDatum((float8)in)));
      val.type = jbvNumeric;
    } else {
      val.type = jbvNull;
    }
  } else if (JS_IsBigInt(ctx, value)) {
    jsonb_bigint_error();
  } else {
    size_t len;
    const char *v = JS_ToCStringLen(ctx, &len, value);

    if (v == NULL) {
      pljs_ereport_js_exception(ctx);
    }

    jsonb_string_value(&val, v, len, ctx);
    JS_FreeCString(ctx, v);
  }

  // Push the result into the parse_state.
  return jsonb_push(pstate, type, &val);
}

/**
 * @brief Returns the key that JSON.stringify() passes to toJSON().
 *
 * @param ctx #JSContext - Javascript context to execute in
 * @param atom #JSAtom - a property's name, or JS_ATOM_NULL
 * @param index @c int32_t - an element's index, or -1
 * @returns #JSValue - the name, the index as a string, or "" for the value
 * being converted
 */
static JSValue jsonb_key(JSContext *ctx, JSAtom atom, int32_t index) {
  char digits[12];

  if (atom != JS_ATOM_NULL) {
    return JS_AtomToString(ctx, atom);
  }

  if (index < 0) {
    return JS_NewString(ctx, "");
  }

  snprintf(digits, sizeof(digits), "%d", index);

  return JS_NewString(ctx, digits);
}

/**
 * @brief Returns the primitive value of a Number, String or Boolean object.
 *
 * As JSON.stringify() writes one: `new String('x')` is "x", where it was
 * written as its properties, {"0": "x"}.  A Number is read as ToNumber() reads
 * it and a String as ToString() does, as JSON.stringify() reads them, and a
 * Boolean the same way as a Number, since QuickJS has no call to read its
 * value directly.
 *
 * @param ctx #JSContext - Javascript context to execute in
 * @param value #JSValue - an owned reference, which is released
 * @returns #JSValue - an owned reference to what to write
 */
static JSValue jsonb_unbox(JSContext *ctx, JSValue value) {
  JSClassID class_id = JS_GetClassID(value);
  JSValue primitive;

  if (class_id == JS_CLASS_NUMBER || class_id == JS_CLASS_BOOLEAN) {
    double number;

    if (JS_ToFloat64(ctx, &number, value) < 0) {
      primitive = JS_EXCEPTION;
    } else if (class_id == JS_CLASS_NUMBER) {
      primitive = JS_NewFloat64(ctx, number);
    } else {
      primitive = JS_NewBool(ctx, number != 0 && !isnan(number));
    }
  } else if (class_id == JS_CLASS_STRING) {
    primitive = JS_ToString(ctx, value);
  } else if (JS_CLASS_BIG_INT != 0 && class_id == JS_CLASS_BIG_INT) {
    /* Object(10n) is a BigInt, and was written as its properties, {}. */
    JS_FreeValue(ctx, value);
    jsonb_bigint_error();
  } else {
    return value;
  }

  JS_FreeValue(ctx, value);

  pljs_checked_value(ctx, primitive);

  return primitive;
}

/**
 * @brief Returns what JSON writes for a value.
 *
 * As JSON.stringify() finds it: an object's toJSON(), or a BigInt's, is
 * called with the value's key, and a Number, String or Boolean object is then
 * written as its primitive value.  A Date's toJSON() gives its toISOString(),
 * or null for an invalid Date; a Date whose toJSON is not a function is
 * written as an object.
 *
 * Only a Date's toJSON() was called, so any other object with one -- a class
 * that writes itself as a string, say -- was written as its own properties,
 * unlike a json column, which writes what JSON.stringify() does.  A Date used
 * to be written as its properties too, and since it has none it was stored as
 * {}.
 *
 * @param build #jsonb_build - the conversion
 * @param value #JSValueConst - the value
 * @param atom #JSAtom - the value's key in an object, or JS_ATOM_NULL
 * @param index @c int32_t - its index in an array, or -1
 * @returns #JSValue - an owned reference to what to write
 */
static JSValue jsonb_json_value(jsonb_build *build, JSValueConst value,
                                JSAtom atom, int32_t index) {
  JSContext *ctx = build->ctx;
  JSValue to_json;
  JSValue json;

  if (!JS_IsObject(value) && !JS_IsBigInt(ctx, value)) {
    return JS_DupValue(ctx, value);
  }

  to_json = JS_GetProperty(ctx, value, build->to_json);

  pljs_checked_value(ctx, to_json);

  if (JS_IsFunction(ctx, to_json)) {
    JSValue key = jsonb_key(ctx, atom, index);

    json = JS_Call(ctx, to_json, value, 1, &key);
    JS_FreeValue(ctx, key);
  } else {
    json = JS_DupValue(ctx, value);
  }

  JS_FreeValue(ctx, to_json);

  pljs_checked_value(ctx, json);

  return jsonb_unbox(ctx, json);
}

/**
 * @brief Whether JSON has no value for a JavaScript value.
 *
 * JSON.stringify() leaves out a property whose value is undefined, a function
 * or a Symbol, and writes null for such an element of an array.
 *
 * A function was converted as an object, and every function's prototype has
 * a constructor that is the function again: the recursion ran until it
 * overflowed the stack and crashed the backend.  A property whose value was
 * undefined wrote its key and no value.
 *
 * @param value #JSValueConst - the value, after jsonb_json_value()
 * @param ctx #JSContext - Javascript context to execute in
 * @returns @c bool
 */
static bool jsonb_has_no_value(JSValueConst value, JSContext *ctx) {
  return JS_IsUndefined(value) || JS_IsSymbol(value) ||
         JS_IsFunction(ctx, value);
}

/**
 * @brief Converts a value to JSONB: an array, an object or a scalar.
 *
 * @param build #jsonb_build - the conversion, which holds the value last
 * @param value #JSValueConst - the value, after jsonb_json_value()
 * @param type #JsonbIteratorToken - WJB_VALUE or WJB_ELEM, for a scalar
 * @returns #JsonbValue of the value
 */
static JsonbValue *jsonb_from_any(jsonb_build *build, JSValueConst value,
                                  JsonbIteratorToken type) {
  if (JS_IsArray(build->ctx, value)) {
    return jsonb_array_from_array(build, value);
  }

  if (JS_IsObject(value)) {
    return jsonb_object_from_object(build, value);
  }

  return jsonb_from_value(value, &build->pstate, type, build->ctx);
}

/**
 * @brief Starts converting the array or object held last, refusing a cycle.
 *
 * JSON.stringify() raises "circular reference" for an object that contains
 * itself, and jsonb recursed into it until the stack overflowed and the
 * backend crashed; so did an object nested deeply enough.
 *
 * @param build #jsonb_build - the conversion
 * @returns @c int - the array's or object's place in build->held
 */
static int jsonb_enter(jsonb_build *build) {
  int entering = build->nheld - 1;
  void *ptr = JS_VALUE_GET_PTR(build->held[entering].value);

  check_stack_depth();

  for (int i = 0; i < entering; i++) {
    if (build->held[i].entered &&
        JS_VALUE_GET_PTR(build->held[i].value) == ptr) {
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("cannot convert a circular structure to jsonb")));
    }
  }

  build->held[entering].entered = true;

  return entering;
}

/**
 * @brief Reads an element of an array or a property of an object, and holds
 * what JSON writes for it.
 *
 * @param build #jsonb_build - the conversion
 * @param holder #JSValueConst - the array or object
 * @param atom #JSAtom - the property's name, or JS_ATOM_NULL for an element
 * @param index @c int32_t - the element's index, or -1 for a property
 * @returns #JSValue - what to write, which the conversion holds last
 */
static JSValue jsonb_member_json(jsonb_build *build, JSValueConst holder,
                                 JSAtom atom, int32_t index) {
  JSContext *ctx = build->ctx;
  JSValue member = atom != JS_ATOM_NULL
                       ? JS_GetProperty(ctx, holder, atom)
                       : JS_GetPropertyUint32(ctx, holder, index);
  JSValue json;

  /*
   * A getter that threw.  Its JS_EXCEPTION went on to be stored as an empty
   * string.
   */
  pljs_checked_value(ctx, member);

  jsonb_hold(build, member);
  json = jsonb_json_value(build, member, atom, index);
  jsonb_release(build);

  jsonb_hold(build, json);

  return json;
}

/**
 * @brief Converts a #JSValue `Array` to a #JsonbValue array.
 *
 * @param build #jsonb_build - the conversion, which holds the array last
 * @param array #JSValueConst - `Array` to convert
 * @returns #JsonbValue of the `JSONB` array
 */
static JsonbValue *jsonb_array_from_array(jsonb_build *build,
                                          JSValueConst array) {
  JSContext *ctx = build->ctx;
  int32_t array_length;

  jsonb_enter(build);

  // Push the beginning of the array into the parse state.
  jsonb_push(&build->pstate, WJB_BEGIN_ARRAY, NULL);

  // Get the length of the `Array`.
  array_length = pljs_js_array_length(array, ctx);

  if (array_length < 0) {
    pljs_ereport_js_exception(ctx);
  }

  // Iterate through the `Array`.
  for (int32_t i = 0; i < array_length; i++) {
    JSValue json = jsonb_member_json(build, array, JS_ATOM_NULL, i);

    if (jsonb_has_no_value(json, ctx)) {
      jsonb_from_value(JS_NULL, &build->pstate, WJB_ELEM, ctx);
    } else {
      jsonb_from_any(build, json, WJB_ELEM);
    }

    jsonb_release(build);
  }

  // Set the value to the end of the array.
  return jsonb_push(&build->pstate, WJB_END_ARRAY, NULL);
}

/**
 * @brief Converts a #JSValue `Object` to a #JsonbValue object.
 *
 * @param build #jsonb_build - the conversion, which holds the object last
 * @param object #JSValueConst - `Object` to convert
 * @returns #JsonbValue of the `JSONB` object
 */
static JsonbValue *jsonb_object_from_object(jsonb_build *build,
                                            JSValueConst object) {
  JSContext *ctx = build->ctx;
  int held = jsonb_enter(build);
  JSPropertyEnum *tab = NULL;
  uint32_t tab_length = 0;

  // Push the beginning of the object into the parse state.
  jsonb_push(&build->pstate, WJB_BEGIN_OBJECT, NULL);

  /*
   * Get the keys of the `Object`: its own enumerable string keys, which are
   * what JSON.stringify() writes.  Without JS_GPN_ENUM_ONLY a function's
   * `prototype` was one of them, which is how converting a function never
   * ended.  The conversion holds them with the object.
   */
  if (JS_GetOwnPropertyNames(ctx, &tab, &tab_length, object,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    pljs_ereport_js_exception(ctx);
  }

  build->held[held].tab = tab;
  build->held[held].tab_length = tab_length;

  // Iterate through the `Object` keys.
  for (uint32_t object_key = 0; object_key < tab_length; object_key++) {
    JSAtom atom = tab[object_key].atom;
    JSValue json = jsonb_member_json(build, object, atom, -1);

    if (!jsonb_has_no_value(json, ctx)) {
      /*
       * The key with its length: a key holding "\u0000" was cut short at it by
       * strlen().
       */
      JSValue key_value = JS_AtomToString(ctx, atom);
      size_t key_length;
      const char *key = JS_ToCStringLen(ctx, &key_length, key_value);
      JsonbValue key_jsonb;

      JS_FreeValue(ctx, key_value);

      if (key == NULL) {
        pljs_ereport_js_exception(ctx);
      }

      jsonb_string_value(&key_jsonb, key, key_length, ctx);
      JS_FreeCString(ctx, key);

      jsonb_push(&build->pstate, WJB_KEY, &key_jsonb);
      jsonb_from_any(build, json, WJB_VALUE);
    }

    jsonb_release(build);
  }

  // Push that we are at the end of an object.
  return jsonb_push(&build->pstate, WJB_END_OBJECT, NULL);
}

/**
 * @brief Converts a #JSValue `Object` to a #Jsonb value.
 *
 * @param object #JSValue - `Object` to convert
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #Jsonb the converted `JSONB` value, or NULL if JSON has no value
 * for it
 */
static Jsonb *convert_object(JSValue object, JSContext *ctx) {
  // Create a new memory context for conversion.
  MemoryContext oldcontext = CurrentMemoryContext;
  MemoryContext conversion_context;
  jsonb_build *build;
  Jsonb *ret;

  conversion_context = AllocSetContextCreate(
      CurrentMemoryContext, "JSONB Conversion Context", ALLOCSET_SMALL_SIZES);

  MemoryContextSwitchTo(conversion_context);

  /* Not on the stack: the PG_CATCH reads what the conversion holds. */
  build = palloc0(sizeof(jsonb_build));
  build->ctx = ctx;
  build->to_json = JS_NewAtom(ctx, "toJSON");
  build->maxheld = 16;
  build->held = palloc(sizeof(jsonb_held) * build->maxheld);

  PG_TRY();
  {
    JSValue json = jsonb_json_value(build, object, JS_ATOM_NULL, -1);

    jsonb_hold(build, json);

    // Check the type and get its value.
    if (jsonb_has_no_value(json, ctx)) {
      build->result = NULL;
    } else if (JS_IsArray(ctx, json)) {
      build->result = jsonb_array_from_array(build, json);
    } else if (JS_IsObject(json)) {
      build->result = jsonb_object_from_object(build, json);
    } else {
      jsonb_push(&build->pstate, WJB_BEGIN_ARRAY, NULL);
      jsonb_from_value(json, &build->pstate, WJB_ELEM, ctx);
      build->result = jsonb_push(&build->pstate, WJB_END_ARRAY, NULL);
      build->result->val.array.rawScalar = true;
    }

    jsonb_release(build);
  }
  PG_CATCH();
  {
    /* What every level the error unwound through held. */
    while (build->nheld > 0) {
      jsonb_release(build);
    }

    JS_FreeAtom(ctx, build->to_json);

    MemoryContextSwitchTo(oldcontext);
    MemoryContextDelete(conversion_context);

    PG_RE_THROW();
  }
  PG_END_TRY();

  JS_FreeAtom(ctx, build->to_json);

  // Switch back to our old #MemoryContext.
  MemoryContextSwitchTo(oldcontext);

  // Create the #Jsonb object to return.
  ret = build->result != NULL ? JsonbValueToJsonb(build->result) : NULL;

  // Delete the conversion #MemoryContext.
  MemoryContextDelete(conversion_context);

  return ret;
}
#endif
