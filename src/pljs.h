#pragma once

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "utils/palloc.h"
#include "windowapi.h"

#include "deps/quickjs/quickjs-libc.h"
#include "deps/quickjs/quickjs.h"

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
  TypeFuncClass typeclass;
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
  TypeFuncClass typeclass;      // used for SRF
  Oid rettype;                  // the return type
  Oid argtypes[FUNC_MAX_ARGS];  // the types of the argument passed
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

// Throw a Javascript error
JSValue js_throw(const char *, JSContext *);

// Functions
JSValue pljs_compile_function(pljs_context *context, bool is_trigger);
JSValue pljs_find_js_function(Oid fn_oid, JSContext *ctx);
bool pljs_has_permission_to_execute(const char *signature);
pljs_storage *pljs_storage_for_context(JSContext *ctx);

// cache.c

// Contexts
void pljs_cache_context_add(Oid, JSContext *);
void pljs_cache_context_remove(Oid);
pljs_context_cache_value *pljs_cache_context_find(Oid user_id);

// Functions
pljs_function_cache_value *pljs_cache_function_find(Oid user_id, Oid fn_oid);
void pljs_cache_function_add(pljs_context *context);

// Serialization and Deserialization
void pljs_function_cache_to_context(pljs_context *,
                                    pljs_function_cache_value *);
void pljs_context_to_function_cache(pljs_function_cache_value *function_entry,
                                    pljs_context *context);
// Utility
void pljs_cache_reset(void);

// type.c

// To Javascript
JSValue pljs_datum_to_jsvalue(Oid type, Datum arg, JSContext *ctx,
                              bool skip_composite);
JSValue pljs_datum_to_array(pljs_type *type, Datum arg, JSContext *ctx);
JSValue pljs_datum_to_object(pljs_type *type, Datum arg, JSContext *ctx);
JSValue pljs_tuple_to_jsvalue(TupleDesc, HeapTuple, JSContext *ctx);
JSValue pljs_spi_result_to_jsvalue(int, JSContext *);

// To Postgres
Datum pljs_jsvalue_to_array(pljs_type *, JSValue, JSContext *,
                            FunctionCallInfo);
Datum pljs_jsvalue_to_datum(Oid, JSValue, JSContext *, FunctionCallInfo,
                            bool *is_null);
Datum pljs_jsvalue_to_record(pljs_type *type, JSValue val, JSContext *ctx,
                             bool *is_null, TupleDesc);
Datum *pljs_jsvalue_to_datums(pljs_type *type, JSValue val, JSContext *ctx,
                              bool **is_null, TupleDesc tupdesc);

// Utility
uint32_t pljs_js_array_length(JSValue, JSContext *);
void pljs_type_fill(pljs_type *, Oid);
bool pljs_jsvalue_object_contains_all_column_names(JSValue val, JSContext *ctx,
                                                   TupleDesc tupdesc);
JSValue pljs_values_to_array(JSValue *, int, int, JSContext *);
void pljs_variable_param_setup(ParseState *, void *);
ParamListInfo pljs_setup_variable_paramlist(pljs_param_state *, Datum *,
                                            char *);
