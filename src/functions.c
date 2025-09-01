#include "deps/quickjs/list.h"
#include "deps/quickjs/quickjs.h"

#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/params.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/fmgrprotos.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "windowapi.h"

#include "pljs.h"

// Local only functions for injecting into pljs
static JSValue pljs_elog(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_execute(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_prepare(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_plan_execute(JSContext *, JSValueConst, int,
                                 JSValueConst *);
static int pljs_execute_params(const char *, JSValue, JSContext *);
static JSValue pljs_plan_execute(JSContext *, JSValueConst, int,
                                 JSValueConst *);
static JSValue pljs_plan_cursor(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_plan_cursor_fetch(JSContext *, JSValueConst, int,
                                      JSValueConst *);
static JSValue pljs_plan_cursor_move(JSContext *, JSValueConst, int,
                                     JSValueConst *);
static JSValue pljs_plan_cursor_close(JSContext *, JSValueConst, int,
                                      JSValueConst *);
static JSValue pljs_plan_cursor_to_string(JSContext *, JSValueConst, int,
                                          JSValueConst *);

static JSValue pljs_plan_free(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_plan_to_string(JSContext *, JSValueConst, int,
                                   JSValueConst *);
static JSValue pljs_commit(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_rollback(JSContext *, JSValueConst, int, JSValueConst *);

static JSValue pljs_find_function(JSContext *, JSValueConst, int,
                                  JSValueConst *);
static JSValue pljs_return_next(JSContext *, JSValueConst, int, JSValueConst *);

static JSValue pljs_get_window_object(JSContext *, JSValueConst, int,
                                      JSValueConst *);
static JSValue pljs_window_get_partition_local(JSContext *, JSValueConst, int,
                                               JSValueConst *);
static JSValue pljs_window_set_partition_local(JSContext *, JSValueConst, int,
                                               JSValueConst *);
static JSValue pljs_window_get_current_position(JSContext *, JSValueConst, int,
                                                JSValueConst *);
static JSValue pljs_window_get_partition_row_count(JSContext *, JSValueConst,
                                                   int, JSValueConst *);
static JSValue pljs_window_set_mark_position(JSContext *, JSValueConst, int,
                                             JSValueConst *);
static JSValue pljs_window_rows_are_peers(JSContext *, JSValueConst, int,
                                          JSValueConst *);
static JSValue pljs_window_get_func_arg_in_partition(JSContext *, JSValueConst,
                                                     int, JSValueConst *);
static JSValue pljs_window_get_func_arg_in_frame(JSContext *, JSValueConst, int,
                                                 JSValueConst *);
static JSValue pljs_window_get_func_arg_current(JSContext *, JSValueConst, int,
                                                JSValueConst *);
static JSValue pljs_window_object_to_string(JSContext *, JSValueConst, int,
                                            JSValueConst *);
static JSValue pljs_subtransaction(JSContext *, JSValueConst, int,
                                   JSValueConst *);

#ifdef EXPOSE_GC
static JSValue pljs_gc(JSContext *, JSValueConst, int, JSValueConst *);
#endif

static JSValue pljs_import(JSContext *, JSValueConst, int, JSValueConst *);

// Set up any stored procedures we export to Postgres.
PGDLLEXPORT Datum pljs_version(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pljs_info(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pljs_reset(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pljs_version);
PG_FUNCTION_INFO_V1(pljs_info);
PG_FUNCTION_INFO_V1(pljs_reset);

/**
 * @brief toString Javascript method for the pljs object.
 *
 * @returns #JSValue containing the string "[object pljs]"
 */
static JSValue pljs_object_to_string(JSContext *ctx, JSValueConst this_obj,
                                     int argc, JSValueConst *argv) {
  return JS_NewString(ctx, "[object pljs]");
}

/**
 * @brief Sets up the `pljs` object.
 *
 * Creates a global object named `pljs` that contains all of the helper
 * functions, such as query access and windowing, along with logging to
 * Postgres and utility functions.
 *
 * @param ctx #JSContext Javascript context
 */
void pljs_setup_namespace(JSContext *ctx) {
  // Get a copy of the global object.
  JSValue global_obj = JS_GetGlobalObject(ctx);

  // Set up the pljs namespace and functions.
  JSValue pljs = JS_NewObjectClass(ctx, js_pljs_storage_id);

  JS_SetPropertyStr(ctx, pljs, "toString",
                    JS_NewCFunction(ctx, pljs_object_to_string, "toString", 0));

  // Logging.
  JS_SetPropertyStr(ctx, pljs, "elog",
                    JS_NewCFunction(ctx, pljs_elog, "elog", 2));

  // Query access.
  JS_SetPropertyStr(ctx, pljs, "execute",
                    JS_NewCFunction(ctx, pljs_execute, "execute", 2));

  JS_SetPropertyStr(ctx, pljs, "prepare",
                    JS_NewCFunction(ctx, pljs_prepare, "prepare", 2));

  // Transactions.
  JS_SetPropertyStr(ctx, pljs, "commit",
                    JS_NewCFunction(ctx, pljs_commit, "commit", 0));

  JS_SetPropertyStr(ctx, pljs, "rollback",
                    JS_NewCFunction(ctx, pljs_rollback, "rollback", 0));

  JS_SetPropertyStr(
      ctx, pljs, "find_function",
      JS_NewCFunction(ctx, pljs_find_function, "find_function", 1));

  JS_SetPropertyStr(ctx, pljs, "return_next",
                    JS_NewCFunction(ctx, pljs_return_next, "return_next", 0));

  JS_SetPropertyStr(
      ctx, pljs, "get_window_object",
      JS_NewCFunction(ctx, pljs_get_window_object, "get_window_object", 0));

  JS_SetPropertyStr(
      ctx, pljs, "subtransaction",
      JS_NewCFunction(ctx, pljs_subtransaction, "subtransaction", 0));

#ifdef EXPOSE_GC
  JS_SetPropertyStr(ctx, pljs, "gc", JS_NewCFunction(ctx, pljs_gc, "gc", 0));
#endif

  // Version.
  JS_SetPropertyStr(ctx, pljs, "version", JS_NewString(ctx, PLJS_VERSION));

  JS_SetPropertyStr(ctx, pljs, "import",
                    JS_NewCFunction(ctx, pljs_import, "import", 1));

  JS_SetPropertyStr(ctx, global_obj, "pljs", pljs);

  // Set up logging levels in the context.
  JS_SetPropertyStr(ctx, global_obj, "DEBUG5", JS_NewInt32(ctx, DEBUG5));
  JS_SetPropertyStr(ctx, global_obj, "DEBUG4", JS_NewInt32(ctx, DEBUG4));
  JS_SetPropertyStr(ctx, global_obj, "DEBUG3", JS_NewInt32(ctx, DEBUG3));
  JS_SetPropertyStr(ctx, global_obj, "DEBUG2", JS_NewInt32(ctx, DEBUG2));
  JS_SetPropertyStr(ctx, global_obj, "DEBUG1", JS_NewInt32(ctx, DEBUG1));
  JS_SetPropertyStr(ctx, global_obj, "LOG", JS_NewInt32(ctx, LOG));
  JS_SetPropertyStr(ctx, global_obj, "INFO", JS_NewInt32(ctx, INFO));
  JS_SetPropertyStr(ctx, global_obj, "NOTICE", JS_NewInt32(ctx, NOTICE));
  JS_SetPropertyStr(ctx, global_obj, "WARNING", JS_NewInt32(ctx, WARNING));
  JS_SetPropertyStr(ctx, global_obj, "ERROR", JS_NewInt32(ctx, ERROR));
}

/**
 * @brief Javascript function `pljs.elog`.
 *
 * Javascript function that can be called from the interpreter for logging
 * purposes.
 *
 * @returns #JSValue containing `undefined`
 */
static JSValue pljs_elog(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  if (argc) {
    int32_t level;

    JS_ToInt32(ctx, &level, argv[0]);

    switch (level) {
    case DEBUG5:
    case DEBUG4:
    case DEBUG3:
    case DEBUG2:
    case DEBUG1:
    case LOG:
    case INFO:
    case NOTICE:
    case WARNING:
    case ERROR:
      break;
    default:
      return js_throw("invalid error level", ctx);
    }

    StringInfoData msg;
    initStringInfo(&msg);

    for (int i = 1; i < argc; i++) {
      if (i > 1) {
        appendStringInfo(&msg, " ");
      }

      JSValue str = JS_ToString(ctx, argv[i]);
      const char *cstr = JS_ToCString(ctx, str);

      appendStringInfo(&msg, "%s", cstr);

      JS_FreeCString(ctx, cstr);
      JS_FreeValue(ctx, str);
    }

    const char *full_message = msg.data;
    MemoryContext m_mcontext = CurrentMemoryContext;

    /* ERROR case. */
    PG_TRY();
    {
      elog(level, "%s", full_message);
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(m_mcontext);
      ErrorData *edata = CopyErrorData();
      JSValue error = js_throw(edata->message, ctx);
      FlushErrorState();
      FreeErrorData(edata);

      return error;
    }
    PG_END_TRY();
  }

  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `pljs.execute`.
 *
 * Javascript function that executes a Postgres query and returns
 * the results of that query.
 *
 * @returns #JSValue containing result of the query
 */
static JSValue pljs_execute(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  int status;
  const char *sql;
  JSValue params = {0};
  int nparam;
  ResourceOwner m_resowner;
  MemoryContext m_mcontext;
  bool cleanup_params = false;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  sql = JS_ToCString(ctx, argv[0]);

  if (argc >= 2) {
    if (JS_IsArray(ctx, argv[1])) {
      params = argv[1];
    } else {
      /* Consume trailing elements as an array. */
      params = pljs_values_to_array(argv, argc, 1, ctx);
      cleanup_params = true;
    }
  }

  nparam = pljs_js_array_length(params, ctx);
  m_resowner = CurrentResourceOwner;
  m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    if (!IsTransactionOrTransactionBlock()) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("transaction lock failure")));
    }

    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(m_mcontext);

    if (nparam == 0) {
      status = SPI_exec(sql, 0);
    } else {
      status = pljs_execute_params(sql, params, ctx);
    }
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(m_mcontext);

    ErrorData *edata = CopyErrorData();
    JSValue error = js_throw(edata->message, ctx);

    RollbackAndReleaseCurrentSubTransaction();
    MemoryContextSwitchTo(m_mcontext);
    CurrentResourceOwner = m_resowner;

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    JS_FreeCString(ctx, sql);

    return error;
  }
  PG_END_TRY();

  ReleaseCurrentSubTransaction();

  JS_FreeCString(ctx, sql);

  // If we allocated params, then we need to free it.
  if (cleanup_params) {
    JS_FreeValue(ctx, params);
  }

  MemoryContextSwitchTo(m_mcontext);
  CurrentResourceOwner = m_resowner;

  JSValue ret = pljs_spi_result_to_jsvalue(status, ctx);

  return ret;
}

/**
 * @brief Executes a query with parameters and returns the status.
 *
 * Accepts a query and parameters and executes the query via SPI.
 *
 * @param sql @c string containing the sql to execute
 * @param params #JSValue array of parameters to execute
 * @param ctx #JSContext
 * @returns #JSValue containing result of the query
 */
static int pljs_execute_params(const char *sql, JSValue params,
                               JSContext *ctx) {
  int nparams = pljs_js_array_length(params, ctx);
  int status;
  Datum *values = palloc(sizeof(Datum) * nparams);
  char *nulls = palloc(sizeof(char) * nparams);

  SPIPlanPtr plan;
  pljs_param_state parstate = {.memory_context = CurrentMemoryContext,
                               .param_types = 0};
  ParamListInfo param_li;

  plan = SPI_prepare_params(sql, pljs_variable_param_setup, &parstate, 0);

  if (parstate.nparams != nparams) {
    elog(ERROR, "parameter count mismatch: %d != %d", parstate.nparams,
         nparams);
  }
  for (int i = 0; i < nparams; i++) {
    JSValue param = JS_GetPropertyUint32(ctx, params, i);
    bool is_null;

    values[i] = pljs_jsvalue_to_datum(parstate.param_types[i], param, &is_null,
                                      ctx, NULL);

    JS_FreeValue(ctx, param);
  }

  param_li = pljs_setup_variable_paramlist(&parstate, values, nulls);
  status = SPI_execute_plan_with_paramlist(plan, param_li, false, 0);

  pfree(values);
  pfree(nulls);

  return status;
}

/**
 * @brief Javascript function `plan.execute`.
 *
 * Javascript function that executes a Postgres plan and returns
 * the results of that query.
 *
 * @returns #JSValue containing result of the query
 */
static JSValue pljs_plan_execute(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  pljs_plan *plan = NULL;
  JSValue params = {0};
  Datum *values = NULL;
  char *nulls = NULL;
  int nparams = 0;
  int argcount;
  MemoryContext m_mcontext;
  ResourceOwner m_resowner;
  int status;
  bool cleanup_params = false;

  if (argc) {
    if (JS_IsArray(ctx, argv[0])) {
      params = argv[0];
    } else {
      /* Consume trailing elements as an array. */
      params = pljs_values_to_array(argv, argc, 0, ctx);
      cleanup_params = true;
    }
  }

  nparams = pljs_js_array_length(params, ctx);

  JSValue ptr = JS_GetPropertyStr(ctx, this_val, "plan");

  plan = JS_GetOpaque(ptr, js_prepared_statement_handle_id);

  JS_FreeValue(ctx, ptr);

  if (plan == NULL) {
    return js_throw("Invalid plan", ctx);
  }

  if (plan->parstate) {
    argcount = plan->parstate->nparams;
  } else {
    argcount = SPI_getargcount(plan->plan);
  }

  if (argcount != nparams) {
    elog(ERROR, "plan expected %d arguments but %d were passed instead",
         argcount, nparams);
  }

  if (nparams > 0) {
    values = palloc(sizeof(Datum) * nparams);
    nulls = palloc((sizeof(char) * nparams));
  }

  for (int i = 0; i < nparams; i++) {
    JSValue param = JS_GetPropertyUint32(ctx, params, i);
    bool is_null;

    values[i] = pljs_jsvalue_to_datum(
        plan->parstate ? plan->parstate->param_types[i] : 0, param, &is_null,
        ctx, NULL);

    JS_FreeValue(ctx, param);
  }

  m_resowner = CurrentResourceOwner;
  m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    if (!IsTransactionOrTransactionBlock()) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("transaction lock failure")));
    }

    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(m_mcontext);

    if (plan->parstate) {
      ParamListInfo paramLI;

      paramLI = pljs_setup_variable_paramlist(plan->parstate, values, nulls);
      status = SPI_execute_plan_with_paramlist(plan->plan, paramLI, false, 0);

    } else {
      status = SPI_execute_plan(plan->plan, values, nulls, false, 0);
    }
  }

  PG_CATCH();
  {
    MemoryContextSwitchTo(m_mcontext);
    ErrorData *edata = CopyErrorData();
    JSValue error = js_throw(edata->message, ctx);

    RollbackAndReleaseCurrentSubTransaction();
    CurrentResourceOwner = m_resowner;

    if (values) {
      pfree(values);
    }

    if (nulls) {
      pfree(nulls);
    }

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return error;
  }

  PG_END_TRY();

  ReleaseCurrentSubTransaction();

  MemoryContextSwitchTo(m_mcontext);
  CurrentResourceOwner = m_resowner;

  JSValue ret = pljs_spi_result_to_jsvalue(status, ctx);
  SPI_freetuptable(SPI_tuptable);

  if (values) {
    pfree(values);
  }

  if (nulls) {
    pfree(nulls);
  }

  if (cleanup_params) {
    JS_FreeValue(ctx, params);
  }

  return ret;
}

/**
 * @brief Javascript function `plan.free`.
 *
 * Javascript function that frees a plan and sets it to the JSValue null.
 * the results of that query.
 *
 * @returns 0 for historic plv8 compatibility
 */
static JSValue pljs_plan_free(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  pljs_plan *plan;
  JSValue ptr = JS_GetPropertyStr(ctx, this_val, "plan");

  plan = JS_GetOpaque(ptr, js_prepared_statement_handle_id);

  if (plan) {
    if (plan->plan) {
      SPI_freeplan(plan->plan);
    }

    if (plan->parstate) {
      pfree(plan->parstate);
    }

    pfree(plan);
  }

  JS_SetPropertyStr(ctx, this_val, "plan", JS_NULL);

  JS_FreeValue(ctx, ptr);

  return JS_NewInt32(ctx, 0);
}

static const JSCFunctionListEntry js_plan_funcs[] = {
    JS_CFUNC_DEF("execute", 2, pljs_plan_execute),
    JS_CFUNC_DEF("free", 0, pljs_plan_free),
    JS_CFUNC_DEF("cursor", 0, pljs_plan_cursor),
    JS_CFUNC_DEF("toString", 0, pljs_plan_to_string)};

/**
 * @brief Javascript function `pljs.prepare`.
 *
 * Javascript function that prepares a plan from sql
 * and returns a `plan` object with the functions of
 * `execute`, `free`, `cursor`, and `toString`.
 *
 * @returns #JSValue containing a plan
 */
static JSValue pljs_prepare(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  const char *sql;
  JSValue params = {0};
  int nparams;
  Oid *types = NULL;
  SPIPlanPtr initial = NULL, saved = NULL;
  pljs_param_state *parstate = NULL;
  pljs_plan *plan = NULL;
  bool cleanup_params = false;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  if (argc >= 2) {
    if (JS_IsArray(ctx, argv[1])) {
      params = argv[1];
    } else {
      /* Consume trailing elements as an array. */
      params = pljs_values_to_array(argv, argc, 1, ctx);
      cleanup_params = true;
    }
  }

  nparams = pljs_js_array_length(params, ctx);

  if (nparams) {
    types = palloc(sizeof(Oid) * nparams);
  }

  for (int i = 0; i < nparams; i++) {
    JSValue param = JS_GetPropertyUint32(ctx, params, i);
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, param);
    int32 typemod;

    parseTypeString(str, &types[i], &typemod, false);

    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, param);
  }

  sql = JS_ToCString(ctx, argv[0]);

  PG_TRY();
  {
    if (argc > 1) {
      parstate = palloc0(sizeof(pljs_param_state));
      parstate->memory_context = CurrentMemoryContext;
      initial = SPI_prepare_params(sql, pljs_variable_param_setup, parstate, 0);
    } else {
      initial = SPI_prepare(sql, nparams, types);
    }

    saved = SPI_saveplan(initial);
    SPI_freeplan(initial);
  }

  PG_CATCH();
  {
    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    JS_FreeCString(ctx, sql);
    return js_throw("Unable to prepare parameters", ctx);
  }

  PG_END_TRY();

  JS_FreeCString(ctx, sql);

  if (types != NULL) {
    pfree(types);
  }

  JSValue ret = JS_NewObject(ctx);

  JS_SetPropertyFunctionList(ctx, ret, js_plan_funcs, 4);

  plan = palloc(sizeof(pljs_plan));

  plan->parstate = parstate;
  plan->plan = saved;

  JSValue handle = JS_NewObjectClass(ctx, js_prepared_statement_handle_id);
  JS_SetOpaque(handle, plan);
  JS_SetPropertyStr(ctx, ret, "plan", handle);

  if (cleanup_params) {
    JS_FreeValue(ctx, params);
  }

  return ret;
}

