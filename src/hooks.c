#include "postgres.h"

#include "catalog/objectaccess.h"
#include "miscadmin.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "access/sdir.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "utils/builtins.h"
#include "utils/elog.h"

#include "pljs.h"

/* Class IDs for our JS wrapper objects. */
JSClassID js_querydesc_id;
JSClassID js_list_id;

/* Previous hook pointers for chaining. */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static planner_hook_type prev_planner = NULL;
static create_upper_paths_hook_type prev_create_upper_paths = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static set_join_pathlist_hook_type prev_set_join_pathlist = NULL;
static join_search_hook_type prev_join_search = NULL;
static get_relation_info_hook_type prev_get_relation_info = NULL;
static needs_fmgr_hook_type prev_needs_fmgr = NULL;
static fmgr_hook_type prev_fmgr = NULL;
static object_access_hook_type prev_object_access = NULL;
static object_access_hook_type_str prev_object_access_str = NULL;
static emit_log_hook_type prev_emit_log = NULL;

/* Per-hook recursion depth counters. Each hook tracks its own depth so
 * that different hook types don't interfere with each other's limits.
 * Max depth is controlled by the pljs.hooks_max_depth GUC. */
static int depth_executor_start = 0;
static int depth_executor_run = 0;
static int depth_executor_end = 0;
static int depth_planner = 0;
static int depth_create_upper_paths = 0;
static int depth_set_rel_pathlist = 0;
static int depth_set_join_pathlist = 0;
static int depth_join_search = 0;
static int depth_get_relation_info = 0;
static int depth_needs_fmgr = 0;
static int depth_fmgr = 0;
static int depth_object_access = 0;
static int depth_object_access_str = 0;
static int depth_emit_log = 0;

/*
 * ---------- String conversion helpers ----------
 */

static const char *cmdtype_to_string(CmdType operation) {
  switch (operation) {
  case CMD_SELECT:
    return "SELECT";
  case CMD_INSERT:
    return "INSERT";
  case CMD_UPDATE:
    return "UPDATE";
  case CMD_DELETE:
    return "DELETE";
  case CMD_MERGE:
    return "MERGE";
  case CMD_UTILITY:
    return "UTILITY";
  case CMD_NOTHING:
    return "NOTHING";
  default:
    return "UNKNOWN";
  }
}

static const char *scandirection_to_string(ScanDirection direction) {
  switch (direction) {
  case ForwardScanDirection:
    return "forward";
  case BackwardScanDirection:
    return "backward";
  case NoMovementScanDirection:
    return "none";
  default:
    return "unknown";
  }
}

static const char *upperrelkind_to_string(UpperRelationKind kind) {
  switch (kind) {
  case UPPERREL_SETOP:
    return "setop";
  case UPPERREL_PARTIAL_GROUP_AGG:
    return "partial_group_agg";
  case UPPERREL_GROUP_AGG:
    return "group_agg";
  case UPPERREL_WINDOW:
    return "window";
  case UPPERREL_PARTIAL_DISTINCT:
    return "partial_distinct";
  case UPPERREL_DISTINCT:
    return "distinct";
  case UPPERREL_ORDERED:
    return "ordered";
  case UPPERREL_FINAL:
    return "final";
  default:
    return "unknown";
  }
}

static const char *jointype_to_string(JoinType jt) {
  switch (jt) {
  case JOIN_INNER:
    return "inner";
  case JOIN_LEFT:
    return "left";
  case JOIN_FULL:
    return "full";
  case JOIN_RIGHT:
    return "right";
  case JOIN_SEMI:
    return "semi";
  case JOIN_ANTI:
    return "anti";
  default:
    return "unknown";
  }
}

static const char *objectaccess_to_string(ObjectAccessType access) {
  switch (access) {
  case OAT_POST_CREATE:
    return "post_create";
  case OAT_DROP:
    return "drop";
  case OAT_POST_ALTER:
    return "post_alter";
  case OAT_NAMESPACE_SEARCH:
    return "namespace_search";
  case OAT_FUNCTION_EXECUTE:
    return "function_execute";
  case OAT_TRUNCATE:
    return "truncate";
  default:
    return "unknown";
  }
}

