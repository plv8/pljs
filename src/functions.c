#include "deps/quickjs/quickjs.h"
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "nodes/params.h"
#include "parser/parse_type.h"
#include "utils/elog.h"
#include "utils/fmgrprotos.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "windowapi.h"

#include "pljs.h"

// local only functions for injecting into pljs
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

// The toString for the pljs object.
static JSValue pljs_object_to_string(JSContext *ctx, JSValueConst this_obj,
                                     int argc, JSValueConst *argv) {
  return JS_NewString(ctx, "[object pljs]");
}

void pljs_setup_namespace(JSContext *ctx) {
  // get a copy of the global object.
  JSValue global_obj = JS_GetGlobalObject(ctx);

  // set up the pljs namespace and functions.
  JSValue pljs = JS_NewObjectClass(ctx, js_pljs_storage_id);

  JS_SetPropertyStr(ctx, pljs, "toString",
                    JS_NewCFunction(ctx, pljs_object_to_string, "toString", 0));

  JS_SetPropertyStr(ctx, pljs, "elog",
                    JS_NewCFunction(ctx, pljs_elog, "elog", 2));

  JS_SetPropertyStr(ctx, pljs, "execute",
                    JS_NewCFunction(ctx, pljs_execute, "execute", 2));

  JS_SetPropertyStr(ctx, pljs, "prepare",
                    JS_NewCFunction(ctx, pljs_prepare, "prepare", 2));

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

  JS_SetPropertyStr(ctx, global_obj, "pljs", pljs);

  // version.
  JS_SetPropertyStr(ctx, pljs, "version", JS_NewString(ctx, PLJS_VERSION));

  // set up logging levels in the context.
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
      return js_throw(ctx, "invalid error level");
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
    }

    const char *full_message = msg.data;

    /* ERROR case. */
    PG_TRY();
    {
      elog(level, "%s", full_message);
    }
    PG_CATCH();
    {
      ErrorData *edata = CopyErrorData();
      JSValue error = js_throw(ctx, edata->message);
      FlushErrorState();
      FreeErrorData(edata);

      return error;
    }
    PG_END_TRY();
  }

  return JS_UNDEFINED;
}

static JSValue pljs_execute(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  int status;
  const char *sql;
  JSValue params = {0};
  int nparam;
  ResourceOwner m_resowner;
  MemoryContext m_mcontext;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  sql = JS_ToCString(ctx, argv[0]);

  if (argc >= 2) {
    if (JS_IsArray(ctx, argv[1])) {
      params = argv[1];
    } else {
      /* Consume trailing elements as an array. */
      params = values_to_array(ctx, argv, argc, 1);
    }
  }

  nparam = js_array_length(ctx, params);
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
    ErrorData *edata = CopyErrorData();
    JSValue error = js_throw(ctx, edata->message);

    RollbackAndReleaseCurrentSubTransaction();
    MemoryContextSwitchTo(m_mcontext);
    CurrentResourceOwner = m_resowner;

    return error;
  }
  PG_END_TRY();

  ReleaseCurrentSubTransaction();

  MemoryContextSwitchTo(m_mcontext);
  CurrentResourceOwner = m_resowner;

  return spi_result_to_jsvalue(ctx, status);
}

static int pljs_execute_params(const char *sql, JSValue params,
                               JSContext *ctx) {
  int nparams = js_array_length(ctx, params);
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

    values[i] = pljs_jsvalue_to_datum(param, parstate.param_types[i], ctx, NULL,
                                      &is_null);
  }

  param_li = pljs_setup_variable_paramlist(&parstate, values, nulls);
  status = SPI_execute_plan_with_paramlist(plan, param_li, false, 0);

  pfree(values);
  pfree(nulls);

  return status;
}

static void js_prepared_statement_finalizer(JSRuntime *rt, JSValue val) {
  pljs_plan *plan = JS_GetOpaque(val, js_prepared_statement_handle_id);

  SPI_freeplan(plan->plan);
  pfree(plan->parstate);
  pfree(plan);
}

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

  if (argc) {
    if (JS_IsArray(ctx, argv[0])) {
      params = argv[0];
    } else {
      /* Consume trailing elements as an array. */
      params = values_to_array(ctx, argv, argc, 0);
    }
  }

  nparams = js_array_length(ctx, params);

  JSValue ptr = JS_GetPropertyStr(ctx, this_val, "plan");

  plan = JS_GetOpaque(ptr, js_prepared_statement_handle_id);

  if (plan == NULL) {
    return js_throw(ctx, "Invalid plan");
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
        param, plan->parstate ? plan->parstate->param_types[i] : 0, ctx, NULL,
        &is_null);
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
    ErrorData *edata = CopyErrorData();
    JSValue error = js_throw(ctx, edata->message);

    RollbackAndReleaseCurrentSubTransaction();
    MemoryContextSwitchTo(m_mcontext);
    CurrentResourceOwner = m_resowner;

    if (values) {
      pfree(values);
    }

    if (nulls) {
      pfree(nulls);
    }

    return error;
  }

  PG_END_TRY();

  ReleaseCurrentSubTransaction();

  MemoryContextSwitchTo(m_mcontext);
  CurrentResourceOwner = m_resowner;

  JSValue ret = spi_result_to_jsvalue(ctx, status);
  SPI_freetuptable(SPI_tuptable);

  if (values) {
    pfree(values);
  }

  if (nulls) {
    pfree(nulls);
  }

  return ret;
}

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

  return JS_NewInt32(ctx, 0);
}

