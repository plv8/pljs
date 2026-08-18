#include "postgres.h"

#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "deps/quickjs/quickjs.h"
#include "windowapi.h"

#include "pljs.h"

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "pljs", .version = PLJS_VERSION);
#else
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(pljs_call_handler);
PG_FUNCTION_INFO_V1(pljs_call_validator);
PG_FUNCTION_INFO_V1(pljs_inline_handler);

static Datum call_function(PG_FUNCTION_ARGS, pljs_context *context,
                           JSValueConst *argv);
static Datum call_srf_function(PG_FUNCTION_ARGS, pljs_context *context,
                               JSValueConst *argv);

static void call_anonymous_function(const char *, JSContext *);
static Datum call_trigger(FunctionCallInfo fcinfo, pljs_context *context);
static void signal_handler(int sig_num);
static int interrupt_handler(JSRuntime *rt, void *opaque);
static void setup_storage_for_context(pljs_context *context,
                                      FunctionCallInfo fcinfo);
static void store_storage_in_context(pljs_context *context,
                                     pljs_storage *storage);

/** \brief QuickJS Runtime */
JSRuntime *rt = NULL;

/** \brief PLJS Configuration */
pljs_configuration configuration = {0};

// class id for prepared statement handles.
JSClassID js_prepared_statement_handle_id;

// class id for cursor handles.
JSClassID js_cursor_handle_id;

// class id for pljs storage
JSClassID js_pljs_storage_id;

// class id for pljs window object
JSClassID js_window_id;

static uint64_t os_pending_signals = 0;

/**
 * @brief PostgreSQL extension initialization function.
 *
 * Initialize the extension, setting up the cache, GUCs, and QuickJS
 * runtime.
 */
void _PG_init(void) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGABRT, signal_handler);

  // Initialize cache.
  pljs_cache_init();

  // Initialize the GUCs.
  pljs_guc_init();

  // Set up the quickjs runtime.
  rt = JS_NewRuntime();

  // Register runtime JS classes (must happen before any JSContext is created,
  // so every context sees the class; e.g. the prepared-statement handle whose
  // finalizer reclaims otherwise-leaked SPI plans).
  pljs_register_js_classes(rt);

  // Set up a memory limit if it exists.
  if (configuration.memory_limit) {
    JS_SetMemoryLimit(rt, configuration.memory_limit * 1024 * 1024);
  }
}

/**
 * @brief Assign hook for pljs.memory_limit.
 *
 * Re-applies the limit to the live QuickJS runtime when the GUC is changed at
 * runtime (SET pljs.memory_limit = ...).  Without this the GUC value changed
 * but the interpreter kept whatever limit was installed in _PG_init (the value
 * present when pljs was first loaded), so a runtime SET silently had no effect
 * and could not be used to contain a misbehaving function in a running backend.
 * At initial GUC definition (boot value) rt is still NULL, so this is a no-op
 * then and _PG_init installs the load-time value explicitly.
 */
static void pljs_assign_memory_limit(int newval, void *extra) {
  if (rt != NULL && newval > 0) {
    JS_SetMemoryLimit(rt, (size_t)newval * 1024 * 1024);
  }
}

/**
 * @brief Set up the GUCs.
 *
 * Sets up the GUCs that help define the behavior of the interpreter.
 */
void pljs_guc_init(void) {
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
                          gettext_noop("The default value is 512 MB"),
                          (int *)&configuration.memory_limit, 512, 64, 3096,
                          PGC_SUSET, 0, NULL, pljs_assign_memory_limit, NULL);

  DefineCustomStringVariable(
      "pljs.start_proc",
      gettext_noop("PLJS function to run once when PLJS is first used."), NULL,
      &configuration.start_proc, NULL, PGC_USERSET, 0, NULL, NULL, NULL);
}

/**
 * @brief Converts a Javascript error into a string.
 *
 * Takes a javascript context and converts it into a string,
 * allocating the memory needed in the currect memory context.
 *
 * @param ctx #JSContext - Javascript context with the error
 * @returns @c char * as an error message
 */
/*
 * Extract the current JavaScript exception.
 *
 * Returns a palloc'd string suitable for errdetail(): the stringified error
 * followed by its stack trace.  When message_out is non-NULL it is set to a
 * palloc'd copy of the error's own `message` property (or the stringified
 * throw value for non-Error throws), so callers can surface that as the
 * primary errmsg() the way plv8 does, instead of a generic "execution error".
 * Preserving the message here is what lets a coded error (e.g. the "[CODE] "
 * prefix embedded by the mirror procedures) survive across a nested
 * pljs.execute()/SPI boundary.
 */
