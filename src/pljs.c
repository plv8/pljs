#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "deps/quickjs/quickjs.h"
#include "windowapi.h"

#include "pljs.h"

#include <string.h>

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
static Datum convert_result(FunctionCallInfo fcinfo, pljs_context *context,
                            Oid rettype, JSValue ret);

static void pljs_source_to_utf8(StringInfoData *src);
static void pljs_build_function_source(StringInfoData *src,
                                       pljs_context *context, bool is_trigger);
static void call_anonymous_function(const char *, JSContext *);
static Datum call_trigger(FunctionCallInfo fcinfo, pljs_context *context);
static Datum dispatch_call(FunctionCallInfo fcinfo);
static void run_inline(FunctionCallInfo fcinfo);
static int interrupt_handler(JSRuntime *rt, void *opaque);
static void setup_storage(pljs_storage *storage, pljs_func *function,
                          FunctionCallInfo fcinfo);
static JSValue js_throw_uncatchable(ErrorData *edata, JSContext *ctx);
static void pljs_run_with_storage(pljs_func *function, FunctionCallInfo fcinfo,
                                  bool own_spi, void (*run)(void *), void *arg);
static void pljs_raise_if_ending(void);
static bool pljs_argmode_is_input(char argmode);

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

/*
 * The storage of the pljs call running now -- a function, trigger or DO block
 * -- or NULL outside of one; see pljs_current_storage().
 */
static pljs_storage *current_storage = NULL;

/*
 * The error the running call has to end with, or NULL; see
 * pljs_throw_fatal_error().
 */
static ErrorData *pljs_fatal_error(void) {
  return current_storage != NULL ? current_storage->fatal_error : NULL;
}

/*
 * Whether a function's result is being converted, anywhere up the stack; see
 * pljs_commit().
 */
bool pljs_converting_result = false;

/**
 * @brief Creates and configures the QuickJS runtime.
 *
 * Everything a runtime needs before any JSContext is created in it is set
 * here, so the runtime pljs_reset() replaces is set up the same way as the
 * one _PG_init() created.
 */
void pljs_runtime_init(void) {
  rt = JS_NewRuntime();

  /*
   * Bound the JS call-stack budget explicitly instead of trusting the vendored
   * JS_DEFAULT_STACK_SIZE.  Without an active limit, unbounded/deep JS
   * recursion runs the backend's C stack into the ground and crashes the whole
   * process (SIGSEGV) rather than raising a catchable "stack overflow".  We
   * size the JS budget to half of the DBA's max_stack_depth so pure-JS
   * recursion trips QuickJS's guard well before PostgreSQL's own C-stack limit
   * (and long before the kernel stack limit) is reached, with a floor so a
   * small max_stack_depth still leaves JS usable.  PostgreSQL's own
   * check_stack_depth() bounds any cumulative C stack consumed across nested
   * JS<->SQL re-entries.
   *
   * The budget set here is a size, not an anchor: JS_NewRuntime() records the
   * stack top at this point in _PG_init, which is not where any real JS call
   * begins.  Each entry into JS therefore calls JS_UpdateStackTop() to
   * re-anchor the measurement at its own depth -- see the call sites around
   * each JS_Call().
   *
   * Note an asymmetry with pljs.memory_limit below, which does install an
   * assign hook: max_stack_depth is read once, here, so a later SET
   * max_stack_depth does not change the JS budget for the life of the backend.
   * It is a core GUC with no hook available to us, and re-reading it per call
   * would let one session's SET silently resize a runtime shared with every
   * other function in the backend.
   */
  {
    long depth_bytes = (long)max_stack_depth * 1024L;
    size_t js_stack_size = (size_t)(depth_bytes / 2);
    if (js_stack_size < 256 * 1024) {
      js_stack_size = 256 * 1024;
    }
    JS_SetMaxStackSize(rt, js_stack_size);
  }

  /*
   * Stop JavaScript for a cancel or a termination; see interrupt_handler().
   * Here, once, rather than before each call: a new backend's pljs.start_proc
   * and a function's top-level code both run before any call, and ran with no
   * handler at all, so a statement_timeout never stopped them, and nor did
   * pg_terminate_backend().
   */
  JS_SetInterruptHandler(rt, interrupt_handler, NULL);

  // Register runtime JS classes (must happen before any JSContext is created,
  // so every context sees the class; e.g. the prepared-statement handle whose
  // finalizer reclaims otherwise-leaked SPI plans).
  pljs_register_js_classes(rt);

  // Set up a memory limit if it exists.
  if (configuration.memory_limit) {
    JS_SetMemoryLimit(rt, configuration.memory_limit * 1024 * 1024);
  }

  // Initialize hook JS classes.
  pljs_hooks_init(rt);
}

/**
 * @brief PostgreSQL extension initialization function.
 *
 * Initialize the extension, setting up the cache, GUCs, and QuickJS
 * runtime.
 */
void _PG_init(void) {
  // NB: do NOT install our own signal() handlers here.  This runs inside a
  // backend that has already set up PostgreSQL's own SIGINT/SIGTERM handlers
  // (query cancel, fast shutdown); overwriting them process-wide broke
  // statement_timeout / pg_cancel_backend / pg_terminate_backend for the whole
  // backend for the rest of its life.  Instead, interrupt_handler() consults
  // PostgreSQL's interrupt flags directly (see below).

  // Initialize cache.
  pljs_cache_init();

  // Initialize what the type conversions keep for the life of the backend.
  pljs_type_io_init();

  // Initialize the GUCs.
  pljs_guc_init();

  // Set up the quickjs runtime.
  pljs_runtime_init();

  // Install hook callbacks.
  pljs_hooks_install();
}

/**
 * GUC check callbacks for hook-related GUCs.
 *
 * Without shared_preload_libraries, PGC_SUSET is not enforced for GUC
 * placeholders set before the library loads. These check callbacks reject
 * non-superuser attempts from interactive sources (SET command), closing
 * the security gap where a non-superuser could set hook GUCs as
 * placeholders that later take effect for superuser queries.
 */
static bool pljs_hook_bool_check(bool *newval, void **extra,
                                 GucSource source) {
  if (*newval && source >= PGC_S_INTERACTIVE && !superuser()) {
    GUC_check_errmsg("permission denied to set hook parameter");
    GUC_check_errhint("Must be superuser to enable PLJS hooks.");
    return false;
  }
  return true;
}

static bool pljs_hook_int_check(int *newval, void **extra, GucSource source) {
  if (source >= PGC_S_INTERACTIVE && !superuser()) {
    GUC_check_errmsg("permission denied to set hook parameter");
    GUC_check_errhint("Must be superuser to configure PLJS hooks.");
    return false;
  }
  return true;
}

static bool pljs_hook_string_check(char **newval, void **extra,
                                   GucSource source) {
  if (*newval && (*newval)[0] != '\0' && source >= PGC_S_INTERACTIVE &&
      !superuser()) {
    GUC_check_errmsg("permission denied to set hook parameter");
    GUC_check_errhint("Must be superuser to configure PLJS hooks.");
    return false;
  }
  return true;
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

  DefineCustomBoolVariable(
      "pljs.hooks_enabled",
      gettext_noop("Enable PLJS executor hooks."),
      gettext_noop("When enabled, PLJS will install executor hooks that allow "
                   "JavaScript functions to intercept query execution. "
                   "Requires superuser to enable."),
      &configuration.hooks_enabled, false, PGC_SUSET, 0,
      pljs_hook_bool_check, NULL, NULL);

  DefineCustomIntVariable(
      "pljs.hooks_max_depth",
      gettext_noop("Maximum recursion depth for PLJS hooks."),
      gettext_noop("Limits how deeply hooks can recurse when a hook "
                   "function triggers the same hook via pljs.execute(). "
                   "Requires superuser to change."),
      &configuration.hooks_max_depth, 5, 1, 64, PGC_SUSET, 0,
      pljs_hook_int_check, NULL, NULL);

  // Hook GUCs - each names a PLJS function to call for the given hook.
  // All require superuser and pljs.hooks_enabled = true at runtime.
  DefineCustomStringVariable(
      "pljs.executor_start_hook",
      gettext_noop("PLJS function to call on ExecutorStart."), NULL,
      &configuration.hook_executor_start, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.executor_run_hook",
      gettext_noop("PLJS function to call on ExecutorRun."), NULL,
      &configuration.hook_executor_run, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.executor_end_hook",
      gettext_noop("PLJS function to call on ExecutorEnd."), NULL,
      &configuration.hook_executor_end, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.planner_hook",
      gettext_noop("PLJS function to call on planner."), NULL,
      &configuration.hook_planner, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.create_upper_paths_hook",
      gettext_noop("PLJS function to call on create_upper_paths."), NULL,
      &configuration.hook_create_upper_paths, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.set_rel_pathlist_hook",
      gettext_noop("PLJS function to call on set_rel_pathlist."), NULL,
      &configuration.hook_set_rel_pathlist, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.set_join_pathlist_hook",
      gettext_noop("PLJS function to call on set_join_pathlist."), NULL,
      &configuration.hook_set_join_pathlist, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.join_search_hook",
      gettext_noop("PLJS function to call on join_search."), NULL,
      &configuration.hook_join_search, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.get_relation_info_hook",
      gettext_noop("PLJS function to call on get_relation_info."), NULL,
      &configuration.hook_get_relation_info, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.needs_fmgr_hook",
      gettext_noop("PLJS function to call on needs_fmgr."), NULL,
      &configuration.hook_needs_fmgr, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.fmgr_hook",
      gettext_noop("PLJS function to call on fmgr."), NULL,
      &configuration.hook_fmgr, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.object_access_hook",
      gettext_noop("PLJS function to call on object_access."), NULL,
      &configuration.hook_object_access, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.object_access_hook_str",
      gettext_noop("PLJS function to call on object_access_hook_str."), NULL,
      &configuration.hook_object_access_str, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);

  DefineCustomStringVariable(
      "pljs.emit_log_hook",
      gettext_noop("PLJS function to call on emit_log."), NULL,
      &configuration.hook_emit_log, NULL, PGC_SUSET, 0,
      pljs_hook_string_check, NULL, NULL);
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
 *
 * sqlstate_out is set the same way from the error's `sqlstate` property, so a
 * PostgreSQL error that passed through JavaScript is re-raised under its own
 * SQLSTATE instead of collapsing to XX000.
 */
/*
 * Reads a property of a thrown error as a palloc'd string, or NULL if it has
 * none.  A getter that throws, or a value whose toString() does, is taken for
 * none, and its exception released: it was left pending in the context, and
 * kept what it threw alive until the next exception replaced it.
 */
static char *dump_error_property(JSContext *ctx, JSValueConst error,
                                 const char *name) {
  JSValue value = JS_GetPropertyStr(ctx, error, name);
  const char *str;
  char *ret;

  if (JS_IsException(value)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return NULL;
  }

  if (JS_IsUndefined(value)) {
    return NULL;
  }

  str = JS_ToCString(ctx, value);
  JS_FreeValue(ctx, value);

  if (str == NULL) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return NULL;
  }

  ret = pstrdup(str);
  JS_FreeCString(ctx, str);

  return ret;
}

