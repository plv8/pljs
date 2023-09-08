#pragma once

#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/params.h"
#include "parser/parse_node.h"

#include "deps/quickjs/quickjs-libc.h"
#include "deps/quickjs/quickjs.h"

#define STORAGE_HASH_LEN 32
#ifndef PLJS_VERSION
#define PLJS_VERSION "unknown"
#endif

// pljs current runtime configuration
typedef struct pljs_configuration {
  size_t memory_limit;
  char *start_proc;
  int execution_timeout;
} pljs_configuration;

extern pljs_configuration configuration;

// static functions in pljs.c
static Datum call_function(PG_FUNCTION_ARGS, JSContext *, JSValue, int, Oid *,
                           Oid);

JSValue pljs_compile_function(JSContext *, char *, const char *, int, char *[]);
void pljs_call_anonymous_function(JSContext *, const char *);
JSValue js_throw(JSContext *, const char *);

// other functions
void _PG_init(void);
void pljs_guc_init(void);

// quickjs runtime
extern JSRuntime *rt;

// function cache key
typedef struct pljs_function_cache_key {
  char proname[NAMEDATALEN];
  bool trigger;
  Oid user_id;
  Oid rettype;
  int nargs;
  Oid argtypes[FUNC_MAX_ARGS];
} pljs_function_cache_key;

// context_cache_key
typedef struct pljs_context_cache_key {
  Oid user_id;
} pljs_context_cache_key;

// cache value definition
typedef struct pljs_cache_value {
  JSValue fn;
  JSContext *ctx;
  void *key;
} pljs_cache_value;

// initialize
void pljs_cache_init(void);

// add
void pljs_cache_context_add(Oid, JSContext *);
void pljs_cache_function_add(Oid, JSContext *, JSValue);

// remove
void pljs_cache_context_remove(Oid);
void pljs_cache_function_remove(Oid);

// search
pljs_cache_value *pljs_cache_context_find(Oid);
pljs_cache_value *pljs_cache_function_find(Oid);

typedef struct pljs_param_state {
  Oid *param_types;
  int nparams;
  MemoryContext memory_context;
} pljs_param_state;

typedef struct pljs_type {
  Oid typid;
  Oid ioparam;
  int16 len;
  bool byval;
  char align;
  char category;
  bool is_composite;
  FmgrInfo fn_input;
  FmgrInfo fn_output;
} pljs_type;

uint32_t js_array_length(JSContext *, JSValue);

void pljs_type_fill(pljs_type *, Oid);

void pljs_setup_namespace(JSContext *);

// plan for prepared statements
typedef struct pljs_plan {
  SPIPlanPtr plan;
  pljs_param_state *parstate;
} pljs_plan;

JSValue pljs_datum_to_jsvalue(Datum arg, Oid type, JSContext *ctx);
JSValue pljs_datum_to_array(Datum arg, pljs_type *type, JSContext *ctx);
JSValue pljs_datum_to_object(Datum arg, pljs_type *type, JSContext *ctx);

Datum pljs_jsvalue_to_array(JSValue, pljs_type *, JSContext *,
                            FunctionCallInfo);
Datum pljs_jsvalue_to_datum(JSValue, Oid, JSContext *, FunctionCallInfo,
                            bool *);
Datum pljs_jsvalue_to_record(JSValue val, pljs_type *type, JSContext *ctx,
                             bool *is_null);
JSValue values_to_array(JSContext *, JSValue *, int, int);
JSValue tuple_to_jsvalue(JSContext *ctx, TupleDesc, HeapTuple);
JSValue spi_result_to_jsvalue(JSContext *, int);

void pljs_variable_param_setup(ParseState *, void *);
ParamListInfo pljs_setup_variable_paramlist(pljs_param_state *, Datum *,
                                            char *);