static char *dump_error(JSContext *ctx, char **message_out, char **detail_out) {
  JSValue exception_val, val;
  const char *stack;
  const char *str;
  bool is_error;
  char *ret = NULL;
  size_t s1, s2;

  if (message_out) {
    *message_out = NULL;
  }

  if (detail_out) {
    *detail_out = NULL;
  }

  exception_val = JS_GetException(ctx);

  /* In the case of OOM, a null exception is thrown. */
  if (JS_IsNull(exception_val)) {
    char *oom = palloc(14);
    strcpy(oom, "out of memory");

    if (message_out) {
      *message_out = pstrdup("out of memory");
    }

    JS_FreeValue(ctx, exception_val);

    return oom;
  }

  is_error = JS_IsError(ctx, exception_val);
  str = JS_ToCStringLen(ctx, &s1, exception_val);

  if (!str) {
    elog(DEBUG3, "error thrown but no error message");
    return NULL;
  }

  if (detail_out && is_error) {
    /*
     * Carry a Postgres detail forward when the error came from a caught SPI
     * error (see js_throw_error_data): callers surface it as errdetail so the
     * original explanation survives a nested pljs.execute() re-raise.
     */
    JSValue detailval = JS_GetPropertyStr(ctx, exception_val, "detail");

    if (!JS_IsUndefined(detailval)) {
      const char *detailstr = JS_ToCString(ctx, detailval);

      if (detailstr && detailstr[0]) {
        *detail_out = pstrdup(detailstr);
      }

      if (detailstr) {
        JS_FreeCString(ctx, detailstr);
      }
    }

    JS_FreeValue(ctx, detailval);
  }

  if (message_out) {
    /*
     * Prefer the Error's own `message` property; fall back to the stringified
     * throw value for non-Error throws or a missing/empty message.
     */
    if (is_error) {
      JSValue msgval = JS_GetPropertyStr(ctx, exception_val, "message");

      if (!JS_IsUndefined(msgval)) {
        const char *msgstr = JS_ToCString(ctx, msgval);

        if (msgstr && msgstr[0]) {
          *message_out = pstrdup(msgstr);
        }

        if (msgstr) {
          JS_FreeCString(ctx, msgstr);
        }
      }

      JS_FreeValue(ctx, msgval);
    }

    if (*message_out == NULL) {
      *message_out = pstrdup(str);
    }
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

/*
 * Report a JavaScript exception as a PostgreSQL error.
 *
 * `message` and `detail` are dump_error()'s out-parameters; `fallback` is used when
 * the exception carried no message of its own, which is why the empty-string check
 * matters and why it lives in one place rather than at every report site.
 *
 * Does not return.
 */
pg_noreturn static void pljs_ereport_js_error(const char *message,
                                             const char *pg_detail,
                                             const char *detail,
                                             const char *fallback) {
  ereport(ERROR, (errmsg("%s", (message && message[0]) ? message : fallback),
                  errdetail("%s", (pg_detail && pg_detail[0]) ? pg_detail : detail)));
  pg_unreachable();
}


static void signal_handler(int sig_num) {
  os_pending_signals |= ((uint64_t)1 << sig_num);
}

static int interrupt_handler(JSRuntime *rt, void *opaque) {
  return (os_pending_signals >> SIGINT) & 1;
}

/**
 * @brief Set up the pljs_context.
 *
 * Sets up the pljs_context with the function and any needed contexts.
 *
 * @param fcinfo #FunctionCallInfo - optional, allows for the `fn_oid` to be
 * added
 * @param proctuple #HeapTuple - information pointer for the source and
 * argument information
 * @param context #pljs_context - context to set up the function into
 * @returns @c bool of success for failure
 */
static bool setup_function(FunctionCallInfo fcinfo, HeapTuple proctuple,
                           pljs_context *context) {
  Datum prosrcdatum;
  bool isnull;
  Form_pg_proc pg_proc_entry = NULL;
  pljs_func *pljs_function = NULL;
  char **arguments = NULL;
  Oid *argtypes = NULL;
  char *argmodes;
  int nargs;

  // Search for the system cache entry for the source of the procedure.
  prosrcdatum =
      SysCacheGetAttr(PROCOID, proctuple, Anum_pg_proc_prosrc, &isnull);

  // If we fail, all is lost already, might as well give up hope.
  if (isnull) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("unable to find prosrc"));

    return false;
  }

  pljs_function = palloc0(sizeof(pljs_func));

  // Make a copy of the source available for later compilation.
  pljs_function->prosrc =
      DatumGetCString(DirectFunctionCall1(textout, prosrcdatum));

  pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);

  // Get the actual name of the procedure.
  memcpy(pljs_function->proname, NameStr(pg_proc_entry->proname), NAMEDATALEN);

  // Are we building a set-returning function?  If so, a special case.
  pljs_function->is_srf = pg_proc_entry->proretset;

  // Figure out the return type, we care about this when we are calling
  // directly from postgres.
  if (fcinfo && IsPolymorphicType(pg_proc_entry->prorettype)) {
    pljs_function->rettype = get_fn_expr_rettype(fcinfo->flinfo);
  } else {
    pljs_function->rettype = pg_proc_entry->prorettype;
  }

  // Get the call type class
  if (fcinfo) {
    pljs_function->typeclass = get_call_result_type(fcinfo, NULL, NULL);
  }
  // Get all of the argument information.
  nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);

  int inargs = 0;
  for (int i = 0; i < nargs; i++) {
    Oid argtype = argtypes[i];
    char argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

    // Get a copy of the arguments themselves, we use them for creating the
    // function.
    if (arguments && arguments[i]) {
      context->arguments[i] = arguments[i];
    } else {
      context->arguments[i] = NULL;
    }

    /* Resolve polymorphic types, if this is an actual call context. */
    if (fcinfo && IsPolymorphicType(argtype)) {
      argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
    }

    pljs_function->argtypes[i] = argtype;
    pljs_function->argmodes[i] = argmode;

    // We differentiate input arguments from output only.
    if (argmode == PROARGMODE_IN || argmode == PROARGMODE_INOUT ||
        argmode == PROARGMODE_VARIADIC) {
      inargs++;
    }
  }

  pljs_function->inargs = inargs;
  pljs_function->nargs = nargs;

  context->function = pljs_function;

  // Functions are scoped to a user, in postgres this is a one-to-many
  // relationship, but in the extension we assign it to a context, which
  // is a single context per user.
  context->function->user_id = GetUserId();

  // If we have the function call info, we set the function OID.
  if (fcinfo) {
    context->function->fn_oid = fcinfo->flinfo->fn_oid;
  }

  return true;
}

/**
 * @brief Check to see if there is permission to execute a function.
 *
 * Searches the catalog for a function and checks to see if the current user
 * has permission to execute it.
 *
 * @param signature @c char *
 * @returns @c bool
 */
bool pljs_has_permission_to_execute(const char *signature) {
  // Stack-allocate FunctionCallInfoBaseData with
  // space for 2 arguments:
  LOCAL_FCINFO(fake_fcinfo, 2);

  FmgrInfo flinfo;

  char perm[16];
  strcpy(perm, "EXECUTE");
  text *arg = (text *)palloc(8 + VARHDRSZ);
  memcpy(VARDATA(arg), perm, 8);
  SET_VARSIZE(arg, 8 + VARHDRSZ);
  Oid funcoid;

  if (strchr(signature, '(') == NULL) {
    funcoid = DatumGetObjectId(
        DirectFunctionCall1(regprocin, CStringGetDatum(signature)));
  } else {
    funcoid = DatumGetObjectId(
        DirectFunctionCall1(regprocedurein, CStringGetDatum(signature)));
  }

  MemSet(&flinfo, 0, sizeof(flinfo));
  fake_fcinfo->flinfo = &flinfo;
  flinfo.fn_oid = InvalidOid;
  flinfo.fn_mcxt = CurrentMemoryContext;
  fake_fcinfo->nargs = 2;
  fake_fcinfo->args[0].value = ObjectIdGetDatum(funcoid);
  fake_fcinfo->args[1].value = PointerGetDatum(arg);

  Datum ret = has_function_privilege_id(fake_fcinfo);

  if (ret == 0) {
    elog(WARNING, "failed to find or no permission for js function %s",
         signature);
    return false;
  } else {
    return true;
  }
}

