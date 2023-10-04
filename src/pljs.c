#include "deps/quickjs/quickjs.h"
#include "postgres.h"

#include "access/xlog_internal.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "pljs.h"

PG_MODULE_MAGIC;

Datum pljs_call_handler(PG_FUNCTION_ARGS);
Datum pljs_call_validator(PG_FUNCTION_ARGS);
Datum pljs_inline_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pljs_call_handler);
PG_FUNCTION_INFO_V1(pljs_call_validator);
PG_FUNCTION_INFO_V1(pljs_inline_handler);

static Datum pljs_call_function(PG_FUNCTION_ARGS, pljs_context *context);

static void pljs_call_anonymous_function(JSContext *, const char *);
static Datum pljs_call_trigger(FunctionCallInfo fcinfo, pljs_context *context);

static char *dump_error(JSContext *ctx) {
  JSValue exception_val, val;
  const char *stack;
  const char *str;
  bool is_error;
  char *ret = NULL;
  size_t s1, s2;

  exception_val = JS_GetException(ctx);

  /* In the case of OOM, a null exception is thrown. */
  if (JS_IsNull(exception_val)) {
    char *oom = palloc(14);
    strcpy(oom, "out of memory");

    JS_FreeValue(ctx, exception_val);

    return oom;
  }

  is_error = JS_IsError(ctx, exception_val);
  str = JS_ToCStringLen(ctx, &s1, exception_val);

  if (!str) {
    elog(NOTICE, "error thrown but no error message");
    return NULL;
  }

  if (!is_error) {
    ret = (char *)palloc((s1 + 8) * sizeof(char));
    sprintf(ret, "Throw:\n%s", str);
  } else {
    val = JS_GetPropertyStr(ctx, exception_val, "stack");

    if (!JS_IsUndefined(val)) {
      stack = JS_ToCStringLen(ctx, &s2, val);

      ret = (char *)palloc((s1 + s2 + 2) * sizeof(char));
      sprintf(ret, "%s\n%s", str, stack);
      JS_FreeCString(ctx, stack);
    }

    JS_FreeValue(ctx, val);
  }

  JS_FreeCString(ctx, str);
  JS_FreeValue(ctx, exception_val);

  return ret;
}

JSRuntime *rt = NULL;
static uint64_t os_pending_signals = 0;

pljs_configuration configuration = {0};

static void signal_handler(int sig_num) {
  os_pending_signals |= ((uint64_t)1 << sig_num);
}

static int interrupt_handler(JSRuntime *rt, void *opaque) {
  return (os_pending_signals >> SIGINT) & 1;
}

// initialization function
void _PG_init(void) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGABRT, signal_handler);

  // initialize cache
  pljs_cache_init();

  // initialize the GUCs
  pljs_guc_init();

  // set up the quickjs runtime
  rt = JS_NewRuntime();

  // set up a memory limit if it exists
  if (configuration.memory_limit) {
    JS_SetMemoryLimit(rt, configuration.memory_limit * 1024 * 1024);
  }
}

// sets up the configuration of the extension
void pljs_guc_init() {
#ifdef EXECUTION_TIMEOUT
  DefineCustomIntVariable(
      "pljs.execution_timeout", gettext_noop("Javascriot execution timeout."),
      gettext_noop(
          "The default value is 300 seconds."
          "This allows you to override the default execution timeout."),
      &configuration.execution_timeout, 300, 1, 65536, PGC_USERSET, 0, NULL,
      NULL, NULL);
#endif

  DefineCustomIntVariable("pljs.memory_limit",
                          gettext_noop("Runtime limit in MBytes"),
                          gettext_noop("The default value is 256 MB"),
                          (int *)&configuration.memory_limit, 256, 256, 3096,
                          PGC_SUSET, 0, NULL, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.start_proc",
      gettext_noop("PLJS function to run once when PLJS is first used."), NULL,
      &configuration.start_proc, NULL, PGC_USERSET, 0, NULL, NULL, NULL);
}