static const JSCFunctionListEntry js_cursor_funcs[] = {
    JS_CFUNC_DEF("fetch", 2, pljs_plan_cursor_fetch),
    JS_CFUNC_DEF("move", 0, pljs_plan_cursor_move),
    JS_CFUNC_DEF("close", 0, pljs_plan_cursor_close),
    JS_CFUNC_DEF("toString", 0, pljs_plan_cursor_to_string)};

/**
 * @brief Javascript function `plan.cursor`.
 *
 * Javascript function that provides access to a plan's cursor.
 *
 * @returns #JSValue containing result of the query
 */
static JSValue pljs_plan_cursor(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  pljs_plan *plan;
  JSValue params = {0};
  Datum *values = NULL;
  char *nulls = NULL;
  int nparams = 0;
  int argcount;
  Portal cursor;
  bool cleanup_params = false;

  JSValue ptr = JS_GetPropertyStr(ctx, this_val, "plan");

  plan = JS_GetOpaque(ptr, js_prepared_statement_handle_id);

  JS_FreeValue(ctx, ptr);

  if (plan == NULL || plan->plan == NULL) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(&buf, "plan unexpectedly null");
    ereport(ERROR, errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("%s", buf.data));

    return JS_UNDEFINED;
  }

  if (argc) {
    if (JS_IsArray(ctx, argv[0])) {
      params = argv[0];
    } else {
      /* Consume trailing elements as an array. */
      params = pljs_values_to_array(argv, argc, 0, ctx);
      cleanup_params = true;
    }
  }

  nparams = pljs_js_array_length(params, ctx);

  if (plan->parstate) {
    argcount = plan->parstate->nparams;
  } else {
    argcount = SPI_getargcount(plan->plan);
  }

  if (argcount != nparams) {
    elog(ERROR, "plan expected %d arguments but %d were passed instead",
         argcount, nparams);
  }

  if (nparams > 0) {
    values = palloc(sizeof(Datum) * nparams);
    nulls = palloc((sizeof(char) * nparams));
  }

  for (int i = 0; i < nparams; i++) {
    JSValue param = JS_GetPropertyUint32(ctx, params, i);
    bool is_null;

    values[i] = pljs_jsvalue_to_datum(
        plan->parstate ? plan->parstate->param_types[i] : 0, param, &is_null,
        ctx, NULL);
  }

  PG_TRY();
  {
    if (plan->parstate) {
      ParamListInfo param_li =
          pljs_setup_variable_paramlist(plan->parstate, values, nulls);
      cursor =
          SPI_cursor_open_with_paramlist(NULL, plan->plan, param_li, false);
    } else {
      cursor = SPI_cursor_open(NULL, plan->plan, values, nulls, false);
    }
  }

  PG_CATCH();
  {
    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return js_throw("Error executing", ctx);
  }

  PG_END_TRY();
  JSValue ret = JS_NewObject(ctx);
  JSValue str = JS_NewString(ctx, cursor->name);
  JS_SetPropertyStr(ctx, ret, "name", str);
  JS_SetPropertyFunctionList(ctx, ret, js_cursor_funcs, 4);

  if (cleanup_params) {
    JS_FreeValue(ctx, params);
  }

  return ret;
}