/**
 * @brief Validates and runs the `pljs.start_proc` if there is one.
 *
 * Finds, verifies, and executes a `pljs.start_proc` if one is set.
 * This is executed whenever a new context is created.
 */
static void setup_start_proc(JSContext *ctx) {
  JSValue func = JS_UNDEFINED;

  // Get a copy of the current memory context, we will need to switch to it in
  // case of an error.
  MemoryContext memory_context = CurrentMemoryContext;

  PG_TRY();
  {
    // Check to see if we have permission to execute the startup procedure
    if (pljs_has_permission_to_execute(configuration.start_proc)) {
      Oid funcoid;
      if (strchr(configuration.start_proc, '(') == NULL) {
        funcoid = DatumGetObjectId(DirectFunctionCall1(
            regprocin, CStringGetDatum(configuration.start_proc)));
      } else {
        funcoid = DatumGetObjectId(DirectFunctionCall1(
            regprocedurein, CStringGetDatum(configuration.start_proc)));
      }

      func = pljs_find_js_function(funcoid, ctx);
    }
  }
  PG_CATCH();
  {
    ErrorData *edata;

    // Switch out of the error memory context and back into the execution
    // context to get the error details
    MemoryContextSwitchTo(memory_context);

    edata = CopyErrorData();
    elog(WARNING, "failed to find pljs function %s: ", edata->message);
    FlushErrorState();
    FreeErrorData(edata);

    return;
  }
  PG_END_TRY();

  if (JS_IsUndefined(func)) {
    elog(DEBUG3, "javascript function is not found for \"%s\"",
         configuration.start_proc);
  } else {
    JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(ret)) {
      char *message = NULL, *pg_detail = NULL;
      char *detail = dump_error(ctx, &message, &pg_detail);

      /*
       * Release the JavaScript side before reporting: the report does not
       * return, so anything freed after it is never freed at all.
       */
      JS_FreeValue(ctx, ret);
      JS_FreeValue(ctx, func);

      pljs_ereport_js_error(message, pg_detail, detail, "start proc execution error");
    }

    /*
     * The function reference and the call's result both belong to this function.
     * Neither was released, so every context creation with pljs.start_proc set
     * leaked both.
     */
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, func);
  }
}

/**
 * @brief Converts all function call arguments from postgres to Javascript.
 *
 * Allocates and creates an array of arguments as Javascript values.
 *
 * @param fcinfo #FunctionCallInfo
 * @param proctuple #HeapTuple
 * @param context #pljs_context
 * @returns an array of #JSValueConst values of the function arguments
 */
static JSValueConst *convert_arguments_to_javascript(FunctionCallInfo fcinfo,
                                                     HeapTuple proctuple,
                                                     pljs_context *context) {
  char **arguments;
  Oid *argtypes = NULL;
  char *argmodes;
  int nargs;

  nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);

  JSValueConst *argv = (JSValueConst *)palloc(sizeof(JSValueConst) * nargs);
  int inargs = 0;

  WindowObject window_obj = PG_WINDOW_OBJECT();

  if (WindowObjectIsValid(window_obj)) {
    for (int i = 0; i < nargs; i++) {
      bool is_null;
      Datum arg = WinGetFuncArgCurrent(window_obj, i, &is_null);
      // Window functions: expand_composite=false (skip composite expansion)
      argv[i] =
          pljs_datum_to_jsvalue(argtypes[i], arg, is_null, false, context->ctx);
    }
  } else {
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

      /* Resolve polymorphic types, if this is an actual call context. */
      if (fcinfo && IsPolymorphicType(argtype)) {
        argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
      }
      bool is_null = (fcinfo->args[inargs].isnull == 1);
      // Regular functions: expand_composite=true (expand composite types)
      argv[inargs] = pljs_datum_to_jsvalue(argtype, fcinfo->args[inargs].value,
                                           is_null, true, context->ctx);

      inargs++;
    }

    /* If there are still empty arguments, fill them with `undefined`. */
    if (inargs < nargs) {
      for (int i = inargs; i < nargs; i++) {
        argv[i] = JS_UNDEFINED;
      }
    }
  }

  return argv;
}

/**
 * @brief Retrieves the #pljs_storage for a given #JSContext.
 *
 * We stash a #pljs_storage object as an opaque object connected to the
 * `pljs` global Javascript object.  This contains important function
 * call contexts that allow us to do the conversions that we need.
 *
 * @param ctx #JSContext
 * @returns #pljs_storage
 */
pljs_storage *pljs_storage_for_context(JSContext *ctx) {
  JSValue global_obj = JS_GetGlobalObject(ctx);

  JSValue pljs = JS_GetPropertyStr(ctx, global_obj, "pljs");

  pljs_storage *storage = JS_GetOpaque(pljs, js_pljs_storage_id);

  return storage;
}

/**
 * @brief Sets up the #pljs_storage object and stores it in the #JSContext.
 *
 * @param context #pljs_context
 * @param fcinfo #FunctionCalInfo
 */
static void setup_storage_for_context(pljs_context *context,
                                      FunctionCallInfo fcinfo) {
  // Set up the pljs storage object.
  pljs_storage *storage = (pljs_storage *)palloc0(sizeof(pljs_storage));

  // Function.
  storage->function = context->function;

  // Set up the MemoryContext, we should be in our execution context by now.
  storage->execution_memory_context = CurrentMemoryContext;

  // Set up the FunctionCallInfo.
  storage->fcinfo = fcinfo;

  // Current WindowObject.
  storage->window_object = PG_WINDOW_OBJECT();

  store_storage_in_context(context, storage);
}

/**
 * @brief Store the #pljs_storage object in the #JSContext.
 *
 * @param context #pljs_context
 * @param storage #pljs_storage
 */
static void store_storage_in_context(pljs_context *context,
                                     pljs_storage *storage) {
  JSValue global_obj = JS_GetGlobalObject(context->ctx);

  JSValue pljs = JS_GetPropertyStr(context->ctx, global_obj, "pljs");

  // Attach storage to the pljs object.
  JS_SetOpaque(pljs, storage);
}