static char *dump_error(JSContext *ctx, char **message_out, char **detail_out,
                        char **sqlstate_out) {
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

  if (sqlstate_out) {
    *sqlstate_out = NULL;
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

    /*
     * The thrown value's toString() threw in turn.  Release that exception as
     * well as the thrown value, rather than leave it pending in the context.
     */
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, exception_val);

    return NULL;
  }

  if (detail_out && is_error) {
    /*
     * Carry a Postgres detail forward when the error came from a caught SPI
     * error (see js_throw_error_data): callers surface it as errdetail so the
     * original explanation survives a nested pljs.execute() re-raise.
     */
    char *detail = dump_error_property(ctx, exception_val, "detail");

    if (detail && detail[0]) {
      *detail_out = detail;
    }
  }

  if (sqlstate_out && is_error) {
    /*
     * js_throw_error_data() attaches the five-character SQLSTATE of the
     * PostgreSQL error it wrapped.  Anything else -- a plain `throw new
     * Error(...)` from user code -- has no such property and is left for the
     * caller to report as ERRCODE_INTERNAL_ERROR.
     */
    char *sqlstate = dump_error_property(ctx, exception_val, "sqlstate");

    if (sqlstate && strlen(sqlstate) == 5) {
      *sqlstate_out = sqlstate;
    }
  }

  if (message_out) {
    /*
     * Prefer the Error's own `message` property; fall back to the stringified
     * throw value for non-Error throws or a missing/empty message.
     */
    if (is_error) {
      char *message = dump_error_property(ctx, exception_val, "message");

      if (message && message[0]) {
        *message_out = message;
      }
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

    /*
     * A `stack` that cannot be made a string -- a Symbol, or a getter that
     * throws -- is treated as no stack at all.  Its NULL went to sprintf(),
     * which wrote "(null)" past the end of a buffer sized for an empty stack.
     */
    stack = JS_IsUndefined(val) ? NULL : JS_ToCStringLen(ctx, &s2, val);

    if (stack != NULL) {
      ret = (char *)palloc((s1 + s2 + 2) * sizeof(char));
      sprintf(ret, "%s\n%s", str, stack);
      JS_FreeCString(ctx, stack);
    } else if (!JS_IsUndefined(val)) {
      JS_FreeValue(ctx, JS_GetException(ctx));
    }

    JS_FreeValue(ctx, val);
  }

  JS_FreeCString(ctx, str);
  JS_FreeValue(ctx, exception_val);

  return ret;
}

/**
 * @brief Extract the current JavaScript exception as an error string.
 *
 * The exported form of dump_error(), for hooks.c and for companion
 * extensions, which have no use for the error's message, detail or SQLSTATE
 * on their own.
 *
 * @param ctx #JSContext - Javascript context with the error
 * @returns @c char * as an error message
 */
char *pljs_dump_error(JSContext *ctx) {
  return dump_error(ctx, NULL, NULL, NULL);
}

/*
 * Report a JavaScript exception as a PostgreSQL error.
 *
 * `message`, `pg_detail` and `sqlstate` are dump_error()'s out-parameters;
 * `fallback` is used when the exception carried no message of its own, which is
 * why the empty-string check matters and why it lives in one place rather than
 * at every report site.
 *
 * An exception that came from a caught PostgreSQL error is re-raised under that
 * error's SQLSTATE, so a caller can still tell `division by zero` from a bug in
 * the extension after it has crossed the JavaScript boundary.  A JavaScript
 * error of any other origin has no SQLSTATE and keeps ERRCODE_INTERNAL_ERROR.
 *
 * Does not return.
 */
pg_noreturn static void pljs_ereport_js_error(const char *message,
                                              const char *pg_detail,
                                              const char *detail,
                                              const char *sqlstate,
                                              const char *fallback) {
  int sqlerrcode = ERRCODE_INTERNAL_ERROR;
  const char *emessage = (message && message[0]) ? message : fallback;
  const char *edetail = (pg_detail && pg_detail[0]) ? pg_detail : detail;
  ErrorData *fatal_error = pljs_fatal_error();

  /* The exception stood for this error; see pljs_throw_fatal_error(). */
  if (fatal_error != NULL) {
    ReThrowError(fatal_error);
  }

  if (sqlstate && strlen(sqlstate) == 5) {
    sqlerrcode = MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2],
                               sqlstate[3], sqlstate[4]);
  }

  /*
   * What JavaScript wrote is UTF-8, and a message in another database
   * encoding had every accented letter as the two bytes of its UTF-8.  Never
   * raising, which would replace the error being reported; see
   * pljs_utf8_to_server_lossy().
   */
  emessage = pljs_utf8_to_server_lossy(emessage, strlen(emessage));

  if (edetail == NULL) {
    edetail = fallback;
  }

  edetail = pljs_utf8_to_server_lossy(edetail, strlen(edetail));

  ereport(ERROR, (errcode(sqlerrcode), errmsg("%s", emessage),
                  errdetail("%s", edetail)));
  pg_unreachable();
}

/**
 * @brief Raises the pending JavaScript exception as a PostgreSQL error.
 *
 * For C code that finds a QuickJS call has failed because JavaScript threw --
 * a getter, toString() or valueOf() run while a value was being converted --
 * and has to raise rather than hand the exception back to JavaScript.  The
 * error keeps the exception's message, and its SQLSTATE if it wrapped a
 * PostgreSQL error.
 *
 * @param ctx #JSContext - the context with the pending exception
 */
void pljs_ereport_js_exception(JSContext *ctx) {
  char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
  char *detail = dump_error(ctx, &message, &pg_detail, &sqlstate);

  /*
   * The exception can be QuickJS's "interrupted", thrown for a pending cancel
   * or termination -- a statement_timeout while a getter or toString() ran
   * for a conversion.  Raise the real error, as the call paths do, so it
   * keeps its SQLSTATE rather than becoming an internal error.
   */
  CHECK_FOR_INTERRUPTS();

  pljs_ereport_js_error(message, pg_detail, detail, sqlstate,
                        "could not convert a JavaScript value");
}