static const char *fmgr_event_to_string(FmgrHookEventType event) {
  switch (event) {
  case FHET_START:
    return "start";
  case FHET_END:
    return "end";
  case FHET_ABORT:
    return "abort";
  default:
    return "unknown";
  }
}

static const char *error_severity_to_string(int elevel) {
  switch (elevel) {
  case DEBUG5:
  case DEBUG4:
  case DEBUG3:
  case DEBUG2:
  case DEBUG1:
    return "DEBUG";
  case LOG:
  case LOG_SERVER_ONLY:
    return "LOG";
  case INFO:
    return "INFO";
  case NOTICE:
    return "NOTICE";
  case WARNING:
  case WARNING_CLIENT_ONLY:
    return "WARNING";
  case ERROR:
    return "ERROR";
  case FATAL:
    return "FATAL";
  case PANIC:
    return "PANIC";
  default:
    return "UNKNOWN";
  }
}

/*
 * ---------- JS object conversion helpers ----------
 */

static JSValue pljs_eflags_to_jsvalue(JSContext *ctx, int eflags) {
  JSValue obj = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, obj, "explainOnly",
                    JS_NewBool(ctx, (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0));
  JS_SetPropertyStr(ctx, obj, "rewind",
                    JS_NewBool(ctx, (eflags & EXEC_FLAG_REWIND) != 0));
  JS_SetPropertyStr(ctx, obj, "backward",
                    JS_NewBool(ctx, (eflags & EXEC_FLAG_BACKWARD) != 0));
  JS_SetPropertyStr(ctx, obj, "mark",
                    JS_NewBool(ctx, (eflags & EXEC_FLAG_MARK) != 0));
  JS_SetPropertyStr(ctx, obj, "skipTriggers",
                    JS_NewBool(ctx, (eflags & EXEC_FLAG_SKIP_TRIGGERS) != 0));

  return obj;
}

JSValue pljs_querydesc_start_to_jsvalue(JSContext *ctx, QueryDesc *queryDesc,
                                        int eflags) {
  JSValue obj = JS_NewObjectClass(ctx, js_querydesc_id);
  JS_SetOpaque(obj, queryDesc);

  JS_SetPropertyStr(ctx, obj, "operation",
                    JS_NewString(ctx, cmdtype_to_string(queryDesc->operation)));
  JS_SetPropertyStr(
      ctx, obj, "sourceText",
      queryDesc->sourceText ? JS_NewString(ctx, queryDesc->sourceText)
                            : JS_UNDEFINED);
  JS_SetPropertyStr(ctx, obj, "eflags", pljs_eflags_to_jsvalue(ctx, eflags));

  return obj;
}