/**
 * @brief Call Javascript from PostgreSQL.
 *
 * Calls Javascript in some form from PostgreSQL, returning the result on
 * success, or throwing an error on exception.  Calls can be of types
 * `function`, `procedure`, `do`, or `trigger`, and are dispatched from this
 * entry point.
 *
 * @param PG_FUNCTION_ARGS Pointer to struct FunctionCallInfoBaseData
 * @returns #Datum of the result
 */
Datum pljs_call_handler(PG_FUNCTION_ARGS) {
  Oid fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple proctuple;
  JSContext *ctx;
  Datum retval;

  bool is_trigger = CALLED_AS_TRIGGER(fcinfo);
  pljs_context context = {0};

  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("cache lookup failed for function %u", fn_oid));
  }

  // First search for a cached copy of the context.
  pljs_function_cache_value *function_entry =
      pljs_cache_function_find(GetUserId(), fn_oid);

  if (function_entry) {
    // Make a copy of the function entry to the pljs context.
    pljs_function_cache_to_context(&context, function_entry);
  } else {
    // Check to see if a context exists in the cache for this user.
    pljs_context_cache_value *entry = pljs_cache_context_find(GetUserId());

    if (entry) {
      ctx = entry->ctx;
    } else {
      // Create a new execution context.
      ctx = JS_NewContext(rt);

      // Set up the namespace, globals and functions available inside the
      // context.
      pljs_setup_namespace(ctx);

      // Check to see if there is a start_proc, if there is, attempt to apply
      // it.
      if (configuration.start_proc != NULL &&
          strlen(configuration.start_proc) != 0) {
        setup_start_proc(ctx);
      }

      // Save the context in the cache for this user id.
      pljs_cache_context_add(GetUserId(), ctx);
    }

    context.ctx = ctx;

    // Set up a copy of all of the function data.
    setup_function(fcinfo, proctuple, &context);

    // Compile the function.
    context.js_function = pljs_compile_function(&context, is_trigger);

    // If there was a problem creating the function, we'll just return VOID.
    if (JS_IsUndefined(context.js_function)) {
      /*
       * This early return bypassed the per-branch ReleaseSysCache() below, so a
       * function whose body failed to compile leaked the pg_proc pin for the
       * rest of the transaction.
       */
      ReleaseSysCache(proctuple);
      PG_RETURN_VOID();
    }

    // Create the cache entry for the function.
    pljs_cache_function_add(&context);
  }

  /*
   * NB: `proctuple` must stay pinned until we have finished reading from it,
   * but it MUST be released before we enter JavaScript, because a PROCEDURE
   * can run COMMIT/ROLLBACK internally and it is illegal to hold a syscache
   * pin across a transaction boundary (it trips "resource was not closed").
   *
   * The original code released the pin up front, before either branch, which
   * was a use-after-free: the trigger branch still read GETSTRUCT(proctuple)
   * and the function branch still passed the tuple to
   * convert_arguments_to_javascript() -> get_func_arg_info().  That only bit
   * once the entry was actually evicted (cache pressure / concurrent DDL in a
   * long-running backend).  Release it in each branch at the last safe point
   * instead: right after the final read, before any JS is executed.
   */
  if (is_trigger) {
    // Call in the context of a trigger.
    Form_pg_proc procStruct;

    procStruct = (Form_pg_proc)GETSTRUCT(proctuple);

    context.function->rettype = procStruct->prorettype;

    ReleaseSysCache(proctuple);

    retval = call_trigger(fcinfo, &context);
  } else {
    // Call as a function.
    JSValueConst *argv =
        convert_arguments_to_javascript(fcinfo, proctuple, &context);

    ReleaseSysCache(proctuple);

    // Get the old storage object.
    pljs_storage *old_storage = pljs_storage_for_context(context.ctx);

    // Set up a new storage object for this call.
    setup_storage_for_context(&context, fcinfo);

    /*
     * The storage MUST be restored even when the call raises.
     *
     * JSContexts are cached per user id and reused for the rest of the session,
     * and the storage is attached to the context, so a call that raised used to
     * leave its own storage installed permanently -- pointing at this call's
     * fcinfo, its return_state and its execution memory context, all of which
     * are gone once the error has unwound.  The next call of *any* kind that
     * reads pljs_storage_for_context() -- a trigger, a window function,
     * return_next() -- then dereferenced that stale pointer and segfaulted the
     * backend.
     *
     * This was latent until a conversion error became reachable from inside a
     * set-returning function: before that, returning an out-of-range integer
     * wrapped silently instead of raising, so nothing escaped this block.  A
     * failing SETOF call followed by any trigger reproduces it within a couple
     * of dozen iterations.
     */
    PG_TRY();
    {
      if (context.function->is_srf) {
        retval = call_srf_function(fcinfo, &context, argv);
      } else {
        retval = call_function(fcinfo, &context, argv);
      }
    }
    PG_CATCH();
    {
      store_storage_in_context(&context, old_storage);
      PG_RE_THROW();
    }
    PG_END_TRY();

    // Reset to the old storage now that the call is over.
    store_storage_in_context(&context, old_storage);
  }

  return retval;
}

/**
 * @brief Execute an inline javascript call.
 *
 * Executes whatever is passed as a `DO` call in postgres, does
 * not accept any arguments to the call, nor allow for any returned
 * data.
 *
 * @returns #Datum containing `VOID`
 */
Datum pljs_inline_handler(PG_FUNCTION_ARGS) {
  pljs_context_cache_value *entry = pljs_cache_context_find(GetUserId());

  InlineCodeBlock *code_block =
      (InlineCodeBlock *)DatumGetPointer(PG_GETARG_DATUM(0));
  char *sourcecode = code_block->source_text;

  JSContext *ctx = NULL;
  bool nonatomic = fcinfo->context && IsA(fcinfo->context, CallContext) &&
                   !castNode(CallContext, fcinfo->context)->atomic;

  // An inline handler is called separately, so there may not be a
  // context created at this point.
  if (entry) {
    ctx = entry->ctx;
  } else {
    // Create a new execution context.
    ctx = JS_NewContext(rt);

    // Set up the namespace, globals and functions available inside the
    // context.
    pljs_setup_namespace(ctx);

    // Check to see if there is a start_proc, if there is, attempt to apply
    // it.
    if (configuration.start_proc != NULL &&
        strlen(configuration.start_proc) != 0) {
      setup_start_proc(ctx);
    }

    // Save the context
    pljs_cache_context_add(GetUserId(), ctx);
  }

  if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT) {
    elog(ERROR, "could not connect to spi manager");
  }

  // Call the function.
  call_anonymous_function(sourcecode, ctx);

  SPI_finish();

  PG_RETURN_VOID();
}