static int interrupt_handler(JSRuntime *rt, void *opaque) {
  /*
   * Return non-zero to make QuickJS abort the running script.  We interrupt on
   * a pending query cancel (statement_timeout, pg_cancel_backend) or backend
   * termination (pg_terminate_backend, fast shutdown), read straight from
   * PostgreSQL's own interrupt flags.
   *
   * We deliberately do NOT call CHECK_FOR_INTERRUPTS() here: that would
   * ereport(ERROR) / siglongjmp out of the middle of the QuickJS interpreter
   * and leave the runtime in an inconsistent state.  Instead we let QuickJS
   * unwind cleanly to a JS exception, and each caller runs
   * CHECK_FOR_INTERRUPTS() once control is back in C to raise the real error
   * (see the JS_IsException paths below).
   */
  /*
   * Only the flags that mean "this query must stop": a cancel, a backend
   * termination, or a lost client.  A recovery conflict sets one of the first
   * two itself.  InterruptPending is deliberately NOT consulted: PostgreSQL
   * sets it for many things that do not end the query -- a sinval catchup, a
   * ProcSignal barrier, pg_log_backend_memory_contexts(), a NOTIFY wake-up --
   * and once QuickJS has aborted the script there is no way back: the caller's
   * CHECK_FOR_INTERRUPTS() handles the benign interrupt and returns, and the
   * function then fails with "interrupted" for no reason.  With
   * InterruptPending in the condition, pg_object_keys_leak failed on every run
   * on PostgreSQL 18 and any long-running pljs function died when another
   * session called pg_log_backend_memory_contexts() on it.
   *
   * A call that has to end with an error stops too, should C code have caught
   * the exception that was to end it; see pljs_throw_fatal_error().
   */
  return (QueryCancelPending || ProcDiePending || ClientConnectionLost ||
          pljs_fatal_error() != NULL)
             ? 1
             : 0;
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

  /*
   * Record which pg_proc tuple this was compiled from.  CREATE OR REPLACE
   * writes a new tuple version, so a cached entry whose xmin/tid no longer
   * match the current tuple was compiled from a body that no longer exists.
   * These two fields were already declared in pljs_func but never written or
   * read.
   */
  pljs_function->fn_xmin = HeapTupleHeaderGetRawXmin(proctuple->t_data);
  pljs_function->fn_tid = proctuple->t_self;

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

    // We differentiate input arguments from output only.
    if (pljs_argmode_is_input(argmode)) {
      /*
       * Resolve polymorphic types, if this is an actual call context.  By the
       * input argument's place in the call, which has no OUT arguments; see
       * pljs_resolve_argtype().
       */
      if (fcinfo && IsPolymorphicType(argtype)) {
        argtype = get_fn_expr_argtype(fcinfo->flinfo, inargs);
      }

      /*
       * By its place in the call, as a window object's methods ask for one;
       * see pljs_window_arg_type().  It was kept by its place among all of
       * the arguments, and an OUT argument before it moved it.
       */
      pljs_function->argtypes[inargs] = argtype;
      inargs++;
    }

    pljs_function->argmodes[i] = argmode;
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
/* What pljs_run_with_storage() runs for setup_start_proc(). */
typedef struct pljs_start_proc_run {
  JSContext *ctx;
  JSValue func;
} pljs_start_proc_run;

/**
 * @brief Calls the pljs.start_proc; see setup_start_proc().
 *
 * @param arg #pljs_start_proc_run - the context and the function, which is
 * released
 */
static void run_start_proc(void *arg) {
  pljs_start_proc_run *run = (pljs_start_proc_run *)arg;
  JSValue ret = JS_Call(run->ctx, run->func, JS_UNDEFINED, 0, NULL);

  if (JS_IsException(ret)) {
    char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
    char *detail = dump_error(run->ctx, &message, &pg_detail, &sqlstate);

    /*
     * Release the JavaScript side before reporting: the report does not
     * return, so anything freed after it is never freed at all.
     */
    JS_FreeValue(run->ctx, ret);
    JS_FreeValue(run->ctx, run->func);

    /* Surface a pending cancel/terminate as the real PostgreSQL error. */
    CHECK_FOR_INTERRUPTS();

    pljs_ereport_js_error(message, pg_detail, detail, sqlstate,
                          "start proc execution error");
  }

  /*
   * The function reference and the call's result both belong to this
   * function. Neither was released, so every context creation with
   * pljs.start_proc set leaked both.
   */
  JS_FreeValue(run->ctx, ret);
  JS_FreeValue(run->ctx, run->func);
}

static void setup_start_proc(JSContext *ctx) {
  pljs_start_proc_run run = {.ctx = ctx, .func = JS_UNDEFINED};
  volatile Oid funcoid = InvalidOid;

  // Get a copy of the current memory context, we will need to switch to it in
  // case of an error.
  MemoryContext memory_context = CurrentMemoryContext;

  /*
   * A start_proc that cannot be found, or that the user may not run, is
   * warned of.  Only finding it: compiling it runs any code at the top level
   * of its source, and an error there -- a statement_timeout included --
   * was taken for a failure to find it, logged, and forgotten, and the
   * statement ran on past its timeout.  It raises as running it does.
   */
  PG_TRY();
  {
    // Check to see if we have permission to execute the startup procedure
    if (pljs_has_permission_to_execute(configuration.start_proc)) {
      if (strchr(configuration.start_proc, '(') == NULL) {
        funcoid = DatumGetObjectId(DirectFunctionCall1(
            regprocin, CStringGetDatum(configuration.start_proc)));
      } else {
        funcoid = DatumGetObjectId(DirectFunctionCall1(
            regprocedurein, CStringGetDatum(configuration.start_proc)));
      }
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

  if (OidIsValid(funcoid)) {
    run.func = pljs_find_js_function(funcoid, ctx);
  }

  if (JS_IsUndefined(run.func)) {
    elog(DEBUG3, "javascript function is not found for \"%s\"",
         configuration.start_proc);
    return;
  }

  /*
   * The start_proc runs with storage of its own, as a DO block does, with no
   * set to return and no window.  A context is created on the first call a
   * user makes, which can be a call made from inside another --
   * pljs.execute() after SET ROLE -- and the start_proc saw that call's
   * storage: its pljs.return_next() added rows to the other call's set.
   */
  pljs_run_with_storage(NULL, NULL, true, run_start_proc, &run);
}

/* What pljs_run_with_storage() runs for pljs_hook_call(). */
typedef struct pljs_hook_call_run {
  JSContext *ctx;
  JSValueConst func;
  int argc;
  JSValueConst *argv;
  MemoryContext caller_context;
  JSValue *result;
  char **error;
  bool ok;
} pljs_hook_call_run;

/**
 * @brief Calls the hook's function; see pljs_hook_call().
 *
 * @param arg #pljs_hook_call_run - the call, and where its result goes
 */
static void run_hook_call(void *arg) {
  pljs_hook_call_run *run = (pljs_hook_call_run *)arg;
  JSValue ret =
      JS_Call(run->ctx, run->func, JS_UNDEFINED, run->argc, run->argv);

  if (JS_IsException(ret)) {
    run->ok = false;

    /*
     * Extracted while the SPI connection is open, as call_function() does, and
     * into the caller's context, which outlives SPI_finish().
     */
    if (run->error != NULL) {
      MemoryContext old_context = MemoryContextSwitchTo(run->caller_context);

      *run->error = pljs_dump_error(run->ctx);
      MemoryContextSwitchTo(old_context);
    } else {
      JS_FreeValue(run->ctx, JS_GetException(run->ctx));
    }
  } else if (run->result != NULL) {
    *run->result = ret;
    return;
  }

  JS_FreeValue(run->ctx, ret);
}

/**
 * @brief Calls a JavaScript function from a PostgreSQL hook.
 *
 * A hook runs outside of any pljs call, so the JavaScript it calls gets what a
 * call would have given it: storage of its own, and a cache for converting
 * types, without which pljs.execute() could not convert its parameters or its
 * results.  The cache lasts for this call only.
 *
 * @param ctx #JSContext - the context the function belongs to
 * @param func #JSValueConst - the function
 * @param argc @c int - how many arguments there are
 * @param argv #JSValueConst* - the arguments
 * @param own_spi @c bool - whether to connect to SPI for the call
 * @param result #JSValue* - set to what the function returned, which the
 * caller frees, or NULL to discard it; left alone when the function throws
 * @param error @c char** - set to the error when the function throws, or NULL
 * to discard it
 * @returns @c bool - false when the function threw
 */
bool pljs_hook_call(JSContext *ctx, JSValueConst func, int argc,
                    JSValueConst *argv, bool own_spi, JSValue *result,
                    char **error) {
  pljs_hook_call_run run = {.ctx = ctx,
                            .func = func,
                            .argc = argc,
                            .argv = argv,
                            .caller_context = CurrentMemoryContext,
                            .result = result,
                            .error = error,
                            .ok = true};
  MemoryContext type_io_context = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS hook call", ALLOCSET_SMALL_SIZES);
  FmgrInfo flinfo;
  pljs_type_io_cache *types;

  memset(&flinfo, 0, sizeof(flinfo));
  flinfo.fn_oid = InvalidOid;
  flinfo.fn_mcxt = type_io_context;

  pljs_encoding_init();
  types = pljs_type_io_enter(&flinfo);

  PG_TRY();
  {
    pljs_run_with_storage(NULL, NULL, own_spi, run_hook_call, &run);
  }
  PG_FINALLY();
  {
    pljs_type_io_exit(types);
    MemoryContextDelete(type_io_context);
  }
  PG_END_TRY();

  return run.ok;
}

/**
 * @brief Creates the running user's JSContext, runs pljs.start_proc in it,
 * and caches it.
 *
 * A start_proc that raises -- one that throws, or times out, whether when it
 * is called or in code at the top level of its source -- leaves the context
 * uncached, and it is freed rather than lost: a function that failed because
 * of its user's start_proc created a context on every call, and kept none.
 *
 * @returns #JSContext - the new context
 */
JSContext *pljs_create_context(void) {
  /* Whose it is, before the start_proc can SET ROLE. */
  Oid user_id = GetUserId();
  JSContext *ctx = JS_NewContext(rt);

  if (ctx == NULL) {
    elog(ERROR, "could not create a JavaScript context");
  }

  PG_TRY();
  {
    // Set up the namespace, globals and functions available inside the
    // context.
    pljs_setup_namespace(ctx);

    // Check to see if there is a start_proc, if there is, attempt to apply
    // it.
    if (configuration.start_proc != NULL &&
        strlen(configuration.start_proc) != 0) {
      setup_start_proc(ctx);
    }

    /*
     * Save the context in the cache for this user id.  Inside the PG_TRY: a
     * start_proc that made a nested call as the same user cached a context of
     * its own, and adding this one raised, and lost it.
     */
    pljs_cache_context_add(user_id, ctx);
  }
  PG_CATCH();
  {
    JS_FreeContext(ctx);
    PG_RE_THROW();
  }
  PG_END_TRY();

  return ctx;
}

/**
 * @brief Whether an argument of a mode is an input argument, and so one of
 * the call's.
 *
 * @param argmode @c char - the argument's mode
 * @returns @c bool
 */
static bool pljs_argmode_is_input(char argmode) {
  return argmode == PROARGMODE_IN || argmode == PROARGMODE_INOUT ||
         argmode == PROARGMODE_VARIADIC;
}

/**
 * @brief Resolves a declared argument type against the call being made.
 *
 * Polymorphic types and `"any"` only name a concrete type at the call site.
 * `IsPolymorphicType()` covers the anyelement family but excludes `"any"`.
 *
 * @param fcinfo #FunctionCallInfo - the call in progress, may be NULL
 * @param argtype #Oid - the argument's declared type
 * @param argno @c int - which input argument: its place in the call, which
 * has no OUT arguments
 * @returns #Oid of the type the datum actually has
 */
static Oid pljs_resolve_argtype(FunctionCallInfo fcinfo, Oid argtype,
                                int argno) {
  if (fcinfo == NULL || !(IsPolymorphicType(argtype) || argtype == ANYOID)) {
    return argtype;
  }

  Oid resolved = get_fn_expr_argtype(fcinfo->flinfo, argno);

  return OidIsValid(resolved) ? resolved : argtype;
}

/**
 * @brief Converts all function call arguments from postgres to Javascript.
 *
 * Allocates and creates an array of arguments as Javascript values.
 *
 * @param fcinfo #FunctionCallInfo
 * @param proctuple #HeapTuple
 * @param context #pljs_context
 * @param argc @c int* - set to the number of values in the array, all of
 * which the caller owns
 * @returns an array of #JSValueConst values of the function arguments
 */
static JSValueConst *convert_arguments_to_javascript(FunctionCallInfo fcinfo,
                                                     HeapTuple proctuple,
                                                     pljs_context *context,
                                                     int *argc) {
  char **arguments;
  Oid *argtypes = NULL;
  char *argmodes;
  int nargs;

  nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);
  *argc = nargs;

  JSValueConst *argv = (JSValueConst *)palloc(sizeof(JSValueConst) * nargs);
  int inargs = 0;

  WindowObject window_obj = PG_WINDOW_OBJECT();

  /*
   * Every element is `undefined` until it is converted, so that when one of
   * them raises, the ones converted before it can be released: the caller
   * only frees the arguments once this has returned them, and the values
   * converted so far stayed in the runtime -- a 1MB text argument for every
   * call whose next argument was a multidimensional array.
   */
  for (int i = 0; i < nargs; i++) {
    argv[i] = JS_UNDEFINED;
  }

  PG_TRY();
  {
    /*
     * The input arguments, by their place in the call: a window's, or fcinfo's,
     * which have no OUT arguments.  They were read, and their polymorphic
     * types resolved, by their place among all of the arguments, and an OUT
     * argument before one moved it: f(OUT r int, a anyelement, b anyarray)
     * took a for an int4[], and read the int 5 it was passed as an array's
     * address, and a window function read past the end of its arguments.
     */
    bool is_window = WindowObjectIsValid(window_obj);

    for (int i = 0; i < nargs; i++) {
      Datum arg;
      bool is_null;

      if (!pljs_argmode_is_input(argmodes ? argmodes[i] : PROARGMODE_IN)) {
        continue;
      }

      if (is_window) {
        arg = WinGetFuncArgCurrent(window_obj, inargs, &is_null);
      } else {
        arg = fcinfo->args[inargs].value;
        is_null = fcinfo->args[inargs].isnull;
      }

      // Window functions: expand_composite=false (skip composite expansion),
      // regular functions: expand_composite=true (expand composite types)
      argv[inargs] = pljs_datum_to_jsvalue(
          pljs_resolve_argtype(fcinfo, argtypes[i], inargs), arg, is_null,
          !is_window, context->ctx);

      inargs++;
    }
  }
  PG_CATCH();
  {
    for (int i = 0; i < nargs; i++) {
      JS_FreeValue(context->ctx, argv[i]);
    }

    PG_RE_THROW();
  }
  PG_END_TRY();

  return argv;
}

/**
 * @brief Retrieves the #pljs_storage of the pljs call running now.
 *
 * The storage holds what the builtins need from the call that is running --
 * its FunctionCallInfo, the set it returns, its window -- and every entry
 * point that runs JavaScript installs its own and restores the previous one
 * however it leaves.
 *
 * It was kept as the opaque of the global `pljs` object, which JavaScript can
 * replace or delete: after `globalThis.pljs = undefined` the lookup returned
 * NULL, and the next call dereferenced it and crashed the backend, for any
 * user of the trusted language.  A function compiled in another user's
 * JSContext -- reached through pljs.find_function() after SET ROLE -- saw
 * that context's storage rather than the running call's.
 *
 * @returns #pljs_storage, or NULL outside of a pljs call
 */
pljs_storage *pljs_current_storage(void) { return current_storage; }

/**
 * @brief Fills in a #pljs_storage for a call.
 *
 * @param storage #pljs_storage - the storage to fill
 * @param function #pljs_func - the function being called, or NULL for a DO
 * block or a start_proc
 * @param fcinfo #FunctionCalInfo - the call, or NULL for a start_proc
 */
static void setup_storage(pljs_storage *storage, pljs_func *function,
                          FunctionCallInfo fcinfo) {
  WindowObject window_object = fcinfo != NULL ? PG_WINDOW_OBJECT() : NULL;

  memset(storage, 0, sizeof(pljs_storage));

  storage->function = function;
  storage->execution_memory_context = CurrentMemoryContext;
  storage->fcinfo = fcinfo;

  /* fcinfo->context is a TriggerData or CallContext for other calls. */
  if (WindowObjectIsValid(window_object)) {
    storage->window_object = window_object;
  }
}

/**
 * @brief Runs JavaScript with storage of its own.
 *
 * What every entry point into JavaScript does around it: a call, a DO block,
 * a pljs.start_proc, and code at the top level of a function's source, which
 * runs as it is compiled.  The storage is installed; the error the call has to
 * end with is raised, should the JavaScript run to its end all the same,
 * since C code can catch the exception that was to end it (see
 * pljs_throw_fatal_error()); and the storage that was installed before is
 * restored however it ends.  Each entry point had a copy of these steps, and
 * the copies had drifted apart.
 *
 * @param function #pljs_func - the function being called, or NULL
 * @param fcinfo #FunctionCallInfo - the call, or NULL
 * @param run - runs the JavaScript
 * @param arg - @p run's argument
 */
static void pljs_run_with_storage(pljs_func *function, FunctionCallInfo fcinfo,
                                  bool own_spi, void (*run)(void *),
                                  void *arg) {
  pljs_storage storage;
  pljs_storage *previous_storage = current_storage;
  volatile bool connected = false;

  setup_storage(&storage, function, fcinfo);
  current_storage = &storage;

  PG_TRY();
  {
    /*
     * Code with no call of its own -- a function's top-level code, a
     * start_proc -- gets a connection to SPI of its own, as a call's does, and
     * its stack is measured from here; see call_function().  It ran on
     * whatever connection was on top of the stack, a PL/pgSQL loop's
     * included, where pljs.execute() failed with "improper call to
     * spi_printtup", and against a stack anchored by some other call.  Atomic:
     * it cannot commit.
     */
    if (own_spi) {
      /*
       * Re-anchoring gives QuickJS its whole budget again from here, so the C
       * stack below has to be checked first: nothing else on the way checks
       * it when pljs.find_function() compiles a function from inside
       * JavaScript, and recursion through it re-anchored at every level until
       * the backend crashed.
       */
      check_stack_depth();

      if (SPI_connect() != SPI_OK_CONNECT) {
        elog(ERROR, "could not connect to spi manager");
      }

      connected = true;
      JS_UpdateStackTop(rt);
    }

    run(arg);

    if (connected) {
      connected = false;
      SPI_finish();
    }

    if (storage.fatal_error != NULL) {
      ReThrowError(storage.fatal_error);
    }
  }
  PG_FINALLY();
  {
    /*
     * The connection is finished on the way out with an error too.  Only
     * rolling back a subtransaction begun around this finished it otherwise,
     * and where none can be begun -- pljs.find_function() compiling a
     * function in parallel mode, before PostgreSQL 17 -- JavaScript that
     * caught the error went on with it on top of the SPI stack: the call's
     * own SPI_finish() finished it in place of the call's connection, and
     * the transaction ended with "transaction left non-empty SPI stack".
     * The error is being handled in ErrorContext, which is gone back to.
     */
    if (connected) {
      MemoryContext error_context = CurrentMemoryContext;

      SPI_finish();
      MemoryContextSwitchTo(error_context);
    }

    /*
     * The key of a set's column, kept for the call; see
     * pljs_single_column_value().  Read through current_storage, which is
     * this call's still, and not a local the PG_TRY wrote.
     */
    if (current_storage->return_state != NULL &&
        current_storage->return_state->column_atom != JS_ATOM_NULL) {
      JS_FreeAtomRT(rt, current_storage->return_state->column_atom);
      current_storage->return_state->column_atom = JS_ATOM_NULL;
    }

    current_storage = previous_storage;
  }
  PG_END_TRY();
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
  pljs_type_io_cache *types;
  Datum retval;

  pljs_encoding_init();
  types = pljs_type_io_enter(fcinfo->flinfo);

  PG_TRY();
  {
    retval = dispatch_call(fcinfo);
  }
  PG_FINALLY();
  {
    pljs_type_io_exit(types);
  }
  PG_END_TRY();

  return retval;
}

/**
 * @brief Calls a pljs function, procedure or trigger; see pljs_call_handler().
 *
 * @param fcinfo #FunctionCallInfo - the call
 * @returns #Datum of the result
 */
/* What pljs_run_with_storage() runs for dispatch_call(). */
typedef struct pljs_dispatch_run {
  FunctionCallInfo fcinfo;
  pljs_context *context;
  JSValueConst *argv;
  bool is_trigger;
  Datum retval;
} pljs_dispatch_run;

/**
 * @brief Calls a function, procedure or trigger; see dispatch_call().
 *
 * @param arg #pljs_dispatch_run - the call, whose retval is set
 */
static void dispatch_run(void *arg) {
  pljs_dispatch_run *run = (pljs_dispatch_run *)arg;

  if (run->is_trigger) {
    run->retval = call_trigger(run->fcinfo, run->context);
  } else if (run->context->function->is_srf) {
    run->retval = call_srf_function(run->fcinfo, run->context, run->argv);
  } else {
    run->retval = call_function(run->fcinfo, run->context, run->argv);
  }
}

static Datum dispatch_call(FunctionCallInfo fcinfo) {
  Oid fn_oid = fcinfo->flinfo->fn_oid;
  HeapTuple proctuple;
  JSContext *ctx;
  /* Read in the PG_FINALLY below. */
  JSValueConst *volatile argv = NULL;
  volatile int argc = 0;

  bool is_trigger = CALLED_AS_TRIGGER(fcinfo);
  pljs_context context = {0};

  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("cache lookup failed for function %u", fn_oid));
  }

  // First search for a cached copy of the context.
  pljs_function_cache_value *function_entry =
      pljs_cache_function_find(GetUserId(), fn_oid, proctuple);

  if (function_entry) {
    // Make a copy of the function entry to the pljs context.
    pljs_function_cache_to_context(&context, function_entry);
  } else {
    // Check to see if a context exists in the cache for this user.
    pljs_context_cache_value *entry = pljs_cache_context_find(GetUserId());

    if (entry) {
      ctx = entry->ctx;
    } else {
      ctx = pljs_create_context();
    }

    context.ctx = ctx;

    // Set up a copy of all of the function data.
    setup_function(fcinfo, proctuple, &context);

    // Compile the function.
    context.js_function = pljs_compile_function(&context, is_trigger);

    /*
     * The source evaluates to the function it declares, unless its top-level
     * code replaced that: `} f = undefined; function z() {`.  That returned
     * void, a zero Datum not marked NULL, whatever the function's type, and
     * the caller took it for a pointer to text and crashed the backend.
     */
    if (!JS_IsFunction(context.ctx, context.js_function)) {
      JS_FreeValue(context.ctx, context.js_function);
      ReleaseSysCache(proctuple);

      ereport(ERROR, (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
                      errmsg("the source of function %s does not evaluate to "
                             "a function",
                             context.function->proname)));
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
   * long-running backend).  Release it at the last safe point instead: right
   * after the final read, before any JS is executed.
   */
  if (is_trigger) {
    // Call in the context of a trigger.
    Form_pg_proc procStruct;

    procStruct = (Form_pg_proc)GETSTRUCT(proctuple);

    context.function->rettype = procStruct->prorettype;
  } else {
    // Call as a function.
    int nargs;

    argv = convert_arguments_to_javascript(fcinfo, proctuple, &context, &nargs);
    argc = nargs;
  }

  ReleaseSysCache(proctuple);

  pljs_dispatch_run run = {.fcinfo = fcinfo,
                           .context = &context,
                           .argv = argv,
                           .is_trigger = is_trigger};

  /*
   * The storage is restored even when the call raises; see
   * pljs_run_with_storage().
   *
   * A call that raised used to leave its own storage installed permanently --
   * pointing at this call's fcinfo, its return_state and its execution memory
   * context, all of which are gone once the error has unwound.  The next call
   * of *any* kind that reads pljs_current_storage() -- a trigger, a window
   * function, return_next() -- then dereferenced that stale pointer and
   * segfaulted the backend.
   *
   * This was latent until a conversion error became reachable from inside a
   * set-returning function: before that, returning an out-of-range integer
   * wrapped silently instead of raising, so nothing escaped this block.  A
   * failing SETOF call followed by any trigger reproduces it within a couple
   * of dozen iterations.
   *
   * The arguments are released here too.  JS_Call() takes references of its
   * own, so these were never freed at all, and every call leaked its
   * arguments into the runtime for the life of the backend -- 20MB over
   * 20,000 calls with a 1kB text argument, until pljs.memory_limit was
   * exhausted for good.
   */
  PG_TRY();
  {
    pljs_run_with_storage(context.function, fcinfo, false, dispatch_run, &run);
  }
  PG_FINALLY();
  {
    for (int i = 0; i < argc; i++) {
      JS_FreeValue(context.ctx, argv[i]);
    }
  }
  PG_END_TRY();

  return run.retval;
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
  pljs_type_io_cache *types;

  pljs_encoding_init();
  types = pljs_type_io_enter(fcinfo->flinfo);

  PG_TRY();
  {
    run_inline(fcinfo);
  }
  PG_FINALLY();
  {
    pljs_type_io_exit(types);
  }
  PG_END_TRY();

  PG_RETURN_VOID();
}

/**
 * @brief Runs a `DO` block; see pljs_inline_handler().
 *
 * @param fcinfo #FunctionCallInfo - the inline handler's call
 */
/* What pljs_run_with_storage() runs for run_inline(). */
typedef struct pljs_inline_run {
  const char *source;
  JSContext *ctx;
} pljs_inline_run;

/**
 * @brief Runs a DO block's code; see run_inline().
 *
 * @param arg #pljs_inline_run - the code and the context to run it in
 */
static void inline_run(void *arg) {
  pljs_inline_run *run = (pljs_inline_run *)arg;

  call_anonymous_function(run->source, run->ctx);
}

static void run_inline(FunctionCallInfo fcinfo) {
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
    ctx = pljs_create_context();
  }

  if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT) {
    elog(ERROR, "could not connect to spi manager");
  }

  /*
   * A DO block has storage of its own, with no set to return and no window:
   * it used to see whatever the call further up the stack had installed, so
   * pljs.return_next() from a DO block run by pljs.execute() added rows to
   * the set of the function that ran it.
   */
  pljs_inline_run run = {.source = sourcecode, .ctx = ctx};

  pljs_run_with_storage(NULL, fcinfo, false, inline_run, &run);

  SPI_finish();
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
   * A language validator is called as validator(oid_of_function_being_created),
   * so the function to check is the ARGUMENT.  This used to read
   * fcinfo->flinfo->fn_oid, which is the validator's *own* OID: it fetched the
   * validator's pg_proc row, whose prosrc is the C symbol name
   * "pljs_call_validator".  That happens to parse as a bare JavaScript
   * identifier, so validation always succeeded and an invalid body was accepted
   * silently --
   *
   *   CREATE FUNCTION f() RETURNS int AS $$ this is ( not js $$ LANGUAGE pljs;
   *   CREATE FUNCTION
   *
   * -- with the syntax error surfacing only on the first call.
   *
   * Correcting the OID is necessary but not sufficient: a pljs body is a
   * function *body*, not a program, so `return 42;` is a syntax error at top
   * level and validating the raw prosrc rejects almost every valid function. It
   * has to be wrapped the way compilation wraps it, which is why the source
   * builder is shared with pljs_compile_function().
   */
  Oid fn_oid = PG_GETARG_OID(0);
  HeapTuple proctuple;
  JSContext *ctx;
  pljs_context context = {0};
  StringInfoData src;
  bool is_trigger;

  if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, fn_oid)) {
    PG_RETURN_VOID();
  }

  pljs_encoding_init();

  /* check_function_bodies = off means "create it, do not compile it". */
  if (!check_function_bodies) {
    PG_RETURN_VOID();
  }

  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  is_trigger = ((Form_pg_proc)GETSTRUCT(proctuple))->prorettype == TRIGGEROID;

  /*
   * Fills in proname, prosrc and the argument names, which the wrapper needs.
   * Passing NULL fcinfo is the same thing pljs_find_js_function() does.
   *
   * Before the context is created: building the source converts it to UTF-8,
   * which can raise, and the context was lost when it did.
   */
  if (!setup_function(NULL, proctuple, &context)) {
    ReleaseSysCache(proctuple);
    PG_RETURN_VOID();
  }

  pljs_build_function_source(&src, &context, is_trigger);

  ReleaseSysCache(proctuple);

  ctx = JS_NewContext(rt);

  if (ctx == NULL) {
    elog(ERROR, "could not create a JavaScript context");
  }

  context.ctx = ctx;

  JSValue val = JS_Eval(ctx, src.data, strlen(src.data), "<function>",
                        JS_EVAL_FLAG_COMPILE_ONLY);

  pfree(src.data);

  if (JS_IsException(val)) {
    char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
    char *detail = dump_error(ctx, &message, &pg_detail, &sqlstate);

    /*
     * dump_error() has copied what we need into palloc'd memory, so release the
     * JavaScript side before reporting.  Without this a rejected body leaked
     * the whole context: JS_FreeContext() will not free one that still has live
     * references into it.
     */
    JS_FreeValue(ctx, val);
    JS_FreeContext(ctx);

    pljs_ereport_js_error(message, pg_detail, detail, sqlstate,
                          "invalid pljs function body");
  }

  JS_FreeValue(ctx, val);
  JS_FreeContext(ctx);

  /*
   * The function is being created or replaced, so drop the compiled copy cached
   * for it -- just that entry, in every user's cache.
   *
   * This was previously a blanket pljs_cache_reset(), which destroys every
   * per-user JSContext and rebuilds it on the next call.  JS_FreeContext() will
   * not free a context that still has live references into it, so the old one
   * was not necessarily reclaimed and a backend doing repeated DDL grew without
   * bound.
   */
  pljs_cache_function_remove(fn_oid);

  PG_RETURN_VOID();
}