#if PG_VERSION_NUM >= 180000
JSValue pljs_querydesc_run_to_jsvalue(JSContext *ctx, QueryDesc *queryDesc,
                                      ScanDirection direction, uint64 count) {
#else
JSValue pljs_querydesc_run_to_jsvalue(JSContext *ctx, QueryDesc *queryDesc,
                                      ScanDirection direction, uint64 count,
                                      bool execute_once) {
#endif
  JSValue obj = JS_NewObjectClass(ctx, js_querydesc_id);
  JS_SetOpaque(obj, queryDesc);

  JS_SetPropertyStr(ctx, obj, "operation",
                    JS_NewString(ctx, cmdtype_to_string(queryDesc->operation)));
  JS_SetPropertyStr(
      ctx, obj, "sourceText",
      queryDesc->sourceText ? JS_NewString(ctx, queryDesc->sourceText)
                            : JS_UNDEFINED);
  JS_SetPropertyStr(ctx, obj, "direction",
                    JS_NewString(ctx, scandirection_to_string(direction)));
  JS_SetPropertyStr(ctx, obj, "count", JS_NewFloat64(ctx, (double)count));
#if PG_VERSION_NUM < 180000
  JS_SetPropertyStr(ctx, obj, "executeOnce", JS_NewBool(ctx, execute_once));
#endif

  return obj;
}

JSValue pljs_querydesc_to_jsvalue(JSContext *ctx, QueryDesc *queryDesc) {
  JSValue obj = JS_NewObjectClass(ctx, js_querydesc_id);
  JS_SetOpaque(obj, queryDesc);

  JS_SetPropertyStr(ctx, obj, "operation",
                    JS_NewString(ctx, cmdtype_to_string(queryDesc->operation)));
  JS_SetPropertyStr(
      ctx, obj, "sourceText",
      queryDesc->sourceText ? JS_NewString(ctx, queryDesc->sourceText)
                            : JS_UNDEFINED);

  return obj;
}

static JSValue pljs_errordata_to_jsvalue(JSContext *ctx, ErrorData *edata) {
  JSValue obj = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, obj, "severity",
                    JS_NewString(ctx, error_severity_to_string(edata->elevel)));
  JS_SetPropertyStr(ctx, obj, "elevel", JS_NewInt32(ctx, edata->elevel));

  if (edata->message)
    JS_SetPropertyStr(ctx, obj, "message",
                      JS_NewString(ctx, edata->message));

  if (edata->detail)
    JS_SetPropertyStr(ctx, obj, "detail", JS_NewString(ctx, edata->detail));

  if (edata->hint)
    JS_SetPropertyStr(ctx, obj, "hint", JS_NewString(ctx, edata->hint));

  if (edata->sqlerrcode)
    JS_SetPropertyStr(ctx, obj, "sqlstate",
                      JS_NewInt32(ctx, edata->sqlerrcode));

  if (edata->schema_name)
    JS_SetPropertyStr(ctx, obj, "schema",
                      JS_NewString(ctx, edata->schema_name));

  if (edata->table_name)
    JS_SetPropertyStr(ctx, obj, "table",
                      JS_NewString(ctx, edata->table_name));

  return obj;
}

static JSValue pljs_object_access_to_jsvalue(JSContext *ctx,
                                             ObjectAccessType access,
                                             Oid classId, Oid objectId,
                                             int subId) {
  JSValue obj = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, obj, "access",
                    JS_NewString(ctx, objectaccess_to_string(access)));
  JS_SetPropertyStr(ctx, obj, "classId", JS_NewInt64(ctx, (int64_t)classId));
  JS_SetPropertyStr(ctx, obj, "objectId", JS_NewInt64(ctx, (int64_t)objectId));
  JS_SetPropertyStr(ctx, obj, "subId", JS_NewInt32(ctx, subId));

  return obj;
}

static JSValue pljs_object_access_str_to_jsvalue(JSContext *ctx,
                                                  ObjectAccessType access,
                                                  Oid classId,
                                                  const char *objectStr,
                                                  int subId) {
  JSValue obj = JS_NewObject(ctx);

  JS_SetPropertyStr(ctx, obj, "access",
                    JS_NewString(ctx, objectaccess_to_string(access)));
  JS_SetPropertyStr(ctx, obj, "classId", JS_NewInt64(ctx, (int64_t)classId));
  if (objectStr)
    JS_SetPropertyStr(ctx, obj, "objectName",
                      JS_NewString(ctx, objectStr));
  JS_SetPropertyStr(ctx, obj, "subId", JS_NewInt32(ctx, subId));

  return obj;
}

/*
 * ---------- Context and function resolution ----------
 */

/**
 * @brief Get or create a JSContext for the current user.
 */