static bool setup_function(FunctionCallInfo fcinfo, HeapTuple proctuple,
                           pljs_context *context) {
  Oid fn_oid = fcinfo->flinfo->fn_oid;
  Datum prosrcdatum;
  bool isnull;
  Form_pg_proc pg_proc_entry = NULL;
  pljs_func *pljs_function = NULL;
  char **arguments;
  Oid *argtypes = NULL;
  char *argmodes;
  int nargs;

  prosrcdatum =
      SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);

  if (isnull) {
    elog(ERROR, "null prosrc");

    return false;
  }

  pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);

  pljs_function = palloc0(sizeof(pljs_func));

  nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);
  pljs_function->prosrc =
      DatumGetCString(DirectFunctionCall1(textout, prosrcdatum));

  strlcpy(pljs_function->proname, NameStr(pg_proc_entry->proname), NAMEDATALEN);

  pljs_function->is_srf = pg_proc_entry->proretset;

  if (fcinfo && IsPolymorphicType(pg_proc_entry->prorettype)) {
    pljs_function->rettype = get_fn_expr_rettype(fcinfo->flinfo);
  } else {
    pljs_function->rettype = pg_proc_entry->prorettype;
  }

  JSValueConst *argv = (JSValueConst *)palloc(sizeof(JSValueConst) * nargs);

  int inargs = 0;
  for (int i = 0; i < nargs; i++) {
    Oid argtype = argtypes[i];
    char argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

    switch (argmode) {
    case PROARGMODE_IN:
    case PROARGMODE_INOUT:
    case PROARGMODE_VARIADIC:
      break;
    default:
      continue;
    }

    if (arguments && arguments[i]) {
      context->arguments[inargs] = arguments[i];
    } else {
      context->arguments[inargs] = NULL;
    }

    /* Resolve polymorphic types, if this is an actual call context. */
    if (fcinfo && IsPolymorphicType(argtype)) {
      argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
    }

    if (fcinfo->args[i].isnull == 1) {
      argv[inargs] = JS_NULL;
    } else {
      argv[inargs] = pljs_datum_to_jsvalue(fcinfo->args[inargs].value, argtype,
                                           context->ctx);
    }

    pljs_function->argtypes[inargs] = argtype;
    inargs++;
  }

  pljs_function->nargs = inargs;

  context->argv = argv;
  context->function = pljs_function;

  return true;
}

Datum pljs_call_handler(PG_FUNCTION_ARGS) {
  Oid fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple proctuple;
  Form_pg_proc pg_proc_entry = NULL;
  const char *sourcecode;
  char arguments[FUNC_MAX_ARGS][NAMEDATALEN];
  Oid *argtypes = NULL;
  char *argmodes;
  int nargs = 0;
  Datum prosrcdatum;
  bool isnull;
  JSContext *ctx;
  Datum retval;
  JSValue func;
  bool nonatomic = fcinfo->context && IsA(fcinfo->context, CallContext) &&
                   !castNode(CallContext, fcinfo->context)->atomic;
  bool is_trigger = CALLED_AS_TRIGGER(fcinfo);
  pljs_context context = {0};

  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);

    return NULL;
  }

  pljs_cache_value *entry = NULL; // pljs_cache_function_find(fn_oid);
  if (entry) {
    // elog(NOTICE, "function found");
    ctx = entry->ctx;
    func = entry->fn;
  } else {
    // elog(NOTICE, "no function found");
    //  check to see if a context exists in the cache for this user
    entry = pljs_cache_context_find(GetUserId());

    if (entry) {
      ctx = entry->ctx;
    } else {
      // create a new execution context.
      ctx = JS_NewContext(rt);

      // set up the namespace, globals and functions available inside the
      // context.
      pljs_setup_namespace(ctx);

      // save the context
      pljs_cache_context_add(GetUserId(), ctx);
    }

    context.ctx = ctx;

    setup_function(fcinfo, proctuple, &context);

    context.js_function = pljs_compile_function(&context, is_trigger);
    if (JS_IsUndefined(func)) {
      // JS_FreeContext(ctx);
      PG_RETURN_VOID();
    }

    // create the cache entry for the function.
    // pljs_cache_function_add(fn_oid, ctx, func);
  }

  if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT) {
    elog(ERROR, "could not connect to spi manager");
  }

  // do the logic
  if (is_trigger) {
    context.function->rettype = pg_proc_entry->prorettype;
    retval = pljs_call_trigger(fcinfo, &context);
  } else {
    retval = pljs_call_function(fcinfo, &context);
  }

  // do not destroy the context, it will get reused
  // JS_FreeContext(ctx);

  ReleaseSysCache(proctuple);

  SPI_finish();

  return retval;
}

Datum pljs_inline_handler(PG_FUNCTION_ARGS) {
  pljs_cache_value *entry = pljs_cache_context_find(GetUserId());

  InlineCodeBlock *code_block =
      (InlineCodeBlock *)DatumGetPointer(PG_GETARG_DATUM(0));
  char *sourcecode = code_block->source_text;

  HeapTuple proctuple;
  Form_pg_proc pg_proc_entry = NULL;
  JSContext *ctx = NULL;
  bool nonatomic = fcinfo->context && IsA(fcinfo->context, CallContext) &&
                   !castNode(CallContext, fcinfo->context)->atomic;

  if (entry) {
    ctx = entry->ctx;
  } else {
    // create a new execution context.
    ctx = JS_NewContext(rt);

    // set up the namespace, globals and functions available inside the
    // context.
    pljs_setup_namespace(ctx);

    // save the context
    pljs_cache_context_add(GetUserId(), ctx);
  }

  if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT) {
    elog(ERROR, "could not connect to spi manager");
  }

  // do the logic
  pljs_call_anonymous_function(ctx, sourcecode);

  SPI_finish();

  PG_RETURN_VOID();
}