/**
 * @brief Call a Javascript function.
 *
 * Calls a Javascript function returning the result on success, or throwing
 * an error on exception.
 *
 * @returns #Datum of type `VOID`
 */
Datum pljs_call_validator(PG_FUNCTION_ARGS) {
  /*
   * XXX This validator does not actually validate anything, and fixing that is
   * deliberately left out of this change.
   *
   * A language validator is called as validator(oid_of_function_being_created),
   * so the function to check is the ARGUMENT; fcinfo->flinfo->fn_oid below is
   * the validator's *own* OID.  Reading prosrc from it fetches the validator's
   * own pg_proc row, whose prosrc is the C symbol name "pljs_call_validator" --
   * which happens to parse as a bare JavaScript identifier.  So validation
   * always succeeds and an invalid body is accepted silently:
   *
   *   CREATE FUNCTION f() RETURNS int AS $$ this is ( not js $$ LANGUAGE pljs;
   *   CREATE FUNCTION
   *
   * with the syntax error only surfacing on the first call.
   *
   * Simply switching to PG_GETARG_OID(0) is NOT the fix: a pljs body is a
   * function *body*, not a standalone program -- `return 42;` is a syntax error
   * at top level -- so the validator has to wrap it the way
   * pljs_compile_function() does, with the function's argument names, before
   * compiling.  Without that, correcting the OID rejects almost every valid
   * function in the suite.  That belongs in its own change with its own tests.
   */
  Oid fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple proctuple;
  const char *sourcecode;
  Datum prosrcdatum;
  bool isnull;
  JSContext *ctx;

  if (fcinfo->flinfo->fn_extra) {
    elog(DEBUG3, "fn_extra on validate");
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
    char *message = NULL, *pg_detail = NULL;
    char *detail = dump_error(ctx, &message, &pg_detail);

    /*
     * dump_error() has copied everything we need into palloc'd memory, so the
     * JavaScript side can go now.  Without this the whole context leaks on
     * every rejected function body.
     */
    JS_FreeValue(ctx, val);
    JS_FreeContext(ctx);
    ReleaseSysCache(proctuple);

    pljs_ereport_js_error(message, pg_detail, detail, "execution error");
  }

  /*
   * Drop the compiled function before releasing the context.  JS_FreeContext()
   * does not free a context that still has live references into it, so leaving
   * this JSValue alone leaked the entire JSContext -- including all of QuickJS's
   * intrinsic objects, roughly 68KB measured -- on every single CREATE OR
   * REPLACE FUNCTION.  A backend doing repeated DDL grew past 500MB and died
   * with SIGSEGV in seconds.  QuickJS allocates on the libc heap, so none of it
   * was visible in pg_backend_memory_contexts.
   */
  JS_FreeValue(ctx, val);
  JS_FreeContext(ctx);

  ReleaseSysCache(proctuple);

  /*
   * Clear the caches: the function being created or replaced may already have a
   * compiled copy cached, and it is now stale.
   *
   * This is deliberately still the blanket reset rather than a targeted removal
   * of this one function.  A cached entry also carries the result-type
   * information resolved at its first call, and for a RECORD-returning function
   * that comes from the *call site*, not the function -- so a targeted
   * invalidation leaves the entry from an earlier call site in place and the
   * next call converts against the wrong tuple descriptor.  Until the cache key
   * covers that, the reset is what keeps it correct.
   *
   * pljs_cache_reset() now frees the QuickJS side before dropping the Postgres
   * memory that references it; it previously orphaned every cached JSContext on
   * the libc heap.
   */
  pljs_cache_reset();

  PG_RETURN_VOID();
}

/**
 * @brief Compile a javascript function and return a pointer to it.
 *
 * Sets up the arguments and code of a javascript function, compiles
 * it, and returns the function itself for use.
 *
 * @param context #pljs_context - context to compile it into, which
 * also has the current function
 * @param is_trigger #bool - whether it is to be called as a trigger
 * or not, this determines arguments
 * @returns #JSValue of the compiled function
 */
JSValue pljs_compile_function(pljs_context *context, bool is_trigger) {
  StringInfoData src;
  int i;

  initStringInfo(&src);

  // generate the function as javascript with all of its arguments
  appendStringInfo(&src, "function %s (", context->function->proname);

  int inarg = 0;
  for (i = 0; i < context->function->nargs; i++) {
    if (context->function->argmodes[i] == PROARGMODE_OUT) {
      continue;
    }
    // commas between arguments
    if (inarg > 0) {
      appendStringInfoChar(&src, ',');
    }

    // if this is a named argument, append it
    if (context->arguments[i]) {
      appendStringInfoString(&src, context->arguments[i]);
    } else {
      // otherwise append it as an unnamed argument with a number
      appendStringInfo(&src, "$%d", inarg + 1);
    }

    inarg++;
  }

  // append the other postgres-specific variables as well
  if (context->function->inargs && is_trigger) {
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
    char *message = NULL, *pg_detail = NULL;
    char *detail = dump_error(context->ctx, &message, &pg_detail);
    pljs_ereport_js_error(message, pg_detail, detail, "execution error");

    return JS_UNDEFINED;
  }

  return val;
}

/**
 * @brief Compile and call an anonymous function.
 *
 * Compiles an anonymous function inside the #JSContext passed, then
 * executes it within the context passed.
 *
 * @param source @c char * - the source code of the function to compile
 * @param ctx #JSContext - the Javascript context to compile and execute in
 */
static void call_anonymous_function(const char *source, JSContext *ctx) {
  StringInfoData src;

  initStringInfo(&src);

  // generate the function as javascript with all of its arguments
  appendStringInfo(&src, "(function () {%s})();", source);

  JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue val = JS_Eval(ctx, src.data, strlen(src.data), "<function>", 0);

  if (!JS_IsException(val)) {
    pfree(src.data);
  } else {
    char *message = NULL, *pg_detail = NULL;
    char *detail = dump_error(ctx, &message, &pg_detail);
    pljs_ereport_js_error(message, pg_detail, detail, "execution error");
  }
}

/**
 * @brief Call a trigger.
 *
 * Sets up all of the function arguments for a trigger, and calls
 * a trigger function.  This also determines the result type and
 * generates a resulting return Datum for postgres to injest.
 *
 * @param fcinfo #FunctionCallInfo
 * @param context #pljs_context
 * @returns #Datum containing the return value from the trigger
 */