static JSContext *pljs_get_or_create_context(void) {
  pljs_context_cache_value *entry = pljs_cache_context_find(GetUserId());
  if (entry)
    return entry->ctx;

  JSContext *ctx = JS_NewContext(rt);
  pljs_setup_namespace(ctx);

  if (configuration.start_proc != NULL &&
      strlen(configuration.start_proc) != 0) {
    pljs_setup_start_proc(ctx);
  }

  pljs_cache_context_add(GetUserId(), ctx);
  return ctx;
}

/**
 * @brief Resolve a hook GUC function name to a JS function value.
 *
 * Returns JS_UNDEFINED if the function cannot be found. Sets *out_ctx to the
 * JSContext used.
 */
static JSValue pljs_resolve_hook_function(const char *funcname,
                                          JSContext **out_ctx) {
  JSContext *ctx;
  Oid funcoid;
  JSValue func;

  ctx = pljs_get_or_create_context();
  *out_ctx = ctx;

  if (strchr(funcname, '(') == NULL) {
    funcoid = DatumGetObjectId(
        DirectFunctionCall1(regprocin, CStringGetDatum(funcname)));
  } else {
    funcoid = DatumGetObjectId(
        DirectFunctionCall1(regprocedurein, CStringGetDatum(funcname)));
  }

  func = pljs_find_js_function(funcoid, ctx);
  return func;
}

/**
 * @brief Check if a particular hook GUC is active (hooks enabled + GUC set).
 */
static inline bool pljs_hook_is_active(const char *hook_func) {
  return configuration.hooks_enabled && hook_func != NULL &&
         hook_func[0] != '\0';
}

/*
 * ---------- Hook implementations ----------
 */

static void pljs_executor_start_hook(QueryDesc *queryDesc, int eflags) {
  if (pljs_hook_is_active(configuration.hook_executor_start)) {
    if (depth_executor_start >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: executor_start exceeded max recursion depth");
    } else {
    int saved_depth = depth_executor_start;
    depth_executor_start++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_executor_start, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        args[0] = pljs_querydesc_start_to_jsvalue(ctx, queryDesc, eflags);

        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: executor_start_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: executor_start_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_executor_start = saved_depth;
    }
  }

  if (prev_ExecutorStart)
    prev_ExecutorStart(queryDesc, eflags);
  else
    standard_ExecutorStart(queryDesc, eflags);
}