static const JSCFunctionListEntry js_plan_funcs[] = {
    JS_CFUNC_DEF("execute", 2, pljs_plan_execute),
    JS_CFUNC_DEF("free", 0, pljs_plan_free),
    JS_CFUNC_DEF("cursor", 0, pljs_plan_cursor),
    JS_CFUNC_DEF("toString", 0, pljs_plan_to_string)};

static JSValue pljs_prepare(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  const char *sql;
  JSValue params = {0};
  int nparams;
  Oid *types = NULL;
  SPIPlanPtr initial = NULL, saved = NULL;
  pljs_param_state *parstate = NULL;
  pljs_plan *plan = NULL;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  if (argc >= 2) {
    if (JS_IsArray(ctx, argv[1])) {
      params = argv[1];
    } else {
      /* Consume trailing elements as an array. */
      params = values_to_array(ctx, argv, argc, 1);
    }
  }

  nparams = js_array_length(ctx, params);

  if (nparams) {
    types = palloc(sizeof(Oid) * nparams);
  }

  for (int i = 0; i < nparams; i++) {
    JSValue param = JS_GetPropertyUint32(ctx, params, i);
    size_t plen;
    const char *str = JS_ToCStringLen(ctx, &plen, param);
    int32 typemod;

    parseTypeString(str, &types[i], &typemod, false);
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
    return js_throw(ctx, "Unable to prepare parameters");
  }

  PG_END_TRY();

  if (types != NULL) {
    pfree(types);
  }

  JSValue ret = JS_NewObject(ctx);
  JSValue str = JS_NewString(ctx, "postgres execution plan");
  JS_SetPropertyStr(ctx, ret, "name", str);
  JS_SetPropertyFunctionList(ctx, ret, js_plan_funcs, 4);

  plan = palloc(sizeof(pljs_plan));

  plan->parstate = parstate;
  plan->plan = saved;

  JSValue handle = JS_NewObjectClass(ctx, js_prepared_statement_handle_id);
  JS_SetOpaque(handle, plan);
  JS_SetPropertyStr(ctx, ret, "plan", handle);

  return ret;
}

static const JSCFunctionListEntry js_cursor_funcs[] = {
    JS_CFUNC_DEF("fetch", 2, pljs_plan_cursor_fetch),
    JS_CFUNC_DEF("move", 0, pljs_plan_cursor_move),
    JS_CFUNC_DEF("close", 0, pljs_plan_cursor_close),
    JS_CFUNC_DEF("toString", 0, pljs_plan_cursor_to_string)};

static JSValue pljs_plan_cursor(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  pljs_plan *plan;
  JSValue params = {0};
  Datum *values = NULL;
  char *nulls = NULL;
  int nparams = 0;
  int argcount;
  Portal cursor;

  JSValue ptr = JS_GetPropertyStr(ctx, this_val, "plan");

  plan = JS_GetOpaque(ptr, js_prepared_statement_handle_id);

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
      params = values_to_array(ctx, argv, argc, 0);
    }
  }

  nparams = js_array_length(ctx, params);

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
        param, plan->parstate ? plan->parstate->param_types[i] : 0, ctx, NULL,
        &is_null);
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
    return js_throw(ctx, "Error executing");
  }

  PG_END_TRY();
  JSValue ret = JS_NewObject(ctx);
  JSValue str = JS_NewString(ctx, cursor->name);
  JS_SetPropertyStr(ctx, ret, "name", str);
  JS_SetPropertyFunctionList(ctx, ret, js_cursor_funcs, 4);

  return ret;
}

static JSValue pljs_plan_cursor_fetch(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *plan_name = JS_ToCString(ctx, name);
  int nfetch = 1;
  bool forward = true, wantarray = false;

  Portal cursor = SPI_cursor_find(plan_name);

  if (cursor == NULL) {
    return js_throw(ctx, "Unable to find cursor");
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
    return js_throw(ctx, "Unable to fetch");
  }
  PG_END_TRY();

  if (SPI_processed > 0) {
    if (!wantarray) {
      JSValue value =
          tuple_to_jsvalue(ctx, SPI_tuptable->tupdesc, SPI_tuptable->vals[0]);
      SPI_freetuptable(SPI_tuptable);

      return value;
    } else {
      JSValue obj = spi_result_to_jsvalue(ctx, SPI_processed);

      SPI_freetuptable(SPI_tuptable);

      return obj;
    }
  }

  SPI_freetuptable(SPI_tuptable);

  return JS_UNDEFINED;
}

