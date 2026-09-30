#pragma once

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/tupconvert.h"
#include "access/tupdesc.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"
#include "windowapi.h"

#include "deps/quickjs/quickjs-libc.h"
#include "deps/quickjs/quickjs.h"

/*
 * PostgreSQL 18 provides pg_noreturn (C11 _Noreturn); earlier versions provide
 * only pg_attribute_noreturn().  GNU attributes may also precede the
 * declaration specifiers, so one spelling works on every supported version.
 */
#ifndef pg_noreturn
#define pg_noreturn pg_attribute_noreturn()
#endif

#define STORAGE_HASH_LEN 32
#ifndef PLJS_VERSION
#define PLJS_VERSION "unknown"
#endif

// pljs current runtime configuration.
typedef struct pljs_configuration {
  size_t memory_limit;
  char *start_proc;
  int execution_timeout;
} pljs_configuration;

// Global #pljs_configuration configuration.
extern pljs_configuration configuration;

// quickjs runtime.
extern JSRuntime *rt;

// Cpntext cache value definition.
typedef struct pljs_context_cache_value {
  Oid user_id;
  JSContext *ctx;
  MemoryContext function_memory_context;
  HTAB *function_hash_table;
} pljs_context_cache_value;

// Function cache value defition.
typedef struct pljs_function_cache_value {
  Oid fn_oid;
  JSValue fn;
  JSContext *ctx;
  bool trigger;
  Oid user_id;
  int nargs;
  bool is_srf;
  char proname[NAMEDATALEN];
  Oid argtypes[FUNC_MAX_ARGS];
  char argmodes[FUNC_MAX_ARGS];
  char *prosrc;

  /*
   * Identity of the pg_proc tuple this entry was compiled from, so a stale
   * entry can be recognised.  Same mechanism plpgsql uses (see
   * plpgsql_compile()).
   */
  TransactionId fn_xmin;
  ItemPointerData fn_tid;
} pljs_function_cache_value;

typedef struct pljs_param_state {
  Oid *param_types;
  int nparams;
  MemoryContext memory_context;
} pljs_param_state;

typedef struct pljs_return_state {
  Tuplestorestate *tuple_store_state;
  TupleDesc tuple_desc;
  Oid rettype;
  bool is_composite;
  bool is_domain; // rettype is a domain over the composite type of the rows
  bool convert_in_subtransaction; // converting a row can run a domain's checks
  bool convert_known;        // convert_in_subtransaction has been worked out
  uint64 convert_generation; // pljs_type_domain_generation() it was worked at
  MemoryContext row_context; // a domain's row is converted in; see
                             // pljs_put_domain_row()
  bool row_context_busy;     // a row is being converted in it
  bool domain_map_known;     // domain_map has been worked out, for:
  uint64 domain_rowtype_id;  // the typcache's identifier of the row type
  MemoryContext domain_context;   // holding:
  TupleDesc domain_rowtype;       // a copy of that row type
  TupleConversionMap *domain_map; // a row of the set to it, or NULL if the
                                  // two match
  Oid fn_oid;                     // the function returning the set
  bool column_known;         // column_name and column_atom have been worked out
  char *column_name;         // a single-column set's column's name, or NULL
  JSAtom column_atom;        // its key; see pljs_single_column_value()
  bool column_type_known;    // column_takes_objects has been worked out
  bool column_takes_objects; // its type takes an object as its value
} pljs_return_state;

// Expanded type definitions for pljs.
typedef struct pljs_type {
  Oid typid;
  Oid ioparam;
  int16 length;
  bool byval;
  char align;
  char category;
  bool is_composite;
} pljs_type;

// Plan for prepared statements.
typedef struct pljs_plan {
  SPIPlanPtr plan;
  pljs_param_state *parstate;
  int32 *param_typmods; // the declared parameters' typmods, or NULL if none
} pljs_plan;

// Context and information for the function to be called.
typedef struct pljs_func {
  Oid fn_oid; // function's OID

  char proname[NAMEDATALEN]; // the function name
  char *prosrc;              // a copy of its source

  TransactionId fn_xmin;
  ItemPointerData fn_tid;
  Oid user_id; // the user id

  bool trigger;
  bool is_srf;                  // are we a set returning function?
  int inargs;                   // the number of input arguments
  int nargs;                    // the total number of arguments
  Oid rettype;                  // the return type
  Oid argtypes[FUNC_MAX_ARGS];  // the input arguments' types, by their place
                                // in the call
  char argmodes[FUNC_MAX_ARGS]; // mode of each argument
} pljs_func;