#if PG_VERSION_NUM >= 180000
static void pljs_executor_run_hook(QueryDesc *queryDesc,
                                   ScanDirection direction, uint64 count) {
#else
static void pljs_executor_run_hook(QueryDesc *queryDesc,
                                   ScanDirection direction, uint64 count,
                                   bool execute_once) {
#endif
  if (pljs_hook_is_active(configuration.hook_executor_run)) {
    if (depth_executor_run >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: executor_run exceeded max recursion depth");
    } else {
    int saved_depth = depth_executor_run;
    depth_executor_run++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_executor_run, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
#if PG_VERSION_NUM >= 180000
        args[0] = pljs_querydesc_run_to_jsvalue(ctx, queryDesc, direction,
                                                count);
#else
        args[0] = pljs_querydesc_run_to_jsvalue(ctx, queryDesc, direction,
                                                count, execute_once);
#endif

        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: executor_run_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: executor_run_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_executor_run = saved_depth;
    }
  }

#if PG_VERSION_NUM >= 180000
  if (prev_ExecutorRun)
    prev_ExecutorRun(queryDesc, direction, count);
  else
    standard_ExecutorRun(queryDesc, direction, count);
#else
  if (prev_ExecutorRun)
    prev_ExecutorRun(queryDesc, direction, count, execute_once);
  else
    standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
}

static void pljs_executor_end_hook(QueryDesc *queryDesc) {
  if (pljs_hook_is_active(configuration.hook_executor_end)) {
    if (depth_executor_end >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: executor_end exceeded max recursion depth");
    } else {
    int saved_depth = depth_executor_end;
    depth_executor_end++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_executor_end, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        args[0] = pljs_querydesc_to_jsvalue(ctx, queryDesc);

        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: executor_end_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: executor_end_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_executor_end = saved_depth;
    }
  }

  if (prev_ExecutorEnd)
    prev_ExecutorEnd(queryDesc);
  else
    standard_ExecutorEnd(queryDesc);
}

static PlannedStmt *pljs_planner_hook(Query *parse, const char *query_string,
                                      int cursorOptions,
                                      ParamListInfo boundParams) {
  if (pljs_hook_is_active(configuration.hook_planner)) {
    if (depth_planner >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: planner exceeded max recursion depth");
    } else {
    int saved_depth = depth_planner;
    depth_planner++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_planner, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        JSValue obj = JS_NewObject(ctx);

        JS_SetPropertyStr(
            ctx, obj, "operation",
            JS_NewString(ctx, cmdtype_to_string(parse->commandType)));
        if (query_string)
          JS_SetPropertyStr(ctx, obj, "queryString",
                            JS_NewString(ctx, query_string));
        JS_SetPropertyStr(ctx, obj, "cursorOptions",
                          JS_NewInt32(ctx, cursorOptions));

        args[0] = obj;
        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: planner_hook error: %s", pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: planner_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_planner = saved_depth;
    }
  }

  if (prev_planner)
    return prev_planner(parse, query_string, cursorOptions, boundParams);
  else
    return standard_planner(parse, query_string, cursorOptions, boundParams);
}

static void pljs_create_upper_paths_hook(PlannerInfo *root,
                                         UpperRelationKind stage,
                                         RelOptInfo *input_rel,
                                         RelOptInfo *output_rel, void *extra) {
  if (pljs_hook_is_active(configuration.hook_create_upper_paths)) {
    if (depth_create_upper_paths >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: create_upper_paths exceeded max recursion depth");
    } else {
    int saved_depth = depth_create_upper_paths;
    depth_create_upper_paths++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func = pljs_resolve_hook_function(
          configuration.hook_create_upper_paths, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        JSValue obj = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, obj, "stage",
                          JS_NewString(ctx, upperrelkind_to_string(stage)));
        JS_SetPropertyStr(ctx, obj, "inputRelRows",
                          JS_NewFloat64(ctx, input_rel->rows));
        JS_SetPropertyStr(ctx, obj, "outputRelRows",
                          JS_NewFloat64(ctx, output_rel->rows));

        args[0] = obj;
        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: create_upper_paths_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: create_upper_paths_hook failed: %s",
           edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_create_upper_paths = saved_depth;
    }
  }

  if (prev_create_upper_paths)
    prev_create_upper_paths(root, stage, input_rel, output_rel, extra);
}

static void pljs_set_rel_pathlist_hook(PlannerInfo *root, RelOptInfo *rel,
                                       Index rti, RangeTblEntry *rte) {
  if (pljs_hook_is_active(configuration.hook_set_rel_pathlist)) {
    if (depth_set_rel_pathlist >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: set_rel_pathlist exceeded max recursion depth");
    } else {
    int saved_depth = depth_set_rel_pathlist;
    depth_set_rel_pathlist++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func = pljs_resolve_hook_function(
          configuration.hook_set_rel_pathlist, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        JSValue obj = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, obj, "relid",
                          JS_NewInt64(ctx, (int64_t)rel->relid));
        JS_SetPropertyStr(ctx, obj, "rows", JS_NewFloat64(ctx, rel->rows));
        JS_SetPropertyStr(ctx, obj, "rti", JS_NewInt32(ctx, (int)rti));

        /* RTE info */
        if (rte->rtekind == RTE_RELATION)
          JS_SetPropertyStr(ctx, obj, "relationOid",
                            JS_NewInt64(ctx, (int64_t)rte->relid));

        args[0] = obj;
        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: set_rel_pathlist_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: set_rel_pathlist_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_set_rel_pathlist = saved_depth;
    }
  }

  if (prev_set_rel_pathlist)
    prev_set_rel_pathlist(root, rel, rti, rte);
}

static void pljs_set_join_pathlist_hook(PlannerInfo *root, RelOptInfo *joinrel,
                                        RelOptInfo *outerrel,
                                        RelOptInfo *innerrel, JoinType jointype,
                                        JoinPathExtraData *extra) {
  if (pljs_hook_is_active(configuration.hook_set_join_pathlist)) {
    if (depth_set_join_pathlist >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: set_join_pathlist exceeded max recursion depth");
    } else {
    int saved_depth = depth_set_join_pathlist;
    depth_set_join_pathlist++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func = pljs_resolve_hook_function(
          configuration.hook_set_join_pathlist, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        JSValue obj = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, obj, "joinType",
                          JS_NewString(ctx, jointype_to_string(jointype)));
        JS_SetPropertyStr(ctx, obj, "joinRelRows",
                          JS_NewFloat64(ctx, joinrel->rows));
        JS_SetPropertyStr(ctx, obj, "outerRelRows",
                          JS_NewFloat64(ctx, outerrel->rows));
        JS_SetPropertyStr(ctx, obj, "innerRelRows",
                          JS_NewFloat64(ctx, innerrel->rows));

        args[0] = obj;
        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: set_join_pathlist_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: set_join_pathlist_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_set_join_pathlist = saved_depth;
    }
  }

  if (prev_set_join_pathlist)
    prev_set_join_pathlist(root, joinrel, outerrel, innerrel, jointype, extra);
}

static RelOptInfo *pljs_join_search_hook(PlannerInfo *root, int levels_needed,
                                         List *initial_rels) {
  if (pljs_hook_is_active(configuration.hook_join_search)) {
    if (depth_join_search >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: join_search exceeded max recursion depth");
    } else {
    int saved_depth = depth_join_search;
    depth_join_search++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_join_search, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        JSValue obj = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, obj, "levelsNeeded",
                          JS_NewInt32(ctx, levels_needed));
        JS_SetPropertyStr(ctx, obj, "initialRelsCount",
                          JS_NewInt32(ctx, list_length(initial_rels)));

        args[0] = obj;
        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: join_search_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: join_search_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_join_search = saved_depth;
    }
  }

  /* Always fall through -- we cannot construct a RelOptInfo from JS. */
  if (prev_join_search)
    return prev_join_search(root, levels_needed, initial_rels);
  else
    return standard_join_search(root, levels_needed, initial_rels);
}