/**
 * @brief Converts JavaScript source from the database's encoding to UTF-8.
 *
 * QuickJS reads its source as UTF-8, and a function's body, its name and its
 * arguments' names are in the database's encoding.  In a LATIN1 database a
 * string literal 'é' in a body became U+FFFD, and so did an accented letter
 * in a name.
 *
 * @param src #StringInfoData - the source, replaced by its conversion
 */
static void pljs_source_to_utf8(StringInfoData *src) {
  char *utf8 = pljs_server_to_utf8(src->data, src->len);

  if (utf8 != src->data) {
    pfree(src->data);
    src->data = utf8;
    src->len = strlen(utf8);
    src->maxlen = src->len + 1;
  }
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
/*
 * Build the JavaScript source for a pljs function: its body wrapped in a named
 * function with the declared argument names, followed by a reference to it so
 * the evaluation yields the function object.
 *
 * Extracted so that pljs_call_validator() can check exactly what
 * pljs_compile_function() will later compile.  A pljs body is a function
 * *body*, not a program -- `return 42;` is a syntax error at top level -- so
 * validating the raw prosrc rejects almost every valid function.  Sharing this
 * makes the two agree by construction rather than by two copies staying in
 * step.
 *
 * The returned StringInfo's data is palloc'd; the caller frees it.
 */
static void pljs_build_function_source(StringInfoData *src,
                                       pljs_context *context, bool is_trigger) {
  int i;

  initStringInfo(src);

  // generate the function as javascript with all of its arguments
  appendStringInfo(src, "function %s (", context->function->proname);

  int inarg = 0;
  for (i = 0; i < context->function->nargs; i++) {
    /*
     * Every argument but an OUT one.  A RETURNS TABLE column is a parameter
     * too, which the call leaves undefined: bodies assign to them, as to
     * variables of their own, and left out they became globals of the user's
     * context, or failed in strict mode.
     */
    if (context->function->argmodes[i] == PROARGMODE_OUT) {
      continue;
    }
    // commas between arguments
    if (inarg > 0) {
      appendStringInfoChar(src, ',');
    }

    // if this is a named argument, append it
    if (context->arguments[i]) {
      appendStringInfoString(src, context->arguments[i]);
    } else {
      // otherwise append it as an unnamed argument with a number
      appendStringInfo(src, "$%d", inarg + 1);
    }

    inarg++;
  }

  // append the other postgres-specific variables as well
  if (context->function->inargs && is_trigger) {
    appendStringInfo(src, ", ");
  }

  if (is_trigger) {
    appendStringInfo(src, "NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
                          "TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
  }

  appendStringInfo(src, ") {\n%s\n}\n %s;\n", context->function->prosrc,
                   context->function->proname);

  pljs_source_to_utf8(src);
}

/**
 * @brief Compiles a function, running any code at the top level of its source.
 *
 * A body can close the function it is wrapped in -- `} code(); function x() {`
 * -- and the code between runs as the source is compiled, before any call.  It
 * gets storage of its own, as a pljs.start_proc does, with no set to return
 * and no window.  It ran with none at all on a function's first call from a
 * query, where a failed commit of a subtransaction it started crashed the
 * backend, and with the storage of the call that compiled it on one from
 * inside another call, where its return_next() added rows to that call's set.
 *
 * @param context #pljs_context - the function
 * @param is_trigger @c bool - whether it is a trigger
 * @returns #JSValue of the compiled function
 */
/* What pljs_run_with_storage() runs for pljs_compile_function(). */
typedef struct pljs_compile_run {
  pljs_context *context;
  StringInfoData src;
  JSValue val;
} pljs_compile_run;

/**
 * @brief Compiles a function's source; see pljs_compile_function().
 *
 * @param arg #pljs_compile_run - the function and its source, which is freed;
 * its val is set
 */
static void compile_run(void *arg) {
  pljs_compile_run *run = (pljs_compile_run *)arg;
  JSContext *ctx = run->context->ctx;
  JSValue val =
      JS_Eval(ctx, run->src.data, strlen(run->src.data), "<function>", 0);

  pfree(run->src.data);

  if (JS_IsException(val)) {
    char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
    char *detail = dump_error(ctx, &message, &pg_detail, &sqlstate);

    /* Surface a pending cancel/terminate as the real PostgreSQL error. */
    CHECK_FOR_INTERRUPTS();

    pljs_ereport_js_error(message, pg_detail, detail, sqlstate,
                          "execution error");
  }

  /* Its call ends with an error all the same; see pljs_run_with_storage(). */
  if (current_storage->fatal_error != NULL) {
    JS_FreeValue(ctx, val);
    return;
  }

  run->val = val;
}

JSValue pljs_compile_function(pljs_context *context, bool is_trigger) {
  pljs_compile_run run = {.context = context, .val = JS_UNDEFINED};

  pljs_build_function_source(&run.src, context, is_trigger);
  pljs_run_with_storage(NULL, NULL, true, compile_run, &run);

  return run.val;
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
  pljs_source_to_utf8(&src);

  /*
   * Re-anchor QuickJS's stack measurement here, at the C-stack depth this call
   * actually starts from.  JS_NewRuntime() records the stack top once, inside
   * _PG_init, at whatever depth the first pljs call happened to be -- but JS
   * can run far deeper than that (SQL -> JS -> pljs.execute -> SQL -> JS ->
   * ...), so a budget measured from the original anchor does not describe the
   * stack this call has left.  The vendored QuickJS exports JS_UpdateStackTop()
   * for exactly this.
   */
  JS_UpdateStackTop(JS_GetRuntime(ctx));

  JSValue val = JS_Eval(ctx, src.data, strlen(src.data), "<function>", 0);

  if (!JS_IsException(val)) {
    JS_FreeValue(ctx, val);
    pfree(src.data);
  } else {
    /*
     * Extract the error, release everything, then report.  The report never
     * returns, so anything freed after it is dead code -- and `val` was never
     * released on this path at all, leaking a QuickJS reference for every
     * failed DO block.
     */
    char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
    char *detail = dump_error(ctx, &message, &pg_detail, &sqlstate);

    JS_FreeValue(ctx, val);
    pfree(src.data);

    /*
     * If QuickJS aborted because a cancel/terminate is pending, raise the real
     * PostgreSQL error (canceling statement / terminating connection) now that
     * we are safely back in C, instead of the generic JS interrupt message.
     * After the cleanup above, so a cancel cannot skip it.
     */
    CHECK_FOR_INTERRUPTS();

    pljs_ereport_js_error(message, pg_detail, detail, sqlstate,
                          "execution error");
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
  Datum result = (Datum)0;

  MemoryContext execution_context = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS Trigger Memory Context (call_trigger)",
      ALLOCSET_SMALL_SIZES);
  MemoryContext old_context = MemoryContextSwitchTo(execution_context);

  /*
   * Every argument is undefined until it is made, and all of them are released
   * if one cannot be: an OLD that did not convert, once NEW had, left NEW in
   * the runtime for every attempt.  Allocated rather than on the stack, so the
   * PG_CATCH reads what the PG_TRY wrote.
   */
  JSValueConst *argv = palloc(sizeof(JSValueConst) * 10);

  for (int i = 0; i < 10; i++) {
    argv[i] = JS_UNDEFINED;
  }

  if (TRIGGER_FIRED_FOR_ROW(event)) {
    result =
        PointerGetDatum(TRIGGER_FIRED_BY_UPDATE(event) ? trig->tg_newtuple
                                                       : trig->tg_trigtuple);
  }

  PG_TRY();
  {
    if (TRIGGER_FIRED_FOR_ROW(event)) {
      TupleDesc tupdesc = RelationGetDescr(rel);

      if (TRIGGER_FIRED_BY_INSERT(event)) {
        // NEW
        argv[0] =
            pljs_tuple_to_jsvalue(tupdesc, trig->tg_trigtuple, context->ctx);
      } else if (TRIGGER_FIRED_BY_DELETE(event)) {
        // OLD
        argv[1] =
            pljs_tuple_to_jsvalue(tupdesc, trig->tg_trigtuple, context->ctx);
      } else if (TRIGGER_FIRED_BY_UPDATE(event)) {
        // NEW
        argv[0] =
            pljs_tuple_to_jsvalue(tupdesc, trig->tg_newtuple, context->ctx);
        // OLD
        argv[1] =
            pljs_tuple_to_jsvalue(tupdesc, trig->tg_trigtuple, context->ctx);
      }
    }

    /*
     * Names and arguments in UTF-8, as QuickJS reads them; see
     * pljs_server_to_utf8().
     */
    const char *name = trig->tg_trigger->tgname;

    // 2: TG_NAME
    argv[2] = pljs_new_server_string(context->ctx, name, strlen(name));

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

    // 6: TG_RELID, unsigned: an OID of 2^31 or more was a negative number.
    argv[6] = JS_NewUint32(context->ctx, RelationGetRelid(rel));

    // 7: TG_TABLE_NAME
    name = RelationGetRelationName(rel);
    argv[7] = pljs_new_server_string(context->ctx, name, strlen(name));

    // 8: TG_TABLE_SCHEMA
    name = get_namespace_name(RelationGetNamespace(rel));
    argv[8] = pljs_new_server_string(context->ctx, name, strlen(name));

    // 9: TG_ARGV
    argv[9] = JS_NewArray(context->ctx);

    for (int i = 0; i < trig->tg_trigger->tgnargs; i++) {
      name = trig->tg_trigger->tgargs[i];

      /* Defined, not set; see pljs_datum_to_object(). */
      JS_DefinePropertyValueUint32(
          context->ctx, argv[9], i,
          pljs_new_server_string(context->ctx, name, strlen(name)),
          JS_PROP_C_W_E);
    }
  }
  PG_CATCH();
  {
    for (int i = 0; i < 10; i++) {
      JS_FreeValue(context->ctx, argv[i]);
    }

    PG_RE_THROW();
  }
  PG_END_TRY();

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

  /*
   * Re-anchor QuickJS's stack measurement here, at the C-stack depth this call
   * actually starts from.  JS_NewRuntime() records the stack top once, inside
   * _PG_init, at whatever depth the first pljs call happened to be -- but JS
   * can run far deeper than that (SQL -> JS -> pljs.execute -> SQL -> JS ->
   * ...), so a budget measured from the original anchor does not describe the
   * stack this call has left.  The vendored QuickJS exports JS_UpdateStackTop()
   * for exactly this.
   */
  JS_UpdateStackTop(JS_GetRuntime(context->ctx));

  JSValue ret =
      JS_Call(context->ctx, context->js_function, JS_UNDEFINED, 10, argv);

  /* JS_Call() took references of its own; see dispatch_call(). */
  for (int i = 0; i < 10; i++) {
    JS_FreeValue(context->ctx, argv[i]);
  }

  if (JS_IsException(ret)) {
    /*
     * Same order as call_function(): extract, release, then report.  The
     * JS_FreeValue() below used to sit *after* the report, which never returns,
     * so it was dead code -- `ret` leaked a QuickJS reference on every trigger
     * exception, not merely on a cancel.
     *
     * The error is extracted while the SPI connection is open, as the result
     * is converted; see call_function().
     */
    char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
    char *detail;

    MemoryContextSwitchTo(execution_context);
    detail = dump_error(context->ctx, &message, &pg_detail, &sqlstate);

    JS_FreeValue(context->ctx, ret);

    /* Before the report, which does not return. */
    SPI_finish();

    MemoryContextSwitchTo(old_context);

    /* Surface a pending cancel/terminate as the real PostgreSQL error. */
    CHECK_FOR_INTERRUPTS();

    pljs_ereport_js_error(message, pg_detail, detail, sqlstate,
                          "execution error");
  }

  /*
   * Convert NEW while the SPI connection is open, and release it however that
   * ends; see call_function().
   */
  MemoryContextSwitchTo(execution_context);

  PG_TRY();
  {
    pljs_raise_if_ending();

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
  }
  PG_FINALLY();
  {
    JS_FreeValue(context->ctx, ret);
  }
  PG_END_TRY();

  SPI_finish();

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

  /*
   * Re-anchor QuickJS's stack measurement here, at the C-stack depth this call
   * actually starts from.  JS_NewRuntime() records the stack top once, inside
   * _PG_init, at whatever depth the first pljs call happened to be -- but JS
   * can run far deeper than that (SQL -> JS -> pljs.execute -> SQL -> JS ->
   * ...), so a budget measured from the original anchor does not describe the
   * stack this call has left.  The vendored QuickJS exports JS_UpdateStackTop()
   * for exactly this.
   */
  JS_UpdateStackTop(JS_GetRuntime(context->ctx));

  JSValue ret = JS_Call(context->ctx, context->js_function, JS_UNDEFINED,
                        context->function->inargs, argv);

  if (JS_IsException(ret)) {
    char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
    char *error_message;

    /*
     * Extract the error while the SPI connection is still open, for the same
     * reason the result is converted then: dump_error() runs JavaScript -- the
     * thrown value's toString(), and getters for its message, detail and
     * sqlstate -- and pljs.commit() from there crashed the backend after
     * SPI_finish().  It is extracted into this call's context, which outlives
     * SPI_finish(), rather than SPI's.
     */
    MemoryContextSwitchTo(execution_context);
    error_message = dump_error(context->ctx, &message, &pg_detail, &sqlstate);

    JS_FreeValue(context->ctx, ret);

    /* Before the report, which does not return. */
    SPI_finish();

    /* Surface a pending cancel/terminate as the real PostgreSQL error. */
    CHECK_FOR_INTERRUPTS();

    pljs_ereport_js_error(message, pg_detail, error_message, sqlstate,
                          "execution error");

    /* Shuts up the compiler, since ereports of ERROR stop execution. */
    return (Datum)0;
  } else {
    Datum datum = 0;
    bool was_converting = pljs_converting_result;

    /*
     * Convert the result while this call's SPI connection is still open, as
     * PL/pgSQL does.  The conversion can run JavaScript -- a getter, or
     * toString(), on what the function returned -- and that can call
     * pljs.execute() or pljs.commit().  After SPI_finish() those ran with no
     * connection, where pljs.commit() crashed the backend, or on the caller's,
     * such as that of the PL/pgSQL function whose query called this one.
     *
     * The result has to outlive SPI_finish(), so it is built in this call's
     * own context rather than SPI's.  A procedure still cannot end its
     * transaction from here; see pljs_commit().
     *
     * The result is released however the conversion ends.  A result that
     * could not be converted -- one bad element of a large array -- stayed in
     * the runtime for the life of the backend, and a loop that retried the
     * call ran pljs.memory_limit out.
     */
    MemoryContextSwitchTo(execution_context);
    pljs_converting_result = true;

    PG_TRY();
    {
      datum = convert_result(fcinfo, context, rettype, ret);
    }
    PG_FINALLY();
    {
      pljs_converting_result = was_converting;
      JS_FreeValue(context->ctx, ret);
    }
    PG_END_TRY();

    SPI_finish();

    MemoryContextSwitchTo(old_context);

    return datum;
  }
}

/**
 * @brief Converts what a function returned to its result type.
 *
 * @param fcinfo #FunctionCallInfo - the call
 * @param context #pljs_context - the function
 * @param rettype #Oid - the result type
 * @param ret #JSValue - what the function returned
 * @returns #Datum of the result, with fcinfo->isnull set for a SQL NULL
 */
static Datum convert_result(FunctionCallInfo fcinfo, pljs_context *context,
                            Oid rettype, JSValue ret) {
  Datum datum;

  pljs_raise_if_ending();

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
    if (get_call_result_type(fcinfo, &rettype, &tupdesc) !=
        TYPEFUNC_COMPOSITE) {
      ereport(ERROR,
              (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
               errmsg("a function returning \"record\" needs a column "
                      "definition list"),
               errhint("Call it as ... AS (column_name data_type, ...).")));
    }

    pljs_type type;
    bool is_null = false;

    pljs_type_fill(&type, rettype);

    /*
     * pljs_jsvalue_to_record() reports a null or undefined result through
     * is_null, and was handed NULL for it: `return null` from a function
     * returning record, or with OUT parameters, crashed the backend.
     */
    datum = pljs_jsvalue_to_record(&type, ret, &is_null, tupdesc, context->ctx);

    if (is_null) {
      fcinfo->isnull = true;
    }
  } else {
    bool is_null;
    datum = pljs_jsvalue_to_datum(rettype, ret, &is_null, context->ctx, fcinfo);
  }

  return datum;
}

/**
 * @brief Puts a row that a set-returning function returned into its result.
 *
 * A row of a composite set is a `{column: value}` object.  A row of a
 * single-column set is its value, or an object naming it, as return_next()
 * accepts; see pljs_single_column_value().
 *
 * @param state #pljs_return_state - the set being returned
 * @param row #JSValueConst - the row
 * @param ctx #JSContext - Javascript context to execute in
 */
static void put_returned_row(pljs_return_state *state, JSValueConst row,
                             JSContext *ctx) {
  pljs_raise_if_ending();
  /* A getter on the returned array that threw. */
  if (JS_IsException(row)) {
    pljs_ereport_js_exception(ctx);
  }

  /*
   * A row of a composite set is an object naming every column, as
   * return_next() requires, which is checked as it is converted.  Anything
   * else but null was stored as a row of NULLs: `return [5, 'oops']`.
   */
  if (state->is_composite && !JS_IsNull(row) && !JS_IsUndefined(row) &&
      !JS_IsObject(row)) {
    ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("returned row must be an object")));
  }

  if (state->is_domain) {
    if (!JS_IsNull(row) && !JS_IsUndefined(row)) {
      pljs_put_domain_row(state, row, ctx, "returned row");
    } else {
      /*
       * A null row is left out, as it is for any other composite set -- but
       * only once the domain has allowed it, as return_next() checks one.  A
       * NOT NULL domain's set could return null rows and have them dropped
       * without a word.
       */
      bool is_null;

      pljs_jsvalue_to_datum(state->rettype, row, &is_null, ctx, NULL);
    }
  } else if (state->is_composite) {
    bool *nulls = (bool *)palloc0(sizeof(bool) * state->tuple_desc->natts);
    Datum *values = pljs_jsvalue_to_datums(NULL, row, &nulls, state->tuple_desc,
                                           ctx, "returned row");

    if (values != NULL) {
      tuplestore_putvalues(state->tuple_store_state, state->tuple_desc, values,
                           nulls);
      pfree(values);
    }

    pfree(nulls);
  } else {
    JSValue value = pljs_single_column_value(ctx, row, state, "returned row");
    bool is_null = false;
    Datum result;

    if (JS_IsException(value)) {
      pljs_ereport_js_exception(ctx);
    }

    result = pljs_jsvalue_to_datum_free(
        TupleDescAttr(state->tuple_desc, 0)->atttypid, value, &is_null, ctx);

    tuplestore_putvalues(state->tuple_store_state, state->tuple_desc, &result,
                         &is_null);
  }
}