static Datum call_trigger(FunctionCallInfo fcinfo, pljs_context *context) {
  TriggerData *trig = (TriggerData *)fcinfo->context;
  Relation rel = trig->tg_relation;
  TriggerEvent event = trig->tg_event;
  JSValueConst argv[10];
  Datum result = (Datum)0;

  MemoryContext execution_context = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS Trigger Memory Context (call_trigger)",
      ALLOCSET_SMALL_SIZES);
  MemoryContext old_context = MemoryContextSwitchTo(execution_context);

  if (TRIGGER_FIRED_FOR_ROW(event)) {
    TupleDesc tupdesc = RelationGetDescr(rel);

    if (TRIGGER_FIRED_BY_INSERT(event)) {
      result = PointerGetDatum(trig->tg_trigtuple);
      // NEW
      argv[0] =
          pljs_tuple_to_jsvalue(tupdesc, trig->tg_trigtuple, context->ctx);
      // OLD
      argv[1] = JS_UNDEFINED;
    } else if (TRIGGER_FIRED_BY_DELETE(event)) {
      result = PointerGetDatum(trig->tg_trigtuple);
      // NEW
      argv[0] = JS_UNDEFINED;
      // OLD
      argv[1] =
          pljs_tuple_to_jsvalue(tupdesc, trig->tg_trigtuple, context->ctx);
    } else if (TRIGGER_FIRED_BY_UPDATE(event)) {
      result = PointerGetDatum(trig->tg_newtuple);
      // NEW
      argv[0] = pljs_tuple_to_jsvalue(tupdesc, trig->tg_newtuple, context->ctx);
      // OLD
      argv[1] =
          pljs_tuple_to_jsvalue(tupdesc, trig->tg_trigtuple, context->ctx);
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

  /*
   * Connect to SPI, which call_trigger() never did -- so pljs.execute(),
   * pljs.prepare() and every other SPI entry point failed inside a trigger. Not
   * just DDL: a bare pljs.execute("SELECT 1") in a BEFORE INSERT trigger failed
   * too.  Upstream reports it as "execution error"; the error-surfacing work in
   * this series turns it into the underlying "current transaction is aborted",
   * which is what made it findable.
   *
   * Atomic unconditionally: a trigger has no CallContext, so there is no
   * nonatomic case to honour, and a trigger must not be able to commit.
   */
  if (SPI_connect_ext(0) != SPI_OK_CONNECT) {
    elog(ERROR, "could not connect to spi manager");
  }

  JS_SetInterruptHandler(JS_GetRuntime(context->ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue ret =
      JS_Call(context->ctx, context->js_function, JS_UNDEFINED, 10, argv);

  /*
   * Before the exception check, as in call_function(): the report below does
   * not return, so an SPI_finish() after it would never run.
   */
  SPI_finish();

  if (JS_IsException(ret)) {
    char *message = NULL, *pg_detail = NULL;
    char *detail = dump_error(context->ctx, &message, &pg_detail);
    pljs_ereport_js_error(message, pg_detail, detail, "execution error");

    JS_FreeValue(context->ctx, ret);

    MemoryContextSwitchTo(old_context);
    PG_RETURN_VOID();
  }

  if (JS_IsNull(ret) || !TRIGGER_FIRED_FOR_ROW(event)) {
    result = PointerGetDatum(NULL);
  } else if (!JS_IsUndefined(ret)) {

    TupleDesc tupdesc = RelationGetDescr(rel);

    pljs_type type;
    pljs_type_fill(&type, context->function->rettype);
    Datum d = pljs_jsvalue_to_record(&type, ret, NULL, tupdesc, context->ctx);

    HeapTupleHeader header = DatumGetHeapTupleHeader(d);

    result = PointerGetDatum((char *)header - HEAPTUPLESIZE);
  }

  JS_FreeValue(context->ctx, ret);

  MemoryContextSwitchTo(old_context);
  return result;
}

/**
 * @brief Call a Javascript function.
 *
 * Calls a Javascript function returning the result on success, or throwing
 * an error on exception.
 *
 * @param fcinfo #FunctionCallInfo
 * @param context #pljs_context
 * @param argv #JSValueConst - array of arguments as Javascript values
 * @returns #Datum containing the return value from the function
 */
static Datum call_function(FunctionCallInfo fcinfo, pljs_context *context,
                           JSValueConst *argv) {
  MemoryContext execution_context = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS Function Memory Context (call_function)",
      ALLOCSET_SMALL_SIZES);
  MemoryContext old_context = MemoryContextSwitchTo(execution_context);

  Oid fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple proctuple =
      SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  Oid rettype;
  Form_pg_proc pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);

  if (fcinfo && IsPolymorphicType(pg_proc_entry->prorettype)) {
    rettype = get_fn_expr_rettype(fcinfo->flinfo);
  } else {
    rettype = pg_proc_entry->prorettype;
  }
  ReleaseSysCache(proctuple);

  bool nonatomic = fcinfo->context && IsA(fcinfo->context, CallContext) &&
                   !castNode(CallContext, fcinfo->context)->atomic;
  if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT) {
    elog(ERROR, "could not connect to spi manager");
  }

  JS_SetInterruptHandler(JS_GetRuntime(context->ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue ret = JS_Call(context->ctx, context->js_function, JS_UNDEFINED,
                        context->function->inargs, argv);

  SPI_finish();

  if (JS_IsException(ret)) {
    char *message = NULL, *pg_detail = NULL;
    char *error_message = dump_error(context->ctx, &message, &pg_detail);

    JS_FreeValue(context->ctx, ret);

    pljs_ereport_js_error(message, pg_detail, error_message, "execution error");

    /* Shuts up the compiler, since ereports of ERROR stop execution. */
    return (Datum)0;
  } else {
    Datum datum = 0;

    if (rettype == RECORDOID) {
      TupleDesc tupdesc;

      /*
       * Check the status rather than discarding it.  TYPEFUNC_RECORD means the
       * caller did not supply a column definition list, and tupdesc comes back
       * NULL; pljs_jsvalue_to_record() then reached
       * lookup_rowtype_tupdesc(RECORDOID, -1) and raised "record type has not
       * been registered", which is an unhelpful way to say "you forgot
       * AS (a int, b text)".
       */
      if (get_call_result_type(fcinfo, &rettype, &tupdesc) != TYPEFUNC_COMPOSITE) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("a function returning \"record\" needs a column "
                        "definition list"),
                 errhint("Call it as ... AS (column_name data_type, ...).")));
      }

      pljs_type type;
      pljs_type_fill(&type, rettype);

      datum = pljs_jsvalue_to_record(&type, ret, NULL, tupdesc, context->ctx);
    } else {
      bool is_null;
      datum =
          pljs_jsvalue_to_datum(rettype, ret, &is_null, context->ctx, fcinfo);
    }

    JS_FreeValue(context->ctx, ret);

    MemoryContextSwitchTo(old_context);

    return datum;
  }
}