static void pljs_get_relation_info_hook(PlannerInfo *root, Oid relationObjectId,
                                        bool inhparent, RelOptInfo *rel) {
  if (pljs_hook_is_active(configuration.hook_get_relation_info)) {
    if (depth_get_relation_info >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: get_relation_info exceeded max recursion depth");
    } else {
    int saved_depth = depth_get_relation_info;
    depth_get_relation_info++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func = pljs_resolve_hook_function(
          configuration.hook_get_relation_info, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        JSValue obj = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, obj, "relationOid",
                          JS_NewInt64(ctx, (int64_t)relationObjectId));
        JS_SetPropertyStr(ctx, obj, "inhparent",
                          JS_NewBool(ctx, inhparent));
        JS_SetPropertyStr(ctx, obj, "rows", JS_NewFloat64(ctx, rel->rows));
        JS_SetPropertyStr(ctx, obj, "pages",
                          JS_NewFloat64(ctx, (double)rel->pages));

        args[0] = obj;
        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: get_relation_info_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: get_relation_info_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_get_relation_info = saved_depth;
    }
  }

  if (prev_get_relation_info)
    prev_get_relation_info(root, relationObjectId, inhparent, rel);
}

static bool pljs_needs_fmgr_hook(Oid fn_oid) {
  if (pljs_hook_is_active(configuration.hook_needs_fmgr)) {
    if (depth_needs_fmgr >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: needs_fmgr exceeded max recursion depth");
    } else {
    int saved_depth = depth_needs_fmgr;
    depth_needs_fmgr++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_needs_fmgr, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        args[0] = JS_NewInt64(ctx, (int64_t)fn_oid);

        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret)) {
          elog(WARNING, "pljs: needs_fmgr_hook error: %s",
               pljs_dump_error(ctx));
        } else if (JS_IsBool(ret)) {
          bool result = JS_ToBool(ctx, ret);
          JS_FreeValue(ctx, ret);
          JS_FreeValue(ctx, args[0]);
          JS_FreeValue(ctx, func);
        SPI_finish();
          return result;
        }

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: needs_fmgr_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_needs_fmgr = saved_depth;
    }
  }

  if (prev_needs_fmgr)
    return prev_needs_fmgr(fn_oid);

  return false;
}