/**
 * @brief Puts a row that a set-returning function returned, and releases it.
 *
 * The row is released however putting it ends.  It is a parameter here,
 * rather than a variable of the caller's loop, because a variable assigned in
 * a PG_TRY cannot be relied on in its PG_FINALLY even when it is volatile:
 * clang reads a volatile JSValue passed by value from the register it was
 * last in, and the row that failed was never released.
 *
 * @param state #pljs_return_state - the set being returned
 * @param row #JSValue - the row, an owned reference, which is released
 * @param ctx #JSContext - Javascript context to execute in
 */
static void put_returned_row_free(pljs_return_state *state, JSValue row,
                                  JSContext *ctx) {
  PG_TRY();
  {
    put_returned_row(state, row, ctx);
  }
  PG_FINALLY();
  {
    JS_FreeValue(ctx, row);
  }
  PG_END_TRY();
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

  MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

  TypeFuncClass typeclass;

  /*
   * Only the set itself, and its descriptor, have to outlive the call.  The
   * state was kept with them, for as long as the query ran: a set-returning
   * function called once for each row of a LATERAL join left one for every
   * row.
   */
  state = (pljs_return_state *)MemoryContextAlloc(execution_context,
                                                  sizeof(pljs_return_state));

  /*
   * The result type is resolved for this call rather than taken from the
   * function's cache entry.  A polymorphic function -- SETOF anyelement --
   * returns a different type at each call site, and the class its first call
   * resolved to was kept for every later one: after f(1), f(ROW(1, 2)::pair)
   * was put as a single column, and the tuplestore read the second off the
   * stack.  The check for a record read its return type from the cache entry
   * too, which never copied it, so every cached call compared uninitialized
   * memory.
   *
   * No descriptor is asked for, which would be a copy of the row type made in
   * the query's memory for every call; the set's is rsinfo's.
   */
  typeclass = get_call_result_type(fcinfo, &state->rettype, NULL);

  /* A record with no column definition list to say what its columns are. */
  if (typeclass == TYPEFUNC_RECORD) {
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("function returning record called in context "
                           "that cannot accept type record")));
  }

  state->tuple_store_state = tuplestore_begin_heap(true, false, work_mem);

  if (!rsinfo->setDesc) {
    state->tuple_desc = CreateTupleDescCopy(rsinfo->expectedDesc);
    rsinfo->setDesc = state->tuple_desc;
  } else {
    state->tuple_desc = rsinfo->setDesc;
  }

  /*
   * A set of a domain over a composite type has rows of the composite type,
   * each of which has to satisfy the domain.  TYPEFUNC_COMPOSITE_DOMAIN was
   * taken for a single-column set, so its row objects had their first
   * property converted, and that one Datum was passed as a row of all of the
   * composite type's columns: tuplestore_putvalues() read the rest off the
   * stack, and the backend was killed.  The domain's constraints were never
   * checked either.
   */
  state->is_domain = typeclass == TYPEFUNC_COMPOSITE_DOMAIN;
  state->is_composite = typeclass == TYPEFUNC_COMPOSITE || state->is_domain;
  state->row_context =
      state->is_domain
          ? AllocSetContextCreate(execution_context, "PLJS Domain Row",
                                  ALLOCSET_SMALL_SIZES)
          : NULL;
  state->row_context_busy = false;
  state->domain_map_known = false;
  state->domain_rowtype_id = 0;
  state->domain_context = NULL;
  state->domain_rowtype = NULL;
  state->domain_map = NULL;

  /*
   * A single-column set's column is named when a row object first needs it;
   * see pljs_single_column_value().
   */
  state->fn_oid = fcinfo->flinfo->fn_oid;
  state->column_known = false;
  state->column_name = NULL;
  state->column_atom = JS_ATOM_NULL;
  state->column_type_known = false;
  state->column_takes_objects = false;

  /*
   * Whether converting a row can run a domain's CHECK constraint, which can
   * run any SQL, is worked out by pljs_return_next() when it is first called.
   */
  state->convert_in_subtransaction = false;
  state->convert_known = false;
  state->convert_generation = 0;

  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = state->tuple_store_state;

  MemoryContextSwitchTo(execution_context);

  // Set the current return context.
  pljs_current_storage()->return_state = state;

  /*
   * Re-anchor QuickJS's stack measurement here, at the C-stack depth this call
   * actually starts from.  JS_NewRuntime() records the stack top once, inside
   * _PG_init, at whatever depth the first pljs call happened to be -- but JS
   * can run far deeper than that (SQL -> JS -> pljs.execute -> SQL -> JS ->
   * ...), so a budget measured from the original anchor does not describe the
   * stack this call has left.  The vendored QuickJS exports JS_UpdateStackTop()
   * for exactly this.
   */
  JS_UpdateStackTop(JS_GetRuntime(context->ctx));

  JSValue ret = JS_Call(context->ctx, context->js_function, JS_UNDEFINED,
                        context->function->inargs, argv);

  if (JS_IsException(ret)) {
    char *message = NULL, *pg_detail = NULL, *sqlstate = NULL;
    char *error_message;

    /* While the SPI connection is open; see call_function(). */
    MemoryContextSwitchTo(execution_context);
    error_message = dump_error(context->ctx, &message, &pg_detail, &sqlstate);

    JS_FreeValue(context->ctx, ret);

    /* Before the report, which does not return. */
    SPI_finish();

    /* Surface a pending cancel/terminate as the real PostgreSQL error. */
    CHECK_FOR_INTERRUPTS();

    pljs_ereport_js_error(message, pg_detail, error_message, sqlstate,
                          "execution error");

    /* Shuts up the compiler, since ereports of ERROR stop execution. */
    return (Datum)0;
  } else {
    /*
     * Rows the function returned rather than passed to return_next() are
     * converted while the SPI connection is open, and released however that
     * ends; see call_function().
     */
    PG_TRY();
    {
      if (!JS_IsUndefined(ret) && !JS_IsNull(ret)) {
        /*
         * In this call's memory, as return_next() converts a row: the
         * tuplestore copies each row into its own.  They were converted in
         * the query's, where what converting them allocated stayed for as long
         * as the query ran -- for every call of a LATERAL join's.
         */
        MemoryContextSwitchTo(execution_context);

        // JS can return a single row or an array of rows.
        if (JS_IsArray(context->ctx, ret)) {
          int32_t length = pljs_js_array_length(ret, context->ctx);

          if (length < 0) {
            pljs_ereport_js_exception(context->ctx);
          }

          for (int32_t i = 0; i < length; i++) {
            put_returned_row_free(state,
                                  JS_GetPropertyUint32(context->ctx, ret, i),
                                  context->ctx);
          }
        } else {
          put_returned_row(state, ret, context->ctx);
        }

        MemoryContextSwitchTo(execution_context);
      }
    }
    PG_FINALLY();
    {
      JS_FreeValue(context->ctx, ret);
    }
    PG_END_TRY();
  }

  SPI_finish();

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
  ErrorData *fatal_error = pljs_fatal_error();

  /* Nothing JavaScript can catch; see pljs_throw_fatal_error(). */
  if (fatal_error != NULL) {
    return js_throw_uncatchable(fatal_error, ctx);
  }

  JSValue error = JS_NewError(ctx);

  /*
   * A message can name a column or a type, in the database's encoding, and
   * QuickJS reads UTF-8; see pljs_server_to_utf8().  So for every message
   * PostgreSQL hands JavaScript.
   */
  JSValue message_value =
      pljs_new_message_string(ctx, message, strlen(message));
  JS_SetPropertyStr(ctx, error, "message", message_value);

  return JS_Throw(ctx, error);
}