/**
 * @brief Call a set returning function (SRF).
 *
 * Sets up all of the function arguments for a set returning function,
 * and calls it.  Generally output is returned with `pljs.return_next()`,
 * but if there are additional results returned, then they are appended
 * as well.  This also determines the result type and generates a resulting
 * return Datum for postgres to injest.
 *
 * @param fcinfo #FunctionCallInfo
 * @param context #pljs_context
 * @param argv #JSValueConst - array of arguments as Javascript values
 * @returns #Datum containing the return value from the function
 */
static Datum call_srf_function(FunctionCallInfo fcinfo, pljs_context *context,
                               JSValueConst *argv) {
  pljs_return_state *state = NULL;
  MemoryContext execution_context = AllocSetContextCreate(
      CurrentMemoryContext,
      "PLJS Set Returning Memory Context (call_srf_function)",
      ALLOCSET_SMALL_SIZES);
  MemoryContext old_context = MemoryContextSwitchTo(execution_context);

  bool nonatomic = fcinfo->context && IsA(fcinfo->context, CallContext) &&
                   !castNode(CallContext, fcinfo->context)->atomic;
  if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT) {
    elog(ERROR, "could not connect to spi manager");
  }

  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;

  if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo)) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("set-valued function called in context that cannot "
                           "accept a set")));
  }

  if (!(rsinfo->allowedModes & SFRM_Materialize)) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("materialize mode required, but it is not "
                           "allowed in this context")));
  }

  if (context->function->rettype == RECORDOID) {
    if (context->function->typeclass != TYPEFUNC_COMPOSITE) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("function returning record called in context "
                             "that cannot accept type record")));
    }
  }

  MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

  TupleDesc tuple_desc;
  state = (pljs_return_state *)palloc(sizeof(pljs_return_state));

  get_call_result_type(fcinfo, &state->rettype, &tuple_desc);

  state->tuple_store_state = tuplestore_begin_heap(true, false, work_mem);

  if (!rsinfo->setDesc) {
    state->tuple_desc = CreateTupleDescCopy(rsinfo->expectedDesc);
    rsinfo->setDesc = state->tuple_desc;
  } else {
    state->tuple_desc = rsinfo->setDesc;
  }

  state->is_composite = context->function->typeclass == TYPEFUNC_COMPOSITE;

  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = state->tuple_store_state;

  MemoryContextSwitchTo(execution_context);

  pljs_storage *storage = pljs_storage_for_context(context->ctx);

  if (storage == NULL) {
    elog(ERROR, "invalid storage found on pljs object");
  }

  // Set the current return context.
  storage->return_state = state;

  JS_SetInterruptHandler(JS_GetRuntime(context->ctx), interrupt_handler, NULL);
  os_pending_signals &= ~((uint64_t)1 << SIGINT);

  JSValue ret = JS_Call(context->ctx, context->js_function, JS_UNDEFINED,
                        context->function->inargs, argv);

  SPI_finish();

  if (JS_IsException(ret)) {
    char *message = NULL, *pg_detail = NULL;
    char *error_message = dump_error(context->ctx, &message, &pg_detail);

    JS_FreeValue(context->ctx, ret);

    pljs_ereport_js_error(message, pg_detail, error_message, "execution error");

    /* Shuts up the compiler, since ereports of ERROR stop execution. */
    return (Datum)0;
  } else {
    // Check to see if we have any values to append
    if (!JS_IsUndefined(ret) && !JS_IsNull(ret)) {
      MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

      bool is_null = false;

      if (state->is_composite) {
        // Handle composite (RETURNS TABLE / RETURNS SETOF record):
        // JS can return a single object or an array of objects.
        if (JS_IsArray(context->ctx, ret)) {
          // Array of objects: iterate and put each as a row.
          for (uint32_t i = 0; i < pljs_js_array_length(ret, context->ctx);
               i++) {
            JSValue val = JS_GetPropertyUint32(context->ctx, ret, i);
            bool *nulls =
                (bool *)palloc0(sizeof(bool) * state->tuple_desc->natts);

            Datum *values = pljs_jsvalue_to_datums(
                NULL, val, &nulls, state->tuple_desc, context->ctx);

            if (values != NULL) {
              tuplestore_putvalues(state->tuple_store_state,
                                   state->tuple_desc, values, nulls);
              pfree(values);
            }
            pfree(nulls);
            JS_FreeValue(context->ctx, val);
          }
        } else {
          // Single object: put as one row.
          bool *nulls =
              (bool *)palloc0(sizeof(bool) * state->tuple_desc->natts);

          Datum *values = pljs_jsvalue_to_datums(
              NULL, ret, &nulls, state->tuple_desc, context->ctx);

          if (values != NULL) {
            tuplestore_putvalues(state->tuple_store_state,
                                 state->tuple_desc, values, nulls);
            pfree(values);
          }
          pfree(nulls);
        }
      } else {
        if (JS_IsArray(context->ctx, ret)) {
          for (uint32_t i = 0; i < pljs_js_array_length(ret, context->ctx);
               i++) {
            JSValue val = JS_GetPropertyUint32(context->ctx, ret, i);

            Datum result = pljs_jsvalue_to_datum(
                TupleDescAttr(state->tuple_desc, 0)->atttypid, val, &is_null,
                context->ctx, NULL);
            tuplestore_putvalues(state->tuple_store_state, state->tuple_desc,
                                 &result, &is_null);
            JS_FreeValue(context->ctx, val);
          }
        } else {
          if (!JS_IsUndefined(ret)) {
            Datum result = pljs_jsvalue_to_datum(
                TupleDescAttr(state->tuple_desc, 0)->atttypid, ret, &is_null,
                context->ctx, NULL);

            tuplestore_putvalues(state->tuple_store_state, state->tuple_desc,
                                 &result, &is_null);
          }
        }
      }

      MemoryContextSwitchTo(execution_context);
    }
  }

  JS_FreeValue(context->ctx, ret);

  // Switch back the original context
  MemoryContextSwitchTo(old_context);

  PG_RETURN_NULL();
}