static void pljs_fmgr_hook(FmgrHookEventType event, FmgrInfo *flinfo,
                            Datum *arg) {
  if (pljs_hook_is_active(configuration.hook_fmgr)) {
    if (depth_fmgr >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: fmgr exceeded max recursion depth");
    } else {
    int saved_depth = depth_fmgr;
    depth_fmgr++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_fmgr, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        JSValue obj = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, obj, "event",
                          JS_NewString(ctx, fmgr_event_to_string(event)));
        JS_SetPropertyStr(ctx, obj, "fnOid",
                          JS_NewInt64(ctx, (int64_t)flinfo->fn_oid));

        args[0] = obj;
        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: fmgr_hook error: %s", pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: fmgr_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_fmgr = saved_depth;
    }
  }

  if (prev_fmgr)
    prev_fmgr(event, flinfo, arg);
}

static void pljs_object_access_hook(ObjectAccessType access, Oid classId,
                                    Oid objectId, int subId, void *arg) {
  if (pljs_hook_is_active(configuration.hook_object_access)) {
    if (depth_object_access >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: object_access exceeded max recursion depth");
    } else {
    int saved_depth = depth_object_access;
    depth_object_access++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func =
          pljs_resolve_hook_function(configuration.hook_object_access, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        args[0] =
            pljs_object_access_to_jsvalue(ctx, access, classId, objectId, subId);

        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: object_access_hook error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: object_access_hook failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_object_access = saved_depth;
    }
  }

  if (prev_object_access)
    prev_object_access(access, classId, objectId, subId, arg);
}

static void pljs_object_access_str_hook(ObjectAccessType access, Oid classId,
                                        const char *objectStr, int subId,
                                        void *arg) {
  if (pljs_hook_is_active(configuration.hook_object_access_str)) {
    if (depth_object_access_str >= configuration.hooks_max_depth) {
      elog(WARNING, "pljs: object_access_str exceeded max recursion depth");
    } else {
    int saved_depth = depth_object_access_str;
    depth_object_access_str++;
    MemoryContext old_context = CurrentMemoryContext;
    PG_TRY();
    {
      JSContext *ctx;
      JSValue func = pljs_resolve_hook_function(
          configuration.hook_object_access_str, &ctx);

      if (!JS_IsUndefined(func)) {
        JSValue args[1];
        args[0] = pljs_object_access_str_to_jsvalue(ctx, access, classId,
                                                    objectStr, subId);

        SPI_connect();
        JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
        if (JS_IsException(ret))
          elog(WARNING, "pljs: object_access_hook_str error: %s",
               pljs_dump_error(ctx));

        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, func);
        SPI_finish();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);
      ErrorData *edata = CopyErrorData();
      FlushErrorState();
      elog(WARNING, "pljs: object_access_hook_str failed: %s", edata->message);
      FreeErrorData(edata);
    }
    PG_END_TRY();
    depth_object_access_str = saved_depth;
    }
  }

  if (prev_object_access_str)
    prev_object_access_str(access, classId, objectStr, subId, arg);
}

/**
 * @brief emit_log_hook callback.
 *
 * This hook is called from within the error reporting system. We use
 * depth_emit_log to allow bounded recursion and silently bail out
 * if the limit is exceeded (cannot elog from here without recursing).
 */