/*
 * Makes a JavaScript string of part of a PostgreSQL error, which is in the
 * database's encoding; see js_throw().  NULL is the empty string.
 */
static JSValue js_error_string(JSContext *ctx, const char *text) {
  if (text == NULL) {
    text = "";
  }

  return pljs_new_message_string(ctx, text, strlen(text));
}

/*
 * Throws an exception that no catch or finally block sees, for the error the
 * running call has to end with; see pljs_throw_fatal_error().
 */
static JSValue js_throw_uncatchable(ErrorData *edata, JSContext *ctx) {
  JSValue error = JS_NewError(ctx);

  JS_SetPropertyStr(ctx, error, "message",
                    js_error_string(ctx, edata->message));
  JS_SetUncatchableError(ctx, error, true);

  return JS_Throw(ctx, error);
}

/**
 * @brief Ends the running call with the PostgreSQL error being handled.
 *
 * For a PG_CATCH whose error JavaScript must not catch, because what raised it
 * left behind state that only ending the query cleans up.  A window object's
 * methods run the executor of the query that called the window function -- to
 * read its partition, and to evaluate its arguments, which can call any
 * function -- and a function that raised there left its SPI connection on the
 * stack.  JavaScript caught the error and carried on with that: "transaction
 * left non-empty SPI stack", and a result for one row computed without the
 * row that raised.  Nothing can roll the query back to a savepoint, and
 * re-throwing the error from a function QuickJS called unwinds past its live
 * frames; see pljs_return_next().
 *
 * So the error is kept in the call's storage, and JavaScript gets an exception
 * that no catch or finally block sees.  QuickJS unwinds to the call handler,
 * which raises the kept error in place of the exception; see
 * pljs_ereport_js_error().  Until then every exception pljs throws is
 * uncatchable as well, since C code can replace the pending exception with one
 * of its own, and a call that returns all the same raises the kept error; see
 * dispatch_call().
 *
 * @param ctx #JSContext - Javascript context
 * @returns #JSValue - JS_EXCEPTION
 */