typedef struct pljs_context {
  JSContext *ctx;
  JSValue js_function; // the function itself

  char *arguments[FUNC_MAX_ARGS];
  MemoryContext memory_context;
  pljs_func *function;
} pljs_context;

typedef struct pljs_storage {
  pljs_return_state *return_state;
  pljs_func *function;
  FunctionCallInfo fcinfo;
  WindowObject window_object;
  MemoryContext execution_memory_context;
  ErrorData *fatal_error; // ends the call; see pljs_throw_fatal_error()
} pljs_storage;

typedef struct pljs_window_storage {
  size_t max_length; // allocated memory
  size_t length;     // the byte size of data
  char data[1];      // actual string (without null-termination
} pljs_window_storage;

extern JSClassID js_prepared_statement_handle_id;
extern JSClassID js_cursor_handle_id;
extern JSClassID js_pljs_storage_id;
extern JSClassID js_window_id;

// pljs.c

// Language call handlers
Datum pljs_call_handler(PG_FUNCTION_ARGS);
Datum pljs_call_validator(PG_FUNCTION_ARGS);
Datum pljs_inline_handler(PG_FUNCTION_ARGS);

// Extension initialization
void _PG_init(void);
void pljs_guc_init(void);
void pljs_cache_init(void);
void pljs_setup_namespace(JSContext *ctx);
// Registers runtime JS classes (e.g. the prepared-statement handle whose GC
// finalizer reclaims the SPI plan).  Must run once, after the runtime exists
// and before any JSContext is created.
void pljs_register_js_classes(JSRuntime *rt);

// Throw a Javascript error
JSValue js_throw(const char *, JSContext *);
// Throw a Javascript error carrying a Postgres ErrorData's detail/hint/sqlstate
JSValue js_throw_error_data(ErrorData *, JSContext *);
// End the running call with the Postgres error being handled
JSValue pljs_throw_fatal_error(JSContext *);
JSValue pljs_throw_fatal_error_data(ErrorData *, JSContext *);
// Raise the pending Javascript exception as a Postgres error
pg_noreturn void pljs_ereport_js_exception(JSContext *);
// Whether the running call has to end with an error, and throw that error
bool pljs_call_is_ending(void);
JSValue pljs_throw_ending(JSContext *);

/*
 * An internal subtransaction a builtin runs its work in; see
 * pljs_subxact_begin().  Set up with pljs_subxact_init() before the PG_TRY
 * that begins it.
 */
typedef struct pljs_subxact {
  MemoryContext mcontext; // the builtin's, gone back to however it ends
  ResourceOwner resowner; // likewise
  /*
   * Begun, and not yet committed or rolled back.  The only member a PG_TRY
   * changes, and volatile itself, as PostgreSQL requires of what the PG_CATCH
   * reads; the others are set before it.
   */
  volatile bool begun;
} pljs_subxact;

void pljs_subxact_init(pljs_subxact *sx);
void pljs_subxact_begin(pljs_subxact *sx, bool optional);
void pljs_subxact_rollback(pljs_subxact *sx);
ErrorData *pljs_subxact_abort(pljs_subxact *sx);
ErrorData *pljs_subxact_commit(pljs_subxact *sx);

// Functions
JSValue pljs_compile_function(pljs_context *context, bool is_trigger);
JSValue pljs_find_js_function(Oid fn_oid, JSContext *ctx);
JSValue pljs_single_column_value(JSContext *ctx, JSValueConst row,
                                 pljs_return_state *state, const char *caller);
void pljs_put_domain_row(pljs_return_state *state, JSValueConst row,
                         JSContext *ctx, const char *caller);
bool pljs_has_permission_to_execute(const char *signature);
pljs_storage *pljs_current_storage(void);

// Whether a function's result is being converted, anywhere up the stack
extern bool pljs_converting_result;

// cache.c

// Contexts
void pljs_cache_context_add(Oid, JSContext *);
void pljs_cache_context_remove(Oid);
pljs_context_cache_value *pljs_cache_context_find(Oid user_id);

// Functions
pljs_function_cache_value *pljs_cache_function_find(Oid user_id, Oid fn_oid,
                                                    HeapTuple proctuple);
void pljs_cache_function_add(pljs_context *context);
void pljs_cache_function_remove(Oid fn_oid);