static void pljs_emit_log_hook(ErrorData *edata) {
  if (pljs_hook_is_active(configuration.hook_emit_log)) {
    if (depth_emit_log >= configuration.hooks_max_depth) {
      /* Cannot elog here -- would recurse. Silently skip. */
    } else {
      int saved_emit_depth = depth_emit_log;
      depth_emit_log++;

      PG_TRY();
      {
        JSContext *ctx;
        JSValue func =
            pljs_resolve_hook_function(configuration.hook_emit_log, &ctx);

        if (!JS_IsUndefined(func)) {
          JSValue args[1];
          args[0] = pljs_errordata_to_jsvalue(ctx, edata);

          JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, args);
          /* Silently discard exceptions -- we cannot log from here. */
          JS_FreeValue(ctx, ret);
          JS_FreeValue(ctx, args[0]);
          JS_FreeValue(ctx, func);
        }
      }
      PG_CATCH();
      {
        /* Silently swallow -- logging from emit_log causes recursion. */
        FlushErrorState();
      }
      PG_END_TRY();

      depth_emit_log = saved_emit_depth;
    }
  }

  if (prev_emit_log)
    prev_emit_log(edata);
}

/*
 * ---------- Initialization ----------
 */

/**
 * @brief Initialize the JS classes for hook wrapper objects.
 *
 * Must be called once per runtime (from _PG_init or equivalent).
 */
void pljs_hooks_init(JSRuntime *runtime) {
  JS_NewClassID(&js_querydesc_id);
  JS_NewClassID(&js_list_id);

  /* QueryDesc class -- opaque pointer, no destructor (we don't own it). */
  JSClassDef querydesc_class = {
      .class_name = "QueryDesc",
  };
  JS_NewClass(runtime, js_querydesc_id, &querydesc_class);

  /* List class -- opaque pointer, no destructor. */
  JSClassDef list_class = {
      .class_name = "List",
  };
  JS_NewClass(runtime, js_list_id, &list_class);
}

/**
 * @brief Save previous hooks and install PLJS hook callbacks.
 *
 * Called from _PG_init. Hooks always chain through: the JS function is
 * called for observation, then the previous hook (or standard function)
 * is invoked.
 */
void pljs_hooks_install(void) {
  prev_ExecutorStart = ExecutorStart_hook;
  ExecutorStart_hook = pljs_executor_start_hook;

  prev_ExecutorRun = ExecutorRun_hook;
  ExecutorRun_hook = pljs_executor_run_hook;

  prev_ExecutorEnd = ExecutorEnd_hook;
  ExecutorEnd_hook = pljs_executor_end_hook;

  prev_planner = planner_hook;
  planner_hook = pljs_planner_hook;

  prev_create_upper_paths = create_upper_paths_hook;
  create_upper_paths_hook = pljs_create_upper_paths_hook;

  prev_set_rel_pathlist = set_rel_pathlist_hook;
  set_rel_pathlist_hook = pljs_set_rel_pathlist_hook;

  prev_set_join_pathlist = set_join_pathlist_hook;
  set_join_pathlist_hook = pljs_set_join_pathlist_hook;

  prev_join_search = join_search_hook;
  join_search_hook = pljs_join_search_hook;

  prev_get_relation_info = get_relation_info_hook;
  get_relation_info_hook = pljs_get_relation_info_hook;

  prev_needs_fmgr = needs_fmgr_hook;
  needs_fmgr_hook = pljs_needs_fmgr_hook;

  prev_fmgr = fmgr_hook;
  fmgr_hook = pljs_fmgr_hook;

  prev_object_access = object_access_hook;
  object_access_hook = pljs_object_access_hook;

  prev_object_access_str = object_access_hook_str;
  object_access_hook_str = pljs_object_access_str_hook;

  prev_emit_log = emit_log_hook;
  emit_log_hook = pljs_emit_log_hook;
}