JSValue pljs_throw_fatal_error(JSContext *ctx) {
  MemoryContext old_context;
  ErrorData *edata;

  /*
   * JavaScript only runs with storage installed, top-level code as its
   * function is compiled included; see pljs_compile_function().
   */
  Assert(current_storage != NULL);

  old_context =
      MemoryContextSwitchTo(current_storage->execution_memory_context);
  edata = CopyErrorData();

  FlushErrorState();
  MemoryContextSwitchTo(old_context);

  /* The first error is the one the call ends with. */
  if (current_storage->fatal_error == NULL) {
    current_storage->fatal_error = edata;
  } else {
    FreeErrorData(edata);
  }

  return js_throw_uncatchable(current_storage->fatal_error, ctx);
}

/*
 * Ends the running call with a copy of an error that has been caught and
 * flushed; see pljs_throw_fatal_error().  ReThrowError() puts the copy back
 * where pljs_throw_fatal_error() takes the error being handled from.
 */
JSValue pljs_throw_fatal_error_data(ErrorData *edata, JSContext *ctx) {
  MemoryContext old_context = CurrentMemoryContext;

  PG_TRY();
  {
    ReThrowError(edata);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(old_context);

    return pljs_throw_fatal_error(ctx);
  }
  PG_END_TRY();

  pg_unreachable();
}