Datum pljs_call_validator(PG_FUNCTION_ARGS) {
  Oid fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple proctuple;
  const char *sourcecode;
  Datum prosrcdatum;
  bool isnull;
  JSContext *ctx;

  if (fcinfo->flinfo->fn_extra) {
    elog(NOTICE, "fn_extra on validate");
  }
  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  prosrcdatum =
      SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);

  if (isnull) {
    elog(ERROR, "null prosrc");
  }

  sourcecode = TextDatumGetCString(prosrcdatum);

  ctx = JS_NewContext(rt);

  JSValue val = JS_Eval(ctx, sourcecode, strlen(sourcecode), "<function>",
                        JS_EVAL_FLAG_COMPILE_ONLY);

  if (JS_IsException(val)) {
    ereport(ERROR,
            (errmsg("execution error"), errdetail("%s", dump_error(ctx))));
  }

  // call validator can release the context
  JS_FreeContext(ctx);

  ReleaseSysCache(proctuple);

  PG_RETURN_VOID();
}

JSValue pljs_compile_function(pljs_context *context, bool is_trigger) {
  StringInfoData src;
  int i;

  initStringInfo(&src);

  // generate the function as javascript with all of its arguments
  appendStringInfo(&src, "function %s (", context->function->proname);

  for (i = 0; i < context->function->nargs; i++) {
    // commas between arguments
    if (i > 0) {
      appendStringInfoChar(&src, ',');
    }

    // if this is a named argument, append it
    if (context->arguments[i]) {
      appendStringInfoString(&src, context->arguments[i]);
    } else {
      // otherwise append it as an unnamed argument with a number
      appendStringInfo(&src, "$%d", i + 1);
    }
  }

  // append the other postgres-specific variables as well
  if (context->function->nargs && is_trigger) {
    appendStringInfo(&src, ", ");
  }

  if (is_trigger) {
    appendStringInfo(&src, "NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
                           "TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
  }

  appendStringInfo(&src, ") {\n%s\n}\n %s;\n", context->function->prosrc,
                   context->function->proname);

  JSValue val =
      JS_Eval(context->ctx, src.data, strlen(src.data), "<function>", 0);

  if (!JS_IsException(val)) {
    pfree(src.data);

    return val;
  } else {
    ereport(ERROR, (errmsg("execution error"),
                    errdetail("%s", dump_error(context->ctx))));

    return JS_UNDEFINED;
  }
}

static void pljs_call_anonymous_function(JSContext *ctx, const char *source) {
  StringInfoData src;

  initStringInfo(&src);

  // generate the function as javascript with all of its arguments
  appendStringInfo(&src, "(function () {\n%s\n})();", source);

  JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue val = JS_Eval(ctx, src.data, strlen(src.data), "<function>", 0);

  if (!JS_IsException(val)) {
    pfree(src.data);
  } else {

    ereport(ERROR,
            (errmsg("execution error"), errdetail("%s", dump_error(ctx))));
  }
}

static Datum pljs_call_trigger(FunctionCallInfo fcinfo, pljs_context *context) {
  TriggerData *trig = (TriggerData *)fcinfo->context;
  Relation rel = trig->tg_relation;
  TriggerEvent event = trig->tg_event;
  JSValueConst argv[10];
  Datum result = (Datum)0;

  MemoryContext execution_context =
      AllocSetContextCreate(CurrentMemoryContext, "PLJS Trigger Memory Context",
                            ALLOCSET_SMALL_SIZES);
  MemoryContext old_context = MemoryContextSwitchTo(execution_context);

  bool nonatomic = fcinfo->context && IsA(fcinfo->context, CallContext) &&
                   !castNode(CallContext, fcinfo->context)->atomic;

  if (TRIGGER_FIRED_FOR_ROW(event)) {
    TupleDesc tupdesc = RelationGetDescr(rel);

    if (TRIGGER_FIRED_BY_INSERT(event)) {
      result = PointerGetDatum(trig->tg_trigtuple);
      // NEW
      argv[0] = tuple_to_jsvalue(context->ctx, tupdesc, trig->tg_trigtuple);
      // OLD
      argv[1] = JS_UNDEFINED;
    } else if (TRIGGER_FIRED_BY_DELETE(event)) {
      result = PointerGetDatum(trig->tg_trigtuple);
      // NEW
      argv[0] = JS_UNDEFINED;
      // OLD
      argv[1] = tuple_to_jsvalue(context->ctx, tupdesc, trig->tg_trigtuple);
    } else if (TRIGGER_FIRED_BY_UPDATE(event)) {
      result = PointerGetDatum(trig->tg_newtuple);
      // NEW
      argv[0] = tuple_to_jsvalue(context->ctx, tupdesc, trig->tg_newtuple);
      // OLD
      argv[1] = tuple_to_jsvalue(context->ctx, tupdesc, trig->tg_trigtuple);
    }
  } else {
    argv[0] = argv[1] = JS_UNDEFINED;
  }

  // 2: TG_NAME
  argv[2] = JS_NewString(context->ctx, trig->tg_trigger->tgname);

  // 3: TG_WHEN
  if (TRIGGER_FIRED_BEFORE(event)) {
    argv[3] = JS_NewString(context->ctx, "BEFORE");
  } else {
    argv[3] = JS_NewString(context->ctx, "AFTER");
  }
  // 4: TG_LEVEL
  if (TRIGGER_FIRED_FOR_ROW(event)) {
    argv[4] = JS_NewString(context->ctx, "ROW");
  } else {
    argv[4] = JS_NewString(context->ctx, "STATEMENT");
  }

  // 5: TG_OP
  if (TRIGGER_FIRED_BY_INSERT(event)) {
    argv[5] = JS_NewString(context->ctx, "INSERT");
  } else if (TRIGGER_FIRED_BY_DELETE(event)) {
    argv[5] = JS_NewString(context->ctx, "DELETE");
  } else if (TRIGGER_FIRED_BY_UPDATE(event)) {
    argv[5] = JS_NewString(context->ctx, "UPDATE");
  } else if (TRIGGER_FIRED_BY_TRUNCATE(event)) {
    argv[5] = JS_NewString(context->ctx, "TRUNCATE");
  } else {
    argv[5] = JS_NewString(context->ctx, "?");
  }

  // 6: TG_RELID
  argv[6] = JS_NewInt32(context->ctx, RelationGetRelid(rel));

  // 7: TG_TABLE_NAME
  argv[7] = JS_NewString(context->ctx, RelationGetRelationName(rel));

  // 8: TG_TABLE_SCHEMA
  argv[8] =
      JS_NewString(context->ctx, get_namespace_name(RelationGetNamespace(rel)));

  // 9: TG_ARGV
  JSValue tgargv = JS_NewArray(context->ctx);

  for (int i = 0; i < trig->tg_trigger->tgnargs; i++) {
    JS_SetPropertyUint32(
        context->ctx, tgargv, i,
        JS_NewString(context->ctx, trig->tg_trigger->tgargs[i]));
  }

  argv[9] = tgargv;

  JS_SetInterruptHandler(JS_GetRuntime(context->ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue ret =
      JS_Call(context->ctx, context->js_function, JS_UNDEFINED, 10, argv);

  if (JS_IsException(ret)) {
    ereport(ERROR, (errmsg("execution error"),
                    errdetail("%s", dump_error(context->ctx))));

    JS_FreeValue(context->ctx, ret);

    MemoryContextSwitchTo(old_context);
    PG_RETURN_VOID();
  } else {
    Datum d = pljs_jsvalue_to_datum(ret, context->function->rettype,
                                    context->ctx, fcinfo, NULL);
    JS_FreeValue(context->ctx, ret);

    MemoryContextSwitchTo(old_context);
    return d;
  }
}

static Datum pljs_call_function(FunctionCallInfo fcinfo,
                                pljs_context *context) {
  MemoryContext execution_context = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS Memory Context", ALLOCSET_SMALL_SIZES);
  MemoryContext old_context = MemoryContextSwitchTo(execution_context);

  JS_SetInterruptHandler(JS_GetRuntime(context->ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue ret = JS_Call(context->ctx, context->js_function, JS_UNDEFINED,
                        context->function->nargs, context->argv);

  if (JS_IsException(ret)) {
    ereport(ERROR, (errmsg("execution error"),
                    errdetail("%s", dump_error(context->ctx))));

    JS_FreeValue(context->ctx, ret);

    MemoryContextSwitchTo(old_context);
    PG_RETURN_VOID();
  } else {
    Datum d = pljs_jsvalue_to_datum(ret, context->function->rettype,
                                    context->ctx, fcinfo, NULL);
    JS_FreeValue(context->ctx, ret);

    MemoryContextSwitchTo(old_context);
    return d;
  }
}

JSValue js_throw(JSContext *ctx, const char *message) {
  JSValue error = JS_NewError(ctx);
  JSValue message_value = JS_NewString(ctx, message);
  JS_SetPropertyStr(ctx, error, "message", message_value);

  return JS_Throw(ctx, error);
}