/**
 * @brief Javascript function `cursor.fetch`.
 *
 * Javascript function that executes a fetch on a cursor.
 *
 * @returns #JSValue containing result of the fetch
 */
static JSValue pljs_plan_cursor_fetch(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *plan_name = JS_ToCString(ctx, name);

  JS_FreeCString(ctx, plan_name);
  JS_FreeValue(ctx, name);

  int nfetch = 1;
  bool forward = true, wantarray = false;

  Portal cursor = SPI_cursor_find(plan_name);

  if (cursor == NULL) {
    return js_throw("Unable to find cursor", ctx);
  }

  if (argc >= 1) {
    wantarray = true;
    JS_ToInt32(ctx, &nfetch, argv[0]);

    if (nfetch < 0) {
      nfetch = -nfetch;
      forward = false;
    }
  }

  PG_TRY();
  {
    SPI_cursor_fetch(cursor, forward, nfetch);
  }
  PG_CATCH();
  {
    SPI_rollback();
    SPI_finish();
    return js_throw("Unable to fetch", ctx);
  }
  PG_END_TRY();

  if (SPI_processed > 0) {
    if (!wantarray) {
      JSValue value = pljs_tuple_to_jsvalue(SPI_tuptable->tupdesc,
                                            SPI_tuptable->vals[0], ctx);
      SPI_freetuptable(SPI_tuptable);

      return value;
    } else {
      JSValue obj = pljs_spi_result_to_jsvalue(SPI_processed, ctx);

      SPI_freetuptable(SPI_tuptable);

      return obj;
    }
  }

  SPI_freetuptable(SPI_tuptable);

  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `cursor.move`.
 *
 * Javascript function that executes a move of the current cursor
 * @returns #JSValue containing result of the query
 */
static JSValue pljs_plan_cursor_move(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *cursor_name = JS_ToCString(ctx, name);
  int nmove = 1;
  bool forward = true;

  Portal cursor = SPI_cursor_find(cursor_name);

  JS_FreeCString(ctx, cursor_name);
  JS_FreeValue(ctx, name);

  if (cursor == NULL) {
    return js_throw("Unable to find plan", ctx);
  }

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  JS_ToInt32(ctx, &nmove, argv[0]);

  if (nmove < 0) {
    nmove = -nmove;
    forward = false;
  }

  PG_TRY();
  {
    SPI_cursor_move(cursor, forward, nmove);
  }
  PG_CATCH();
  {
    return js_throw("Unable to fetch", ctx);
  }
  PG_END_TRY();

  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `cursor.close`.
 *
 * Javascript function that closes the cursor.
 *
 * @returns #JSValue containing an integer result of 0 if
 * unsuccessful or 1 if successful
 */
static JSValue pljs_plan_cursor_close(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *cursor_name = JS_ToCString(ctx, name);
  Portal cursor = SPI_cursor_find(cursor_name);

  JS_FreeCString(ctx, cursor_name);
  JS_FreeValue(ctx, name);

  if (!cursor) {
    return js_throw("Unable to find cursor", ctx);
  }

  PG_TRY();
  {
    SPI_cursor_close(cursor);
  }
  PG_CATCH();
  {
    SPI_rollback();
    SPI_finish();
    return js_throw("Unable to close cursor", ctx);
  }
  PG_END_TRY();

  JSValue ret = JS_NewInt32(ctx, cursor ? 1 : 0);

  return ret;
}

static JSValue pljs_plan_cursor_to_string(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  return JS_NewString(ctx, "[object Cursor]");
}

static JSValue pljs_plan_to_string(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  return JS_NewString(ctx, "[object Plan]");
}

/**
 * @brief Javascript function `pljs.commit`.
 *
 * Javascript function that commits the current transaction.
 *
 * @returns #JSValue containing `undefined`
 */
static JSValue pljs_commit(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  PG_TRY();
  {
    // HoldPinnedPortals();
    SPI_commit();
    SPI_start_transaction();
  }
  PG_CATCH();
  {
    return js_throw("Unable to commit", ctx);
  }
  PG_END_TRY();

  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `pljs.rollback`.
 *
 * Javascript function that rolls back the current transaction
 *
 * @returns #JSValue containing `undefined`
 */
static JSValue pljs_rollback(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  PG_TRY();
  {
    // HoldPinnedPortals();
    SPI_rollback();
    SPI_start_transaction();
  }
  PG_CATCH();
  {
    return js_throw("Unable to rollback", ctx);
  }
  PG_END_TRY();

  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `pljs.find_function`.
 *
 * Javascript function that finds a specific Javascript function from Postgres.
 *
 * @returns #JSValue containing a Javascript function or `undefined`
 */
static JSValue pljs_find_function(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_UNDEFINED;
  }
  const char *signature = JS_ToCString(ctx, argv[0]);
  JSValue func = JS_UNDEFINED;

  PG_TRY();
  {
    Oid funcoid;

    if (!pljs_has_permission_to_execute(signature)) {
      return func;
    } else {
      if (strchr(signature, '(') == NULL) {
        funcoid = DatumGetObjectId(
            DirectFunctionCall1(regprocin, CStringGetDatum(signature)));
      } else {
        funcoid = DatumGetObjectId(
            DirectFunctionCall1(regprocedurein, CStringGetDatum(signature)));
      }

      func = pljs_find_js_function(funcoid, ctx);

      if (JS_IsUndefined(func)) {
        elog(ERROR, "javascript function is not found for \"%s\"", signature);
      }
    }
  }
  PG_CATCH();
  {
    StringInfoData str;
    initStringInfo(&str);
    appendStringInfo(&str, "javascript function is not found for \"%s\"",
                     signature);

    JS_FreeCString(ctx, signature);

    return js_throw(NameStr(str), ctx);
  }
  PG_END_TRY();

  JS_FreeCString(ctx, signature);

  return func;
}

/**
 * @brief Javascript function `pljs.return_next`.
 *
 * Javascript function that adds a value to return for a Set Returning Function.
 *
 * @returns #JSValue containing `undefined`
 */
static JSValue pljs_return_next(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  pljs_storage *storage = pljs_storage_for_context(ctx);

  pljs_return_state *retstate = storage->return_state;

  if (retstate == NULL) {
    return js_throw("return_next called in context that cannot accept a set",
                    ctx);
  }

  if (retstate->is_composite) {
    if (!JS_IsObject(argv[0])) {
      return js_throw("argument must be an object", ctx);
    }

    if (!pljs_jsvalue_object_contains_all_column_names(argv[0], ctx,
                                                       retstate->tuple_desc)) {
      return js_throw("field name / property name mismatch", ctx);
    }

    bool *nulls = (bool *)palloc0(sizeof(bool) * retstate->tuple_desc->natts);
    Datum *values = pljs_jsvalue_to_datums(NULL, argv[0], &nulls,
                                           retstate->tuple_desc, ctx);

    tuplestore_putvalues(retstate->tuple_store_state, retstate->tuple_desc,
                         values, nulls);

    pfree(nulls);
    pfree(values);
  } else {
    bool is_null = false;
    Datum result =
        pljs_jsvalue_to_datum(TupleDescAttr(retstate->tuple_desc, 0)->atttypid,
                              argv[0], &is_null, ctx, NULL);
    tuplestore_putvalues(retstate->tuple_store_state, retstate->tuple_desc,
                         &result, &is_null);
  }
  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `window.get_partition_local`.
 *
 * Javascript function that gets a local partition from a window.
 *
 * @returns #JSValue containing JSON of the stored value
 */
static JSValue pljs_window_get_partition_local(JSContext *ctx,
                                               JSValueConst this_val, int argc,
                                               JSValueConst *argv) {

  // Default to 1000.
  size_t size = 1000;

  if (argc) {
    int input_size;
    JS_ToInt32(ctx, &input_size, argv[0]);

    if (input_size < 0) {
      return js_throw("allocation size cannot be negative", ctx);
    }

    if (input_size) {
      size = input_size;
    }
  }

  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  pljs_window_storage *window_storage;

  PG_TRY();
  {
    window_storage = (pljs_window_storage *)WinGetPartitionLocalMemory(
        winobj, size + sizeof(pljs_window_storage));
  }
  PG_CATCH();
  {
    return js_throw("Unable to retrieve window storage", ctx);
  }
  PG_END_TRY();

  /* If it's new, store the maximum size. */
  if (window_storage->max_length == 0) {
    window_storage->max_length = size;
  }

  /* If nothing is stored, undefined is returned. */
  if (window_storage->length == 0) {
    return JS_UNDEFINED;
  }
  window_storage->data[window_storage->length] = '\0';

  JSValue json =
      JS_ParseJSON(ctx, window_storage->data, window_storage->length, NULL);

  return json;
}

/**
 * @brief Javascript function `window.set_partition_local`.
 *
 * Javascript function that sets a value to a local partition in a window.
 *
 * @returns #JSValue containing `undefined`
 */
static JSValue pljs_window_set_partition_local(JSContext *ctx,
                                               JSValueConst this_val, int argc,
                                               JSValueConst *argv) {
  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  JSValue js = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);

  const char *str = JS_ToCString(ctx, js);
  size_t str_size = strlen(str);

  size_t size = str_size;

  pljs_window_storage *window_storage;

  PG_TRY();
  {
    window_storage = (pljs_window_storage *)WinGetPartitionLocalMemory(
        winobj, size + sizeof(pljs_window_storage));
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  if (window_storage->max_length != 0 &&
      window_storage->max_length < size + sizeof(pljs_window_storage)) {
    return js_throw("window local memory overflow", ctx);
  } else if (window_storage->max_length == 0) {
    /* new allocation */
    window_storage->max_length = size;
  }
  window_storage->length = str_size;
  memcpy(window_storage->data, str, str_size);

  JS_FreeCString(ctx, str);
  JS_FreeValue(ctx, js);

  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `window.get_current_position`.
 *
 * Javascript function that gets the current position from a window.
 *
 * @returns #JSValue containing the position
 */
static JSValue pljs_window_get_current_position(JSContext *ctx,
                                                JSValueConst this_val, int argc,
                                                JSValueConst *argv) {
  int64 pos = 0;
  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  PG_TRY();
  {
    pos = WinGetCurrentPosition(winobj);
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  return JS_NewInt64(ctx, pos);
}

/**
 * @brief Javascript function `window.get_partition_row_count`.
 *
 * Javascript function that gets the number of rows in a partition from a
 * window.
 *
 * @returns #JSValue containing number of rows
 */
static JSValue pljs_window_get_partition_row_count(JSContext *ctx,
                                                   JSValueConst this_val,
                                                   int argc,
                                                   JSValueConst *argv) {
  int64 pos = 0;
  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  PG_TRY();
  {
    pos = WinGetPartitionRowCount(winobj);
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  return JS_NewInt64(ctx, pos);
}

/**
 * @brief Javascript function `window.set_mark_position`.
 *
 * Javascript function that sets a mark position for a window.
 *
 * @returns #JSValue containing `undefined`
 */
static JSValue pljs_window_set_mark_position(JSContext *ctx,
                                             JSValueConst this_val, int argc,
                                             JSValueConst *argv) {
  int64_t mark_pos;
  JS_ToInt64(ctx, &mark_pos, argv[0]);

  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  PG_TRY();
  {
    WinSetMarkPosition(winobj, mark_pos);
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  return JS_UNDEFINED;
}

/**
 * @brief Javascript function `window.rows_are_peers`.
 *
 * Javascript function that whether rows in a window are peers.
 *
 * @returns #JSValue containing a boolean result of `true` or `false`
 */
static JSValue pljs_window_rows_are_peers(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
  if (argc < 2) {

    return JS_UNDEFINED;
  }
  int64_t pos1;
  JS_ToInt64(ctx, &pos1, argv[0]);
  int64_t pos2;
  JS_ToInt64(ctx, &pos2, argv[1]);
  bool res = false;

  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  PG_TRY();
  {
    res = WinRowsArePeers(winobj, pos1, pos2);
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  return JS_NewBool(ctx, res);
}

static JSValue pljs_window_get_func_arg_in_partition(JSContext *ctx,
                                                     JSValueConst this_val,
                                                     int argc,
                                                     JSValueConst *argv) {
  /* Since we return undefined in "isout" case, throw if arg isn't enough. */
  if (argc < 4) {
    return js_throw("not enough arguments for get_func_arg_in_partition", ctx);
  }

  int argno;
  JS_ToInt32(ctx, &argno, argv[0]);

  int relpos;
  JS_ToInt32(ctx, &relpos, argv[1]);

  int seektype;
  JS_ToInt32(ctx, &seektype, argv[2]);

  bool set_mark = JS_ToBool(ctx, argv[3]);

  bool isnull, isout;
  Datum res;

  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  PG_TRY();
  {
    res = WinGetFuncArgInPartition(winobj, argno, relpos, seektype, set_mark,
                                   &isnull, &isout);
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  /* Return undefined to tell it's out of partition. */
  if (isout) {
    return JS_UNDEFINED;
  }

  return pljs_datum_to_jsvalue(storage->function->argtypes[argno], res, isnull,
                               true, ctx);
}

static JSValue pljs_window_get_func_arg_in_frame(JSContext *ctx,
                                                 JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  /* Since we return undefined in "isout" case, throw if arg isn't enough. */
  if (argc < 4) {
    return js_throw("not enough arguments for get_func_arg_in_partition", ctx);
  }

  int argno;
  JS_ToInt32(ctx, &argno, argv[0]);

  int relpos;
  JS_ToInt32(ctx, &relpos, argv[1]);

  int seektype;
  JS_ToInt32(ctx, &seektype, argv[2]);

  bool set_mark = JS_ToBool(ctx, argv[3]);

  bool isnull, isout;
  Datum res;

  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  PG_TRY();
  {
    res = WinGetFuncArgInFrame(winobj, argno, relpos, seektype, set_mark,
                               &isnull, &isout);
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  /* Return undefined to tell it's out of frame. */
  if (isout) {
    return JS_UNDEFINED;
  }
  return pljs_datum_to_jsvalue(storage->function->argtypes[argno], res, isnull,
                               true, ctx);
}

static JSValue pljs_window_get_func_arg_current(JSContext *ctx,
                                                JSValueConst this_val, int argc,
                                                JSValueConst *argv) {
  if (argc < 1) {
    return JS_UNDEFINED;
  }

  int argno;
  JS_ToInt32(ctx, &argno, argv[0]);

  bool isnull;
  Datum res;

  pljs_storage *storage = pljs_storage_for_context(ctx);
  FunctionCallInfo fcinfo = storage->fcinfo;

  WindowObject winobj = PG_WINDOW_OBJECT();

  PG_TRY();
  {
    res = WinGetFuncArgCurrent(winobj, argno, &isnull);
  }
  PG_CATCH();
  {
    PG_RE_THROW();
  }
  PG_END_TRY();

  return pljs_datum_to_jsvalue(storage->function->argtypes[argno], res, isnull,
                               true, ctx);
}

static JSValue pljs_window_object_to_string(JSContext *ctx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  return JS_NewString(ctx, "[object Window]");
}

static const JSCFunctionListEntry js_window_funcs[] = {
    JS_CFUNC_DEF("get_partition_local", 0, pljs_window_get_partition_local),
    JS_CFUNC_DEF("set_partition_local", 1, pljs_window_set_partition_local),
    JS_CFUNC_DEF("get_current_position", 0, pljs_window_get_current_position),
    JS_CFUNC_DEF("get_partition_row_count", 0,
                 pljs_window_get_partition_row_count),
    JS_CFUNC_DEF("set_mark_position", 1, pljs_window_set_mark_position),
    JS_CFUNC_DEF("rows_are_peers", 2, pljs_window_rows_are_peers),
    JS_CFUNC_DEF("get_func_arg_in_partition", 4,
                 pljs_window_get_func_arg_in_partition),
    JS_CFUNC_DEF("get_func_arg_in_frame", 4, pljs_window_get_func_arg_in_frame),
    JS_CFUNC_DEF("get_func_arg_current", 1, pljs_window_get_func_arg_current),
    JS_CFUNC_DEF("toString", 0, pljs_window_object_to_string)};

/**
 * @brief Javascript function `pljs.get_window_object`.
 *
 * Javascript function that a window object.
 *
 * @returns #JSValue containing the window object
 */
static JSValue pljs_get_window_object(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  pljs_storage *storage = pljs_storage_for_context(ctx);

  if (storage->window_object == NULL ||
      !WindowObjectIsValid(storage->window_object)) {
    return js_throw("get_window_object called in wrong context", ctx);
  }
  // Create the window object that we will return.
  JSValue window_obj = JS_NewObjectClass(ctx, js_window_id);

  JS_SetPropertyFunctionList(ctx, window_obj, js_window_funcs, 10);

  JS_SetPropertyStr(ctx, window_obj, "SEEK_CURRENT",
                    JS_NewInt32(ctx, WINDOW_SEEK_CURRENT));
  JS_SetPropertyStr(ctx, window_obj, "SEEK_HEAD",
                    JS_NewInt32(ctx, WINDOW_SEEK_HEAD));
  JS_SetPropertyStr(ctx, window_obj, "SEEK_TAIL",
                    JS_NewInt32(ctx, WINDOW_SEEK_TAIL));

  return window_obj;
}

static JSValue pljs_subtransaction(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
  JSValue result = JS_UNDEFINED;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  if (!IsTransactionOrTransactionBlock()) {
    return js_throw("out of transaction", ctx);
  }

  if (!JS_IsFunction(ctx, argv[0])) {
    return JS_UNDEFINED;
  }

  ResourceOwner m_resowner = CurrentResourceOwner;
  MemoryContext m_mcontext = CurrentMemoryContext;

  BeginInternalSubTransaction(NULL);
  MemoryContextSwitchTo(m_mcontext);

  result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);

  bool success = !JS_IsException(result);

  if (success) {
    ReleaseCurrentSubTransaction();
  } else {
    RollbackAndReleaseCurrentSubTransaction();
  }

  MemoryContextSwitchTo(m_mcontext);
  CurrentResourceOwner = m_resowner;

  return result;
}

#ifdef EXPOSE_GC
static JSValue pljs_gc(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
  JS_RunGC(JS_GetRuntime(ctx));

  return JS_UNDEFINED;
}
#endif

void log_type(JSContext *ctx, JSValue val) {
  if (JS_IsException(val)) {
    elog(NOTICE, "is exception");
  }

  if (JS_IsException(val)) {
    elog(NOTICE, "is exception");
  }

  if (JS_IsNumber(val)) {
    elog(NOTICE, "is number");
  }

  if (JS_IsString(val)) {
    elog(NOTICE, "is string");
  }

  if (JS_IsObject(val)) {
    elog(NOTICE, "is object");
  }

  if (JS_IsNull(val)) {
    elog(NOTICE, "is null");
  }

  if (JS_IsArray(ctx, val)) {
    elog(NOTICE, "is array");
  }

  if (JS_IsFunction(ctx, val)) {
    elog(NOTICE, "is function");
  }
}

static JSValue pljs_import(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  if (argc != 1) {
    return js_throw("import() expects exactly one argument", ctx);
  }

  if (!JS_IsString(argv[0])) {
    return js_throw("import() expects a string", ctx);
  }

  const char *path = JS_ToCString(ctx, argv[0]);
  elog(NOTICE, "Calling module load");
  JSValue ret = pljs_module_load(ctx, path);
  elog(NOTICE, "have ret");
  log_type(ctx, ret);

  ret = JS_EvalFunction(ctx, ret);
  elog(NOTICE, "evald function");
  log_type(ctx, ret);
  // ret = js_std_await(ctx, ret);
  // log_type(ctx, ret);
  elog(NOTICE, "returning");

  return ret;
}

/**
 * @brief Returns the `PLJS_VERSION` to Postgres.
 *
 * Callable function from Postgres `SELECT pljs_version();` which returns
 * a `TEXT` copy of the current compiled version.
 */
Datum pljs_version(PG_FUNCTION_ARGS) {
  int32 length = strlen(PLJS_VERSION);
  text *version = (text *)palloc0(sizeof(text) + length);

  memcpy(VARDATA(version), PLJS_VERSION, length);
  SET_VARSIZE(version, VARHDRSZ + length);

  PG_RETURN_TEXT_P(version);
}

struct JSString {
  JSRefCountHeader header; /* must come first, 32-bit */
  uint32_t len : 31;
  uint8_t is_wide_char : 1; /* 0 = 8 bits, 1 = 16 bits characters */
  /* for JS_ATOM_TYPE_SYMBOL: hash = weakref_count, atom_type = 3,
     for JS_ATOM_TYPE_PRIVATE: hash = JS_ATOM_HASH_PRIVATE, atom_type = 3
     XXX: could change encoding to have one more bit in hash */
  uint32_t hash : 30;
  uint8_t atom_type : 2; /* != 0 if atom, JS_ATOM_TYPE_x */
  uint32_t hash_next;    /* atom_index for JS_ATOM_TYPE_SYMBOL */
#ifdef DUMP_LEAKS
  struct list_head link; /* string list */
#endif
  union {
    uint8_t str8[0]; /* 8 bit strings will get an extra null terminator */
    uint16_t str16[0];
  } u;
};

typedef enum {
  JS_GC_PHASE_NONE,
  JS_GC_PHASE_DECREF,
  JS_GC_PHASE_REMOVE_CYCLES,
} JSGCPhaseEnum;

typedef struct JSShapeProperty {
  uint32_t hash_next : 26; /* 0 if last in list */
  uint32_t flags : 6;      /* JS_PROP_XXX */
  JSAtom atom;             /* JS_ATOM_NULL = free property entry */
} JSShapeProperty;

struct local_JSRuntime {
  JSMallocFunctions mf;
  JSMallocState malloc_state;
  const char *rt_info;

  int atom_hash_size; /* power of two */
  int atom_count;
  int atom_size;
  int atom_count_resize; /* resize hash table at this count */
  uint32_t *atom_hash;
  struct JSString **atom_array;
  int atom_free_index; /* 0 = none */

  int class_count; /* size of class_array */
  JSClass *class_array;

  struct list_head context_list; /* list of JSContext.link */
  /* list of JSGCObjectHeader.link. List of allocated GC objects (used
     by the garbage collector) */
  struct list_head gc_obj_list;
  /* list of JSGCObjectHeader.link. Used during JS_FreeValueRT() */
  struct list_head gc_zero_ref_count_list;
  struct list_head tmp_obj_list; /* used during GC */
  JSGCPhaseEnum gc_phase : 8;
  size_t malloc_gc_threshold;
  struct list_head weakref_list; /* list of JSWeakRefHeader.link */
#ifdef DUMP_LEAKS
  struct list_head string_list; /* list of JSString.link */
#endif
  /* stack limitation */
  uintptr_t stack_size; /* in bytes, 0 if no limit */
  uintptr_t stack_top;
  uintptr_t stack_limit; /* lower stack limit */

  JSValue current_exception;
  /* true if inside an out of memory error, to avoid recursing */
  int in_out_of_memory : 8;

  struct JSStackFrame *current_stack_frame;

  JSInterruptHandler *interrupt_handler;
  void *interrupt_opaque;

  JSHostPromiseRejectionTracker *host_promise_rejection_tracker;
  void *host_promise_rejection_tracker_opaque;

  struct list_head job_list; /* list of JSJobEntry.link */

  JSModuleNormalizeFunc *module_normalize_func;
  JSModuleLoaderFunc *module_loader_func;
  void *module_loader_opaque;
  /* timestamp for internal use in module evaluation */
  int64_t module_async_evaluation_next_timestamp;

  int can_block : 8; /* TRUE if Atomics.wait can block */
  /* used to allocate, free and clone SharedArrayBuffers */
  JSSharedArrayBufferFunctions sab_funcs;
  /* see JS_SetStripInfo() */
  uint8_t strip_flags;

  /* Shape hash table */
  int shape_hash_bits;
  int shape_hash_size;
  int shape_hash_count; /* number of hashed shapes */
  void **shape_hash;
  void *user_opaque;
};

Datum pljs_info(PG_FUNCTION_ARGS) {
  struct local_JSRuntime *local_rt = (struct local_JSRuntime *)rt;

  size_t malloc_count = local_rt->malloc_state.malloc_count;
  size_t malloc_size = local_rt->malloc_state.malloc_size;
  size_t malloc_limit = local_rt->malloc_state.malloc_limit;
  size_t stack_size = local_rt->stack_size;
  size_t stack_limit = local_rt->stack_limit;

  char *ret = palloc0(512);
  sprintf(
      ret,
      "{ \"malloc_count\": %ld, \"malloc_size\": %ld, \"malloc_limit\": %ld, "
      "\"stack_size\": %ld, \"stack_limit\": %ld }",
      malloc_count, malloc_size, malloc_limit, stack_size, stack_limit);

  return (Datum)CStringGetTextDatum(ret);
}

Datum pljs_reset(PG_FUNCTION_ARGS) {
  JS_FreeRuntime(rt);
  rt = JS_NewRuntime();
  pljs_cache_reset();

  PG_RETURN_VOID();
}