/**
 * @brief Throws a Javascript exception.
 *
 * Throws a Javascript exception and fills it with the message passed, along
 * with as much context as it can derive from the current state of Postgres
 * when the exception is called.
 *
 * @param message @c const char * - message to throw with
 * @param ctx #JSContext - Javascript context to execute in
 * @returns #JSValue of the exception
 */
JSValue js_throw(const char *message, JSContext *ctx) {
  JSValue error = JS_NewError(ctx);
  JSValue message_value = JS_NewString(ctx, message);
  JS_SetPropertyStr(ctx, error, "message", message_value);

  return JS_Throw(ctx, error);
}

/*
 * Like js_throw(), but also attaches the Postgres error's detail, hint and
 * SQLSTATE to the JS error object (matching plv8).  Used when a Postgres
 * error caught during SPI execution is surfaced to JavaScript, so the full
 * error envelope survives into JS and can be re-raised faithfully across a
 * nested pljs.execute() boundary instead of collapsing to the message alone.
 */
JSValue js_throw_error_data(ErrorData *edata, JSContext *ctx) {
  JSValue error = JS_NewError(ctx);

  JS_SetPropertyStr(ctx, error, "message",
                    JS_NewString(ctx, edata->message ? edata->message : ""));

  if (edata->detail) {
    JS_SetPropertyStr(ctx, error, "detail", JS_NewString(ctx, edata->detail));
  }

  if (edata->hint) {
    JS_SetPropertyStr(ctx, error, "hint", JS_NewString(ctx, edata->hint));
  }

  /*
   * Expose the SQLSTATE under both names.
   *
   * The value has always been the five-character string rather than the packed
   * integer -- unpack_sql_state() is applied here -- but the property was named
   * `sqlerrcode`, which is PostgreSQL's internal name for the *packed* form.
   * Every other PL calls it `sqlstate`, and that is the name a JavaScript author
   * reaches for:
   *
   *     catch (e) { if (e.sqlstate === '23505') ... }
   *
   * `sqlerrcode` is kept so that existing code keeps working.
   */
  JS_SetPropertyStr(ctx, error, "sqlerrcode",
                    JS_NewString(ctx, unpack_sql_state(edata->sqlerrcode)));
  JS_SetPropertyStr(ctx, error, "sqlstate",
                    JS_NewString(ctx, unpack_sql_state(edata->sqlerrcode)));

  return JS_Throw(ctx, error);
}

/**
 * @brief Finds a `pljs` function and returns the JSValue of the function.
 *
 * Finds a function by its #Oid, compiles it in its own context, and
 * returns a #JSValue containing a compiled version of the function.
 * Note that no permissions checks are done, it is assumed these are
 * done before calling.
 *
 * @param fn_oid #Oid
 * @param ctx #JSContext - if NULL, the cached ctx will be used
 * @returns #JSValue representation of either the function if it exists
 * of `JS_UNDEFINED` if it does not
 */
JSValue pljs_find_js_function(Oid fn_oid, JSContext *ctx) {
  Form_pg_proc proc;
  Oid prolang;
  NameData langname = {.data = "pljs"};
  JSValue func = JS_UNDEFINED;

  HeapTuple functuple =
      SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
  if (!HeapTupleIsValid(functuple)) { // NOLINT
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  proc = (Form_pg_proc)GETSTRUCT(functuple);
  prolang = proc->prolang;

  /* Should not happen? */
  if (!OidIsValid(prolang)) { // NOLINT
    ReleaseSysCache(functuple);
    return func;
  }

  /* See if the function language is a compatible one */
  HeapTuple langtuple =
      SearchSysCache(LANGNAME, NameGetDatum(&langname), 0, 0, 0);
  if (HeapTupleIsValid(langtuple)) {
    /*
     * This is a pg_language tuple, so it must be read through
     * Form_pg_language.  It was previously cast to Form_pg_database, which
     * happened to yield the right answer only because both catalogs begin with
     * an `Oid oid` at the same offset -- any future field access, or a change to
     * either catalog's layout, would have read the wrong bytes.
     */
    Form_pg_language langForm = (Form_pg_language)GETSTRUCT(langtuple);
    Oid langtupoid = langForm->oid;

    ReleaseSysCache(langtuple);

    if (langtupoid != prolang) {
      ReleaseSysCache(functuple);
      return func;
    }
  }

  pljs_context context = {0};

  pljs_function_cache_value *function_entry =
      pljs_cache_function_find(GetUserId(), fn_oid);

  if (function_entry != NULL) {
    pljs_function_cache_to_context(&context, function_entry);

    /*
     * Hand out a reference we own.  pljs_function_cache_to_context() borrows the
     * cache's, and the cache entry is that value's only owner -- but a JSValue
     * returned from a C function belongs to its caller, so pljs.find_function()
     * handed JavaScript a reference it had not counted.  The engine dropped it
     * when the JS variable died, and after enough lookups the refcount reached
     * zero while the entry was still cached, leaving the cache holding a freed
     * object.  The next call through it terminated the backend.
     */
    func = JS_DupValue(context.ctx, context.js_function);

    /*
     * The pin was previously released only on the cache-miss branch below, so a
     * pljs.find_function() that hit the cache -- the common case once a function
     * has been called once -- held a syscache pin on pg_proc for the rest of the
     * transaction.  Repeated lookups in one transaction accumulated them, which
     * is what produces "WARNING: resource was not closed: cache pg_proc ... has
     * count N" under USE_ASSERT_CHECKING.
     */
    ReleaseSysCache(functuple);
  } else {
    pljs_context_cache_value *context_entry =
        pljs_cache_context_find(GetUserId());

    if (ctx == NULL) {
      context.ctx = context_entry->ctx;
    } else {
      context.ctx = ctx;
    }

    setup_function(NULL, functuple, &context);

    func = pljs_compile_function(&context, false);

    ReleaseSysCache(functuple);
  }

  // If there was a problem creating the function, we'll just return VOID.
  if (JS_IsUndefined(func)) {
    return JS_UNDEFINED;
  }

  return func;
}