static JSValue pljs_plan_cursor_move(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *cursor_name = JS_ToCString(ctx, name);
  int nmove = 1;
  bool forward = true;

  Portal cursor = SPI_cursor_find(cursor_name);

  if (cursor == NULL) {
    return js_throw(ctx, "Unable to find plan");
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
    return js_throw(ctx, "Unable to fetch");
  }
  PG_END_TRY();

  return JS_UNDEFINED;
}

static JSValue pljs_plan_cursor_close(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *cursor_name = JS_ToCString(ctx, name);
  Portal cursor = SPI_cursor_find(cursor_name);

  if (!cursor) {
    return js_throw(ctx, "Unable to find cursor");
  }

  PG_TRY();
  {
    SPI_cursor_close(cursor);
  }
  PG_CATCH();
  {
    SPI_rollback();
    SPI_finish();
    return js_throw(ctx, "Unable to close cursor");
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
    return js_throw(ctx, "Unable to commit");
  }
  PG_END_TRY();

  return JS_UNDEFINED;
}

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
    return js_throw(ctx, "Unable to rollback");
  }
  PG_END_TRY();

  return JS_UNDEFINED;
}

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

    if (!has_permission_to_execute(signature)) {
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

    return js_throw(ctx, NameStr(str));
  }
  PG_END_TRY();

  return func;
}

static JSValue pljs_return_next(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  pljs_storage *storage = pljs_storage_for_context(ctx);

  pljs_return_state *retstate = storage->return_state;

  if (retstate == NULL) {
    return js_throw(ctx,
                    "return_next called in context that cannot accept a set");
  }

  if (retstate->is_composite) {
    if (!JS_IsObject(argv[0])) {
      return js_throw(ctx, "argument must be an object");
    }

    if (!pljs_jsvalue_object_contains_all_column_names(argv[0], ctx,
                                                       retstate->tuple_desc)) {
      return js_throw(ctx, "field name / property name mismatch");
    }

    bool is_null;
    pljs_jsvalue_to_record(argv[0], NULL, ctx, &is_null, retstate->tuple_desc,
                           retstate->tuple_store_state);
  } else {
    bool is_null;
    Datum result = pljs_jsvalue_to_datum(
        argv[0], TupleDescAttr(retstate->tuple_desc, 0)->atttypid, ctx, NULL,
        &is_null);
    tuplestore_putvalues(retstate->tuple_store_state, retstate->tuple_desc,
                         &result, &is_null);
  }
  return JS_UNDEFINED;
}

static JSValue pljs_window_get_partition_local(JSContext *ctx,
                                               JSValueConst this_val, int argc,
                                               JSValueConst *argv) {

  // Default to 1000.
  size_t size = 1000;

  if (argc) {
    int input_size;
    JS_ToInt32(ctx, &input_size, argv[0]);

    if (input_size < 0) {
      return js_throw(ctx, "allocation size cannot be negative");
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
    return js_throw(ctx, "Unable to retrieve window storage");
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
    return js_throw(ctx, "window local memory overflow");
  } else if (window_storage->max_length == 0) {
    /* new allocation */
    window_storage->max_length = size;
  }
  window_storage->length = str_size;
  memcpy(window_storage->data, str, str_size);

  return JS_UNDEFINED;
}

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
    return js_throw(ctx, "not enough arguments for get_func_arg_in_partition");
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

  return pljs_datum_to_jsvalue(res, storage->function->argtypes[argno], ctx,
                               false);
}

static JSValue pljs_window_get_func_arg_in_frame(JSContext *ctx,
                                                 JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  /* Since we return undefined in "isout" case, throw if arg isn't enough. */
  if (argc < 4) {
    return js_throw(ctx, "not enough arguments for get_func_arg_in_partition");
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
  return pljs_datum_to_jsvalue(res, storage->function->argtypes[argno], ctx,
                               false);
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

  return pljs_datum_to_jsvalue(res, storage->function->argtypes[argno], ctx,
                               false);
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

static JSValue pljs_get_window_object(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  pljs_storage *storage = pljs_storage_for_context(ctx);

  if (storage->window_object == NULL ||
      !WindowObjectIsValid(storage->window_object)) {
    return js_throw(ctx, "get_window_object called in wrong context");
  }
  // Create the window object that
  JSValue window_obj = JS_NewObjectClass(ctx, js_window_id);

  // JSValue str = JS_NewString(ctx, "postgres window object");
  // JS_SetPropertyStr(ctx, window_obj, "name", str);
  JS_SetPropertyFunctionList(ctx, window_obj, js_window_funcs, 10);

  JS_SetPropertyStr(ctx, window_obj, "SEEK_CURRENT",
                    JS_NewInt32(ctx, WINDOW_SEEK_CURRENT));
  JS_SetPropertyStr(ctx, window_obj, "SEEK_HEAD",
                    JS_NewInt32(ctx, WINDOW_SEEK_HEAD));
  JS_SetPropertyStr(ctx, window_obj, "SEEK_TAIL",
                    JS_NewInt32(ctx, WINDOW_SEEK_TAIL));

  // JS_SetPropertyStr(ctx, window_obj, "class",
  //                   JS_NewString(ctx, "window object"));

  return window_obj;
}