/**
 * @brief Raises the error the running call has to end with, if it has one.
 *
 * Before what JavaScript returned is converted.  JavaScript can run on after
 * the exception that was to end the call -- a Promise's executor turns it
 * into a rejection -- and return all the same, and an error converting what
 * it returned replaced the one the call had to end with: a timeout came back
 * as "returned row must be an object".
 */
static void pljs_raise_if_ending(void) {
  ErrorData *fatal_error = pljs_fatal_error();

  if (fatal_error != NULL) {
    ReThrowError(fatal_error);
  }
}

/**
 * @brief Whether the running call has to end with an error.
 *
 * Only an exception that is thrown cannot be caught, and JavaScript runs on
 * until one is: a Promise's executor turns the exception that was to end the
 * call into a rejection, and the code after it ran -- its pljs.execute(),
 * whose nextval() or advisory lock outlived the call that failed, and its
 * pljs.commit(), which committed that call's work.  So every builtin that
 * runs SQL refuses once the call has to end with an error, with
 * pljs_throw_ending().  See pljs_throw_fatal_error().
 *
 * @returns @c bool
 */
bool pljs_call_is_ending(void) { return pljs_fatal_error() != NULL; }

/**
 * @brief Throws the error the running call has to end with; see
 * pljs_call_is_ending().
 *
 * @param ctx #JSContext - Javascript context
 * @returns #JSValue - JS_EXCEPTION
 */
JSValue pljs_throw_ending(JSContext *ctx) {
  return js_throw_uncatchable(pljs_fatal_error(), ctx);
}

/*
 * Like js_throw(), but also attaches the Postgres error's detail, hint and
 * SQLSTATE to the JS error object (matching plv8).  Used when a Postgres
 * error caught during SPI execution is surfaced to JavaScript, so the full
 * error envelope survives into JS and can be re-raised faithfully across a
 * nested pljs.execute() boundary instead of collapsing to the message alone.
 */
JSValue js_throw_error_data(ErrorData *edata, JSContext *ctx) {
  ErrorData *fatal_error = pljs_fatal_error();

  /*
   * Nothing JavaScript can catch, whatever the error: pljs.execute() or
   * return_next() that caught the kept error itself, raised on the way out of
   * a conversion; see pljs_throw_fatal_error().
   */
  if (fatal_error != NULL) {
    return js_throw_uncatchable(fatal_error, ctx);
  }

  /*
   * A cancel -- statement_timeout, pg_cancel_backend() -- ends the call.  By
   * the time it is raised it is no longer pending, and nothing interrupts
   * JavaScript that caught it again: a function that caught a timeout from
   * pljs.execute() and went on looping ran for ever.
   */
  if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED && current_storage != NULL) {
    return pljs_throw_fatal_error_data(edata, ctx);
  }

  JSValue error = JS_NewError(ctx);

  JS_SetPropertyStr(ctx, error, "message",
                    js_error_string(ctx, edata->message));

  if (edata->detail) {
    JS_SetPropertyStr(ctx, error, "detail",
                      js_error_string(ctx, edata->detail));
  }

  if (edata->hint) {
    JS_SetPropertyStr(ctx, error, "hint", js_error_string(ctx, edata->hint));
  }

  /*
   * Expose the SQLSTATE under both names.
   *
   * The value has always been the five-character string rather than the packed
   * integer -- unpack_sql_state() is applied here -- but the property was named
   * `sqlerrcode`, which is PostgreSQL's internal name for the *packed* form.
   * Every other PL calls it `sqlstate`, and that is the name a JavaScript
   * author reaches for:
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
 * @brief Compiles a function pljs_find_js_function() found, in a
 * subtransaction.
 *
 * Compiling runs any code at the top level of the source, which can hold
 * anything -- a catalog pin, a relation lock -- when it raises, and only
 * rolling back releases it.  pljs.find_function() catches the error, and left
 * what it held, which COMMIT warned of.  Only compiling needs one: a function
 * already compiled is found without.  Where none can be begun, it compiles
 * without one, as it did before; see pljs_subxact_begin().
 *
 * @param context #pljs_context - the function
 * @returns #JSValue of the compiled function
 */
static JSValue pljs_compile_found_function(pljs_context *context) {
  pljs_subxact sx;
  JSValue func = JS_UNDEFINED;
  ErrorData *edata;

  pljs_subxact_init(&sx);

  PG_TRY();
  {
    pljs_subxact_begin(&sx, true);

    func = pljs_compile_function(context, false);
  }
  PG_CATCH();
  {
    ReThrowError(pljs_subxact_abort(&sx));
  }
  PG_END_TRY();

  edata = pljs_subxact_commit(&sx);

  if (edata != NULL) {
    JS_FreeValue(context->ctx, func);
    ReThrowError(edata);
  }

  return func;
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
     * an `Oid oid` at the same offset -- any future field access, or a change
     * to either catalog's layout, would have read the wrong bytes.
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
      pljs_cache_function_find(GetUserId(), fn_oid, functuple);

  if (function_entry != NULL) {
    pljs_function_cache_to_context(&context, function_entry);

    /*
     * Hand out a reference we own.  pljs_function_cache_to_context() borrows
     * the cache's, and the cache entry is that value's only owner -- but a
     * JSValue returned from a C function belongs to its caller, so
     * pljs.find_function() handed JavaScript a reference it had not counted.
     * The engine dropped it when the JS variable died, and after enough lookups
     * the refcount reached zero while the entry was still cached, leaving the
     * cache holding a freed object.  The next call through it terminated the
     * backend.
     */
    func = JS_DupValue(context.ctx, context.js_function);

    /*
     * The pin was previously released only on the cache-miss branch below, so a
     * pljs.find_function() that hit the cache -- the common case once a
     * function has been called once -- held a syscache pin on pg_proc for the
     * rest of the transaction.  Repeated lookups in one transaction accumulated
     * them, which is what produces "WARNING: resource was not closed: cache
     * pg_proc ... has count N" under USE_ASSERT_CHECKING.
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

    /*
     * Released before compiling, which runs any code at the top level of the
     * source and can raise: pljs.find_function() catches that, and the pin
     * was left, which COMMIT warned of.
     */
    ReleaseSysCache(functuple);

    func = pljs_compile_found_function(&context);
  }

  // What is not a function is not found; see dispatch_call().
  if (!JS_IsFunction(context.ctx, func)) {
    JS_FreeValue(context.ctx, func);
    return JS_UNDEFINED;
  }

  return func;
}
