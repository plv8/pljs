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
#include "utils/palloc.h"
#include "utils/syscache.h"

#include "pljs.h"

Datum pljs_call_handler(PG_FUNCTION_ARGS);
Datum pljs_call_validator(PG_FUNCTION_ARGS);
Datum pljs_inline_handler(PG_FUNCTION_ARGS);

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljs_call_handler);
PG_FUNCTION_INFO_V1(pljs_call_validator);
PG_FUNCTION_INFO_V1(pljs_inline_handler);

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

Datum pljs_call_handler(PG_FUNCTION_ARGS) {
  Oid fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple proctuple;
  Form_pg_proc pg_proc_entry = NULL;
  const char *sourcecode;
  char **arguments;
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
  ;

  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  prosrcdatum =
      SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);

  if (isnull) {
    elog(ERROR, "null prosrc");
  }

  pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);
  nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);

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

    sourcecode = DatumGetCString(DirectFunctionCall1(textout, prosrcdatum));

    func = pljs_compile_function(ctx, NameStr(pg_proc_entry->proname),
                                 sourcecode, nargs, arguments);
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
  retval = call_function(fcinfo, ctx, func, nargs, argtypes,
                         pg_proc_entry->prorettype);

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

JSValue pljs_compile_function(JSContext *ctx, char *name, const char *source,
                              int nargs, char *arguments[]) {
  StringInfoData src;
  int i;

  initStringInfo(&src);

  // generate the function as javascript with all of its arguments
  appendStringInfo(&src, "function %s (", name);

  for (i = 0; i < nargs; i++) {
    // commas between arguments
    if (i > 0) {
      appendStringInfoChar(&src, ',');
    }

    // if this is a named argument, append it
    if (arguments && arguments[i]) {
      appendStringInfoString(&src, arguments[i]);
    } else {
      // otherwise append it as an unnamed argument with a number
      appendStringInfo(&src, "$%d", i + 1);
    }
  }

  // append the other postgres-specific variables as well
  if (nargs) {
    appendStringInfo(&src, ", ");
  }

  appendStringInfo(&src, "NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
                         "TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");

  appendStringInfo(&src, ") {\n%s\n}\n %s;\n", source, name);

  JSValue val = JS_Eval(ctx, src.data, strlen(src.data), "<function>", 0);

  if (!JS_IsException(val)) {
    pfree(src.data);

    return val;
  } else {
    ereport(ERROR,
            (errmsg("execution error"), errdetail("%s", dump_error(ctx))));

    return JS_UNDEFINED;
  }
}

void pljs_call_anonymous_function(JSContext *ctx, const char *source) {
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

static Datum call_function(FunctionCallInfo fcinfo, JSContext *ctx,
                           JSValue func, int nargs, Oid *argtypes,
                           Oid rettype) {
  JSValueConst *argv = (JSValueConst *)palloc(sizeof(JSValueConst) * nargs);

  MemoryContext old_context = CurrentMemoryContext;
  MemoryContext execution_context = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS Memory Context", ALLOCSET_SMALL_SIZES);

  for (int i = 0; i < nargs; i++) {
    if (fcinfo->args[i].isnull == 1) {
      argv[i] = JS_NULL;
    } else {
      argv[i] = pljs_datum_to_jsvalue(fcinfo->args[i].value, argtypes[i], ctx);
    }
  }

  JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, nargs, argv);

  if (JS_IsException(ret)) {
    ereport(ERROR,
            (errmsg("execution error"), errdetail("%s", dump_error(ctx))));

    JS_FreeValue(ctx, ret);

    CurrentMemoryContext = old_context;
    PG_RETURN_VOID();
  } else {
    Datum d = pljs_jsvalue_to_datum(ret, rettype, ctx, fcinfo, NULL);
    JS_FreeValue(ctx, ret);

    CurrentMemoryContext = old_context;
    return d;
  }
}

JSValue js_throw(JSContext *ctx, const char *message) {
  JSValue error = JS_NewError(ctx);
  JSValue message_value = JS_NewString(ctx, message);
  JS_SetPropertyStr(ctx, error, "message", message_value);

  return JS_Throw(ctx, error);
}