// Serialization and Deserialization
void pljs_function_cache_to_context(pljs_context *,
                                    pljs_function_cache_value *);
void pljs_context_to_function_cache(pljs_function_cache_value *function_entry,
                                    pljs_context *context);
// Utility
void pljs_cache_reset(void);

// type.c

// To Javascript
JSValue pljs_datum_to_jsvalue(Oid argtype, Datum arg, bool is_null,
                              bool expand_composite, JSContext *ctx);
JSValue pljs_datum_to_array(pljs_type *type, Datum arg, JSContext *ctx);
JSValue pljs_datum_to_object(pljs_type *type, Datum arg, JSContext *ctx);
JSValue pljs_tuple_to_jsvalue(TupleDesc, HeapTuple, JSContext *ctx);
JSValue pljs_spi_result_to_jsvalue(int, JSContext *);

// To Postgres
Datum pljs_jsvalue_to_array(pljs_type *, JSValue, JSContext *, int32);
Datum pljs_jsvalue_to_datum(Oid rettype, JSValue val, bool *is_null,
                            JSContext *ctx, FunctionCallInfo fcinfo);
Datum pljs_jsvalue_to_datum_free(Oid rettype, JSValue val, bool *is_null,
                                 JSContext *ctx);
Datum pljs_jsvalue_to_datum_typmod_free(Oid typid, int32 typmod, JSValue val,
                                        bool *is_null, JSContext *ctx);
Datum pljs_jsvalue_to_record(pljs_type *type, JSValue val, bool *is_null,
                             TupleDesc tupdesc, JSContext *ctx);
Datum *pljs_jsvalue_to_datums(pljs_type *type, JSValue val, bool **is_null,
                              TupleDesc tupdesc, JSContext *ctx,
                              const char *caller);

// Utility
int32_t pljs_js_array_length(JSValue, JSContext *);
void pljs_type_fill(pljs_type *, Oid);
Oid pljs_type_base(Oid);
bool pljs_type_may_check_domain(Oid);
void pljs_type_io_init(void);
uint64 pljs_type_domain_generation(void);
/* A watch for domain checks; see pljs_type_domain_watch_start(). */
typedef struct pljs_domain_watch {
  volatile int level; // volatile: a PG_TRY sets it, and a PG_CATCH reads it
  volatile uint64 hits;
} pljs_domain_watch;

void pljs_type_domain_watch_start(pljs_domain_watch *saved);
bool pljs_type_domain_watch_end(pljs_domain_watch *saved);
void pljs_type_classes_init(JSContext *ctx);

// Type conversion state for the pljs function being called
typedef struct pljs_type_io_cache pljs_type_io_cache;
pljs_type_io_cache *pljs_type_io_enter(FmgrInfo *);
void pljs_type_io_exit(pljs_type_io_cache *);
bool pljs_jsvalue_is_plain_object(JSValueConst obj);
bool pljs_jsvalue_is_proxy(JSValueConst obj);
void pljs_type_domain_check(Oid typid, Datum value, bool isnull);
void pljs_free_prop_enum(JSContext *ctx, JSPropertyEnum *tab, uint32_t len);
char *pljs_server_to_utf8(const char *str, size_t len);
JSAtom pljs_column_atom(JSContext *ctx, Form_pg_attribute attr);
JSAtom pljs_name_atom(JSContext *ctx, const char *name);
JSValue pljs_row_get_column(JSContext *ctx, JSValueConst row, JSAtom atom,
                            bool *found);
int pljs_converts_itself(JSContext *ctx, JSValueConst obj);
bool pljs_type_is_json(Oid typid);
bool pljs_column_refuses(JSContext *ctx, JSValueConst value, Oid typid);
pg_noreturn void pljs_function_column_error(const char *name,
                                            const char *caller);
JSValue pljs_new_server_string(JSContext *ctx, const char *str, size_t len);
JSValue pljs_new_message_string(JSContext *ctx, const char *str, size_t len);
char *pljs_utf8_to_server(const char *str, size_t len);
char *pljs_utf8_to_server_lossy(const char *str, size_t len);
void pljs_encoding_init(void);
JSValue pljs_values_to_array(JSValue *, int, int, JSContext *);
void pljs_variable_param_setup(ParseState *, void *);
ParamListInfo pljs_setup_variable_paramlist(pljs_param_state *, Datum *,
                                            char *);
