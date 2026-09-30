#include "deps/quickjs/list.h"
#include "deps/quickjs/quickjs.h"

#include "postgres.h"

#include "access/tupconvert.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "nodes/params.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "utils/typcache.h"
#include "windowapi.h"

#include "pljs.h"

#include <math.h>

// Local only functions for injecting into pljs
static JSValue pljs_elog(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_execute(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_prepare(JSContext *, JSValueConst, int, JSValueConst *);
static JSValue pljs_window_caught_error(JSContext *ctx, MemoryContext mcontext);
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
static JSValue pljs_return_next_internal(JSContext *, pljs_return_state *,
                                         JSValueConst);

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
static JSValue pljs_require(JSContext *, JSValueConst, int, JSValueConst *);

// Set up any stored procedures we export to Postgres.
PGDLLEXPORT Datum pljs_version(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pljs_info(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum pljs_reset(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pljs_version);
PG_FUNCTION_INFO_V1(pljs_info);
PG_FUNCTION_INFO_V1(pljs_reset);

/**
 * @brief Takes the PostgreSQL error being handled off the error stack.
 *
 * The first thing a builtin's PG_CATCH does.  The error is copied into
 * @p mcontext -- errfinish() leaves ErrorContext current, and a copy made
 * there is freed under the caller -- and flushed, since PG_CATCH() does not pop
 * the errordata stack, and the sixth error left on it PANICs the cluster.
 *
 * Only then is a failed subtransaction rolled back, and the error thrown to
 * JavaScript with pljs_throw_caught_error().  Throwing can raise the error
 * again, for a cancel, and did so on top of the unflushed error, inside the
 * subtransaction that had failed, where some builtins threw it first.
 *
 * @param mcontext #MemoryContext - the builtin's own, to copy the error into
 * @returns #ErrorData - the error, for pljs_throw_caught_error()
 */
static ErrorData *pljs_catch_error(MemoryContext mcontext) {
  ErrorData *edata;

  MemoryContextSwitchTo(mcontext);
  edata = CopyErrorData();
  FlushErrorState();

  return edata;
}

/**
 * @brief Throws an error pljs_catch_error() took to JavaScript, and frees it.
 *
 * The last thing a builtin's PG_CATCH does; see pljs_catch_error().  A window
 * object's methods that leave nothing behind when they raise use it too; one
 * that runs the executor ends the call instead; see
 * pljs_window_get_func_arg().
 *
 * @param edata #ErrorData - what pljs_catch_error() returned
 * @param ctx #JSContext - Javascript context
 * @returns #JSValue - JS_EXCEPTION
 */
static JSValue pljs_throw_caught_error(ErrorData *edata, JSContext *ctx) {
  JSValue error = js_throw_error_data(edata, ctx);

  FreeErrorData(edata);

  return error;
}

/**
 * @brief Whether an internal subtransaction can be begun.
 *
 * Not in parallel mode before PostgreSQL 17, which refuses one there.
 *
 * @returns @c bool
 */
static bool pljs_can_begin_subtransaction(void) {
#if PG_VERSION_NUM >= 170000
  return true;
#else
  return !IsInParallelMode();
#endif
}

/**
 * @brief Sets up an internal subtransaction, before the PG_TRY that begins it.
 *
 * It notes the memory context and resource owner that are current, which
 * are restored however the subtransaction ends.
 *
 * @param sx #pljs_subxact - the subtransaction
 */
void pljs_subxact_init(pljs_subxact *sx) {
  sx->mcontext = CurrentMemoryContext;
  sx->resowner = CurrentResourceOwner;
  sx->begun = false;
}

/**
 * @brief Begins an internal subtransaction for a builtin's work, in its
 * PG_TRY.
 *
 * Catching an error is safe only once whatever raised it has let go of what
 * it held -- the portal and snapshot of a query that failed, a catalog pin,
 * a relation lock, a nested function's SPI connection -- and only rolling
 * back to a subtransaction releases those.  So every builtin that runs SQL,
 * or can, does its work in one: pljs_subxact_abort() rolls it back in the
 * PG_CATCH, and pljs_subxact_commit() commits it after PG_END_TRY.
 *
 * Every builtin had its own copy of these steps, and the copies had drifted
 * apart.  Some rolled back a subtransaction they had failed to begin -- the
 * caller's, or at the top level none, which is FATAL -- and some began one
 * where PostgreSQL before 17 refuses to, in parallel mode, so a PARALLEL SAFE
 * set-returning function failed in return_next().
 *
 * @param sx #pljs_subxact - the subtransaction, set up by pljs_subxact_init()
 * @param optional @c bool - whether the work can be done without one where
 * none can be begun; otherwise beginning it raises there
 */
void pljs_subxact_begin(pljs_subxact *sx, bool optional) {
  if (optional && !pljs_can_begin_subtransaction()) {
    return;
  }

  BeginInternalSubTransaction(NULL);
  sx->begun = true;
  MemoryContextSwitchTo(sx->mcontext);
}

/**
 * @brief Rolls back the internal subtransaction if one was begun, and
 * restores the memory context and resource owner.
 *
 * For a builtin that rolls back without an error to report --
 * pljs.subtransaction(), whose callback threw -- or that ends the call with
 * the error being handled.  Rolling back can raise; the subtransaction counts
 * as ended all the same, so calling this again only restores.
 *
 * @param sx #pljs_subxact - the subtransaction
 */
void pljs_subxact_rollback(pljs_subxact *sx) {
  if (sx->begun) {
    sx->begun = false;
    RollbackAndReleaseCurrentSubTransaction();
  }

  MemoryContextSwitchTo(sx->mcontext);
  CurrentResourceOwner = sx->resowner;
}

/**
 * @brief Takes the error off the stack, and rolls back the internal
 * subtransaction if one was begun; for the PG_CATCH of the PG_TRY that
 * began it.
 *
 * The error is copied and flushed first; see pljs_catch_error().  Then the
 * subtransaction is rolled back, as PL/pgSQL rolls back a block's; see
 * pljs_subxact_rollback().
 *
 * @param sx #pljs_subxact - the subtransaction
 * @returns #ErrorData - the error, for pljs_throw_caught_error()
 */
ErrorData *pljs_subxact_abort(pljs_subxact *sx) {
  ErrorData *edata = pljs_catch_error(sx->mcontext);

  pljs_subxact_rollback(sx);

  return edata;
}

/**
 * @brief Commits the internal subtransaction, if one was begun; after the
 * PG_END_TRY of the PG_TRY that began it.
 *
 * Committing can raise -- a subtransaction callback, a foreign server's
 * RELEASE SAVEPOINT, running out of memory -- and a subtransaction that did
 * not commit is rolled back, as PL/pgSQL rolls back a block's.  Ending the
 * call instead left it half committed and current: the session's next
 * ROLLBACK was FATAL, and a PL/pgSQL block that caught the error rolled back
 * pljs's subtransaction in place of its own.
 *
 * @param sx #pljs_subxact - the subtransaction
 * @returns #ErrorData - NULL once committed; otherwise the error, rolled
 * back, for the builtin to release what it holds and throw it with
 * pljs_throw_caught_error()
 */
ErrorData *pljs_subxact_commit(pljs_subxact *sx) {
  ErrorData *edata = NULL;

  if (sx->begun) {
    PG_TRY();
    {
      ReleaseCurrentSubTransaction();
      sx->begun = false;
    }
    PG_CATCH();
    {
      edata = pljs_subxact_abort(sx);
    }
    PG_END_TRY();
  }

  MemoryContextSwitchTo(sx->mcontext);
  CurrentResourceOwner = sx->resowner;

  return edata;
}

/**
 * @brief Rolls back the internal subtransaction, after the builtin's
 * JavaScript has run, if the call has to end.
 *
 * Something the JavaScript called -- a nested return_next(), say -- ended the
 * call with what its error held left inside this subtransaction, and
 * committing kept it: "resource was not closed", and "subtransaction left
 * non-empty SPI stack".  A JavaScript exception that does not end the call
 * leaves nothing: every builtin cleans up after itself.  A rollback that
 * fails ends the call with its own error.
 *
 * @param sx #pljs_subxact - the subtransaction
 * @param ctx #JSContext - Javascript context
 * @returns @c bool - true with the call ending, and the subtransaction rolled
 * back; false to commit it
 */
static bool pljs_subxact_ending(pljs_subxact *sx, JSContext *ctx) {
  if (!pljs_call_is_ending()) {
    return false;
  }

  PG_TRY();
  {
    pljs_subxact_rollback(sx);
  }
  PG_CATCH();
  {
    pljs_subxact_rollback(sx);
    pljs_throw_fatal_error(ctx);
    return true;
  }
  PG_END_TRY();

  pljs_throw_ending(ctx);

  return true;
}

/**
 * @brief Returns the text of a string argument, in the database's encoding.
 *
 * SQL text, a type name, a function's signature: QuickJS writes UTF-8, and
 * these went to PostgreSQL as QuickJS wrote them, while the values beside them
 * were converted.  In a LATIN1 database a value read from a query and written
 * back into SQL text no longer equalled itself.  Text the database's encoding
 * cannot hold -- a lone surrogate, or a character it has no equivalent for --
 * is thrown to JavaScript, as PostgreSQL raises it.
 *
 * @param ctx #JSContext - Javascript context
 * @param value #JSValueConst - the argument
 * @returns @c char* - palloc'd, or NULL with an exception pending
 */
static char *pljs_string_arg(JSContext *ctx, JSValueConst value) {
  MemoryContext m_mcontext = CurrentMemoryContext;
  size_t length;
  const char *str = JS_ToCStringLen(ctx, &length, value);
  char *volatile text = NULL;

  if (str == NULL) {
    return NULL;
  }

  PG_TRY();
  {
    char *server = pljs_utf8_to_server(str, length);

    text = server == str ? pnstrdup(str, length) : server;
  }
  PG_CATCH();
  {
    JS_FreeCString(ctx, str);

    pljs_throw_caught_error(pljs_catch_error(m_mcontext), ctx);
    return NULL;
  }
  PG_END_TRY();

  JS_FreeCString(ctx, str);

  return text;
}

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
  // Before any JavaScript runs in the context.
  pljs_type_classes_init(ctx);

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

  JS_SetPropertyStr(ctx, pljs, "require",
                    JS_NewCFunction(ctx, pljs_require, "require", 1));

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

  /*
   * JS_GetGlobalObject() hands back a reference of its own, and keeping it
   * kept the context: one a start_proc failed in could not be freed.
   */
  JS_FreeValue(ctx, global_obj);
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

      JS_FreeValue(ctx, str);

      /* A toString() that threw; its NULL was formatted as "(null)". */
      if (cstr == NULL) {
        pfree(msg.data);
        return JS_EXCEPTION;
      }

      appendStringInfo(&msg, "%s", cstr);

      JS_FreeCString(ctx, cstr);
    }

    MemoryContext m_mcontext = CurrentMemoryContext;

    /* ERROR case. */
    PG_TRY();
    {
      /*
       * Only a message that goes somewhere is converted: to the database's
       * encoding, never raising for a character it cannot hold, which failed
       * a NOTICE's function; see pljs_utf8_to_server_lossy().  Before elog(),
       * whose arguments are evaluated once it has begun its message, and
       * inside the PG_TRY, since running out of memory can raise.
       */
      if (message_level_is_interesting(level)) {
        const char *full_message = pljs_utf8_to_server_lossy(msg.data, msg.len);

        elog(level, "%s", full_message);
      }
    }
    PG_CATCH();
    {
      return pljs_throw_caught_error(pljs_catch_error(m_mcontext), ctx);
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
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  char *sql;
  JSValue ret = JS_UNDEFINED;
  JSValue params = {0};
  int nparam;
  MemoryContext m_mcontext;
  pljs_subxact sx;
  ErrorData *edata;
  bool cleanup_params = false;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  sql = pljs_string_arg(ctx, argv[0]);

  if (sql == NULL) {
    return JS_EXCEPTION;
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

  nparam = pljs_js_array_length(params, ctx);

  /* Reading the array's length threw: hand that back to JavaScript. */
  if (nparam < 0) {
    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    pfree(sql);
    return JS_EXCEPTION;
  }

  m_mcontext = CurrentMemoryContext;
  pljs_subxact_init(&sx);

  PG_TRY();
  {
    int status;

    if (!IsTransactionOrTransactionBlock()) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("transaction lock failure")));
    }

    pljs_subxact_begin(&sx, false);

    if (nparam == 0) {
      status = SPI_exec(sql, 0);
    } else {
      status = pljs_execute_params(sql, params, ctx);
    }

    /*
     * The rows are converted inside the PG_TRY as well.  Converting a value
     * can raise -- a numeric too large for a double, a multidimensional
     * array, a value its type's output function cannot render -- and that was
     * raised after the PG_TRY, where JavaScript could not catch it, unwinding
     * past the interpreter's live frames: the next Error the session built
     * crashed the backend.
     */
    MemoryContextSwitchTo(m_mcontext);
    ret = pljs_spi_result_to_jsvalue(status, ctx);

    /*
     * The result rows have been copied into JavaScript values, so the SPI
     * tuple table is dead weight from here.  It lives in the SPI procedure
     * context and would otherwise survive until SPI_finish() at the end of the
     * enclosing function call -- so a function looping over pljs.execute() held
     * every result set it had ever produced.  plan.execute() already does this.
     */
    SPI_freetuptable(SPI_tuptable);
  }
  PG_CATCH();
  {
    edata = pljs_subxact_abort(&sx);

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    pfree(sql);

    return pljs_throw_caught_error(edata, ctx);
  }
  PG_END_TRY();

  edata = pljs_subxact_commit(&sx);

  pfree(sql);

  // If we allocated params, then we need to free it.
  if (cleanup_params) {
    JS_FreeValue(ctx, params);
  }

  if (edata != NULL) {
    JS_FreeValue(ctx, ret);
    return pljs_throw_caught_error(edata, ctx);
  }

  return ret;
}

/**
 * @brief Converts an array of parameters to their types.
 *
 * Each element is released however its conversion ends; see
 * pljs_jsvalue_to_datum_free().
 *
 * @param params #JSValueConst - the array of parameters
 * @param nparams @c int - how many there are
 * @param types #Oid* - the type of each
 * @param typmods @c int32* - the typmod of each, or NULL if none has one
 * @param values #Datum* - filled with each value
 * @param nulls @c char* - filled with 'n' for each null value, ' ' otherwise
 * @param ctx #JSContext - Javascript context to execute in
 */
static void pljs_params_to_datums(JSValueConst params, int nparams,
                                  const Oid *types, const int32 *typmods,
                                  Datum *values, char *nulls, JSContext *ctx) {
  for (int i = 0; i < nparams; i++) {
    bool is_null;

    values[i] = pljs_jsvalue_to_datum_typmod_free(
        types[i], typmods != NULL ? typmods[i] : -1,
        JS_GetPropertyUint32(ctx, params, i), &is_null, ctx);
    nulls[i] = is_null ? 'n' : ' ';
  }
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

  /* Called under pljs_execute()'s PG_TRY, which hands it back to JavaScript. */
  if (nparams < 0) {
    pljs_ereport_js_exception(ctx);
  }

  /*
   * Everything this function allocates is scoped to a child context that is
   * deleted on both the success and the error path.
   *
   * The frees used to sit after SPI_execute_plan_with_paramlist(), so any query
   * that raised -- the common case in real code, and the whole point of a retry
   * loop -- leaked all of it.  None of it is reclaimed by subtransaction
   * rollback: the plan lives in the SPI procedure context and the rest in the
   * caller's, both of which outlive the failed statement.
   *
   * The SPI plan is the exception that still needs explicit handling, because
   * SPI_freeplan() is not memory-context based, hence the PG_CATCH below rather
   * than a context delete alone.
   */
  MemoryContext parm_cxt = AllocSetContextCreate(
      CurrentMemoryContext, "PLJS execute params", ALLOCSET_SMALL_SIZES);
  MemoryContext old_cxt = MemoryContextSwitchTo(parm_cxt);

  Datum *values = palloc(sizeof(Datum) * nparams);
  char *nulls = palloc(sizeof(char) * nparams);

  /*
   * NB: SPI_prepare_params() stores the address of this *stack-local* parstate
   * in the plan's parserSetupArg, so the plan must not outlive this frame.  The
   * SPI_freeplan() calls below are what guarantee that -- they are not an
   * optimisation to be removed.
   */
  pljs_param_state parstate = {.memory_context = parm_cxt, .param_types = 0};
  SPIPlanPtr volatile plan = NULL;

  PG_TRY();
  {
    plan = SPI_prepare_params(sql, pljs_variable_param_setup, &parstate, 0);

    /*
     * Every SPI entry point returns with CurrentMemoryContext set to the SPI
     * procedure context, not to what the caller had.  Re-enter the child
     * context so the parameter datums and the ParamListInfo built below land
     * in it and are released with it; otherwise they accumulate in the SPI
     * procedure context for the life of the enclosing function call.
     */
    MemoryContextSwitchTo(parm_cxt);

    if (parstate.nparams != nparams) {
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("parameter count mismatch: %d != %d",
                             parstate.nparams, nparams)));
    }

    pljs_params_to_datums(params, nparams, parstate.param_types, NULL, values,
                          nulls, ctx);

    ParamListInfo param_li =
        pljs_setup_variable_paramlist(&parstate, values, nulls);

    status = SPI_execute_plan_with_paramlist(plan, param_li, false, 0);
  }
  PG_CATCH();
  {
    if (plan) {
      SPI_freeplan(plan);
    }
    MemoryContextSwitchTo(old_cxt);
    MemoryContextDelete(parm_cxt);
    PG_RE_THROW();
  }
  PG_END_TRY();

  SPI_freeplan(plan);
  MemoryContextSwitchTo(old_cxt);
  MemoryContextDelete(parm_cxt);

  return status;
}

/**
 * @brief Frees the state of a plan's parameters, whose types the parser
 * infers, and the types.
 *
 * @param parstate #pljs_param_state - the state, or NULL for none
 */
static void pljs_free_param_state(pljs_param_state *parstate) {
  if (parstate == NULL) {
    return;
  }

  if (parstate->param_types) {
    pfree(parstate->param_types);
  }

  pfree(parstate);
}

/**
 * @brief Frees a #pljs_plan and everything it owns.
 *
 * Shared by the explicit `plan.free()` path and the GC finalizer.  All of the
 * members live in long-lived contexts (the SPI plan is a saved plan, parstate
 * and its param_types are in CacheMemoryContext), so this is safe to call at
 * any time, including from a finalizer that may run outside a transaction.
 * Note the previous free path leaked parstate->param_types; it is reclaimed
 * here.
 */
static void pljs_free_plan_struct(pljs_plan *plan) {
  if (plan == NULL) {
    return;
  }

  if (plan->plan) {
    SPI_freeplan(plan->plan);
  }

  pljs_free_param_state(plan->parstate);

  if (plan->param_typmods) {
    pfree(plan->param_typmods);
  }

  pfree(plan);
}

/**
 * @brief GC finalizer for the prepared-statement handle class.
 *
 * Without this, a plan object that JavaScript prepares but never explicitly
 * `.free()`s leaks its saved SPI plan and CacheMemoryContext parstate for the
 * life of the backend -- e.g. preparing 20k plans in a loop grew
 * CacheMemoryContext from ~1MB to ~64MB with no way to reclaim it.  The
 * finalizer runs when the handle becomes unreachable so the leak is bounded by
 * the GC interval instead of being permanent.  plan.free() clears the opaque,
 * so this is a no-op after an explicit free (no double free).
 */
static void pljs_plan_handle_finalizer(JSRuntime *rt, JSValue val) {
  pljs_plan *plan = JS_GetOpaque(val, js_prepared_statement_handle_id);

  pljs_free_plan_struct(plan);
}

static const JSClassDef pljs_plan_handle_class = {
    "PljsPreparedStatement",
    .finalizer = pljs_plan_handle_finalizer,
};

void pljs_register_js_classes(JSRuntime *runtime) {
  JS_NewClassID(&js_prepared_statement_handle_id);
  JS_NewClass(runtime, js_prepared_statement_handle_id,
              &pljs_plan_handle_class);
}

/**
 * @brief Converts the parameters of plan.execute() or plan.cursor().
 *
 * Called under the caller's PG_TRY, which hands an error back to JavaScript,
 * with the context current that the values are to be allocated in.
 *
 * The count is checked, and the parameters' types are read, before any
 * JavaScript runs: converting a value can run a getter, which can free the
 * plan -- plan.free(), or `p.plan = null` dropping the last reference to its
 * handle.  Whether the plan survived the conversion is checked after it; see
 * pljs_plan_execute().
 *
 * @param plan #pljs_plan - the plan
 * @param handle #JSValueConst - the plan's handle, which the caller holds
 * @param params #JSValueConst - the array of parameters
 * @param nparams @c int - how many there are
 * @param values #Datum** - set to the values
 * @param nulls @c char** - set to their nulls, as SPI takes them
 * @param ctx #JSContext - Javascript context to execute in
 */
static void pljs_plan_params_to_datums(pljs_plan *plan, JSValueConst handle,
                                       JSValueConst params, int nparams,
                                       Datum **values, char **nulls,
                                       JSContext *ctx) {
  int argcount =
      plan->parstate ? plan->parstate->nparams : SPI_getargcount(plan->plan);
  Oid *types;
  int32 *typmods = NULL;

  if (argcount != nparams) {
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("plan expected %d arguments but %d were passed instead",
                    argcount, nparams)));
  }

  *values = palloc0(sizeof(Datum) * nparams);
  *nulls = palloc(sizeof(char) * nparams);
  types = palloc(sizeof(Oid) * nparams);

  for (int i = 0; i < nparams; i++) {
    types[i] = plan->parstate ? plan->parstate->param_types[i]
                              : SPI_getargtypeid(plan->plan, i);
  }

  /* Copied for the same reason as the types: the plan can be freed. */
  if (plan->param_typmods != NULL) {
    typmods = palloc(sizeof(int32) * nparams);
    memcpy(typmods, plan->param_typmods, sizeof(int32) * nparams);
  }

  pljs_params_to_datums(params, nparams, types, typmods, *values, *nulls, ctx);

  if (JS_GetOpaque(handle, js_prepared_statement_handle_id) != plan) {
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("plan was freed while its parameters were converted")));
  }
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
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  pljs_plan *plan = NULL;
  JSValue params = {0};
  int nparams = 0;
  MemoryContext m_mcontext;
  MemoryContext param_context;
  pljs_subxact sx;
  ErrorData *edata;
  bool cleanup_params = false;
  JSValue ret = JS_UNDEFINED;

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

  /* Reading the array's length threw: hand that back to JavaScript. */
  if (nparams < 0) {
    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return JS_EXCEPTION;
  }

  /*
   * Hold the plan's handle until the plan has run.  Converting a parameter can
   * run JavaScript, and when that dropped the last other reference to the
   * handle -- `p.plan = null` in a getter -- its finalizer freed the plan the
   * conversion was about to read.  plan.free() from there is caught below.
   */
  JSValue handle = JS_GetPropertyStr(ctx, this_val, "plan");

  plan = JS_GetOpaque(handle, js_prepared_statement_handle_id);

  if (plan == NULL) {
    JS_FreeValue(ctx, handle);

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return js_throw("Invalid plan", ctx);
  }

  m_mcontext = CurrentMemoryContext;
  pljs_subxact_init(&sx);

  /*
   * The parameters' values, and anything converting them allocated, belong to
   * this one execution, so they are built in a context of their own and
   * released with it however the execution ends.
   *
   * They were freed one by one instead.  That missed every parameter of a plan
   * prepared with type names -- the documented form -- which leaked until the
   * call ended, and pfree() of a composite parameter raised "pfree called with
   * invalid pointer": its Datum points inside the tuple's allocation rather
   * than at the start of it.  That error was raised after the PG_TRY, where
   * JavaScript could not catch it, and it unwound past the interpreter's live
   * frames.
   */
  param_context = AllocSetContextCreate(m_mcontext, "PLJS plan parameters",
                                        ALLOCSET_SMALL_SIZES);

  PG_TRY();
  {
    if (!IsTransactionOrTransactionBlock()) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("transaction lock failure")));
    }

    pljs_subxact_begin(&sx, false);
    MemoryContextSwitchTo(param_context);

    /*
     * The argument-count check and the bind-parameter conversion run inside
     * the PG_TRY, not before it.  Both can raise -- and since the conversion
     * layer rejects an out-of-range number, a bad boolean string, an embedded
     * NUL or a nested array, they raise for ordinary bad input.  This is a C
     * function QuickJS called: an ereport that escapes it siglongjmps past the
     * interpreter's live frames and the next Error built in the session
     * (return_next's, for one) walks that dead list and segfaults.  Caught
     * here, they become ordinary JavaScript exceptions like every other error
     * from plan.execute().
     */
    Datum *values;
    char *nulls;

    pljs_plan_params_to_datums(plan, handle, params, nparams, &values, &nulls,
                               ctx);

    int status;

    if (plan->parstate) {
      ParamListInfo paramLI =
          pljs_setup_variable_paramlist(plan->parstate, values, nulls);

      status = SPI_execute_plan_with_paramlist(plan->plan, paramLI, false, 0);
    } else {
      status = SPI_execute_plan(plan->plan, values, nulls, false, 0);
    }

    /*
     * Converted inside the PG_TRY as well: converting a value can raise, and
     * that was raised after it; see pljs_execute().
     */
    MemoryContextSwitchTo(m_mcontext);
    ret = pljs_spi_result_to_jsvalue(status, ctx);
    SPI_freetuptable(SPI_tuptable);
  }
  PG_CATCH();
  {
    edata = pljs_subxact_abort(&sx);

    MemoryContextDelete(param_context);
    JS_FreeValue(ctx, handle);

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return pljs_throw_caught_error(edata, ctx);
  }

  PG_END_TRY();

  edata = pljs_subxact_commit(&sx);

  MemoryContextDelete(param_context);
  JS_FreeValue(ctx, handle);

  if (cleanup_params) {
    JS_FreeValue(ctx, params);
  }

  if (edata != NULL) {
    JS_FreeValue(ctx, ret);
    return pljs_throw_caught_error(edata, ctx);
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
  MemoryContext m_mcontext = CurrentMemoryContext;

  plan = JS_GetOpaque(ptr, js_prepared_statement_handle_id);

  /*
   * Clear the opaque *before* freeing, and free under PG_TRY.
   *
   * SPI_freeplan() raises on an invalid plan pointer, and this is a C function
   * QuickJS called, so letting that error out siglongjmps past QuickJS's live
   * stack frames -- see pljs_return_next() for what that costs.  Clearing the
   * opaque first also means a failed free cannot leave the handle pointing at a
   * plan the GC finalizer would then try to free again.
   */
  JS_SetOpaque(ptr, NULL);

  PG_TRY();
  {
    pljs_free_plan_struct(plan);
  }
  PG_CATCH();
  {
    ErrorData *edata = pljs_catch_error(m_mcontext);

    JS_SetPropertyStr(ctx, this_val, "plan", JS_NULL);
    JS_FreeValue(ctx, ptr);

    return pljs_throw_caught_error(edata, ctx);
  }
  PG_END_TRY();

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
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  char *sql;
  JSValue params = {0};
  int nparams;
  Oid *types = NULL;
  int32 *typmods = NULL;
  int32 *saved_typmods = NULL;
  SPIPlanPtr initial = NULL;
  SPIPlanPtr volatile saved = NULL;
  pljs_param_state *parstate = NULL;
  pljs_plan *plan = NULL;
  bool cleanup_params = false;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  /*
   * An undefined or null list of type names is none, rather than one type
   * named "undefined".
   */
  if (argc == 2 && (JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]))) {
    argc = 1;
  }

  if (argc >= 2) {
    if (JS_IsArray(ctx, argv[1])) {
      params = argv[1];
    } else {
      /* Consume trailing elements as an array. */
      params = pljs_values_to_array(argv, argc, 1, ctx);
      cleanup_params = true;
    }

    nparams = pljs_js_array_length(params, ctx);
  } else {
    nparams = 0;
  }

  /* Reading the array's length threw: hand that back to JavaScript. */
  if (nparams < 0) {
    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return JS_EXCEPTION;
  }

  if (nparams) {
    types = palloc(sizeof(Oid) * nparams);
    typmods = palloc(sizeof(int32) * nparams);
  }

  sql = pljs_string_arg(ctx, argv[0]);

  if (sql == NULL) {
    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return JS_EXCEPTION;
  }

  /*
   * Allocate parstate in CacheMemoryContext so it survives beyond the current
   * call_function execution_context (which gets deleted on return).
   * param_types is palloc'd via parstate->memory_context so it must point to
   * the same long-lived context.  We allocate it *before* PG_TRY so the
   * pointer is stable for the cleanup path (and not at risk of being clobbered
   * across the longjmp), and so the PG_CATCH branch can free it: because it
   * lives in the permanent CacheMemoryContext, a failed SPI_prepare would
   * otherwise leak it (and its param_types) for the life of the backend.
   *
   * Only a plan prepared without type names has the parser infer its
   * parameters' types, as plv8 does.  This was the other way round: type names
   * were parsed and then ignored, so `pljs.prepare('SELECT $1 AS x', ['int4'])`
   * bound $1 as text, and a parameter declared as a domain was never checked
   * against it.  An empty list of type names, or undefined, names none, so
   * the parser infers them for it too: prepared with no types at all, the plan
   * could not have used a parameter.
   */
  if (nparams == 0) {
    parstate =
        MemoryContextAllocZero(CacheMemoryContext, sizeof(pljs_param_state));
    parstate->memory_context = CacheMemoryContext;
  }

  pljs_subxact sx;
  ErrorData *edata = NULL;

  pljs_subxact_init(&sx);

  PG_TRY();
  {
    /*
     * In a subtransaction, as pljs.execute() runs its query: parsing the
     * statement and the type names holds catalog pins and relation locks, and
     * an error -- a column that does not exist, a typmod a type does not take
     * -- left them, since only rolling back releases them.  COMMIT warned of
     * each.  Where none can be begun, it does without, as it did before.
     */
    pljs_subxact_begin(&sx, true);

    /*
     * Inside the PG_TRY: an unknown type name raises, and an error escaping a
     * function QuickJS called unwinds past the interpreter's live frames.
     */
    for (int i = 0; i < nparams; i++) {
      JSValue param = JS_GetPropertyUint32(ctx, params, i);
      const char *str = JS_ToCString(ctx, param);

      JS_FreeValue(ctx, param);

      if (str == NULL) {
        elog(ERROR, "could not convert a type name to a string");
      }

      PG_TRY();
      {
        /* In the database's encoding; see pljs_string_arg(). */
        parseTypeString(pljs_utf8_to_server(str, strlen(str)), &types[i],
                        &typmods[i], false);
      }
      PG_FINALLY();
      {
        JS_FreeCString(ctx, str);
      }
      PG_END_TRY();
    }

    if (parstate) {
      initial = SPI_prepare_params(sql, pljs_variable_param_setup, parstate, 0);
    } else {
      initial = SPI_prepare(sql, nparams, types);
    }

    saved = SPI_saveplan(initial);
    SPI_freeplan(initial);
  }

  PG_CATCH();
  {
    /*
     * errfinish() leaves CurrentMemoryContext set to ErrorContext and expects
     * the handler to reset it.  Without this switch the JavaScript function
     * carried on running in ErrorContext after a caught prepare failure, and
     * the next caught error copied its ErrorData there, flushed it, and read
     * the freed copy -- two more caught pljs.execute() failures segfaulted
     * the backend.
     *
     * PG_CATCH() restores PG_exception_stack but does not pop the errordata
     * stack; without FlushErrorState() each caught error leaks one of the five
     * ERRORDATA_STACK_SIZE slots and the sixth PANICs the backend.
     *
     * What went wrong -- an unknown type name, say -- is reported below.
     */
    edata = pljs_subxact_abort(&sx);
  }

  PG_END_TRY();

  /*
   * SPI_prepare() and SPI_saveplan() return with CurrentMemoryContext set to
   * SPI's procedure context, as every SPI entry point does.  Left there, it
   * outlived this call: a getter on a function's result that called
   * pljs.prepare() had the rest of the result built in the procedure context,
   * which SPI_finish() then freed before the result was returned.  Committing
   * the subtransaction goes back to this call's context, with none as well.
   *
   * A subtransaction that did not commit is rolled back, and its error thrown
   * to JavaScript, which can catch it and prepare the plan again, as for any
   * other error.  The plan and its parameters' state are freed either way,
   * since they outlive the rollback, in CacheMemoryContext: each retry after
   * a commit that failed left a pair of them behind for the life of the
   * backend.
   */
  if (edata == NULL) {
    edata = pljs_subxact_commit(&sx);
  }

  if (edata != NULL) {
    if (saved != NULL) {
      SPI_freeplan(saved);
    }

    pljs_free_param_state(parstate);

    if (types != NULL) {
      pfree(types);
      pfree(typmods);
    }

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    pfree(sql);

    return pljs_throw_caught_error(edata, ctx);
  }

  pfree(sql);

  /*
   * The typmods the type names declared.  SPI_prepare() takes only the types,
   * so a value bound to `numeric(5,2)` kept 3.14159 and one bound to
   * `varchar(3)` kept 'abcdef'; the parameters are converted with them
   * instead, as a column's value is.
   */
  for (int i = 0; i < nparams; i++) {
    if (typmods[i] >= 0) {
      saved_typmods =
          MemoryContextAlloc(CacheMemoryContext, sizeof(int32) * nparams);
      memcpy(saved_typmods, typmods, sizeof(int32) * nparams);
      break;
    }
  }

  if (types != NULL) {
    pfree(types);
    pfree(typmods);
  }

  JSValue ret = JS_NewObject(ctx);

  JS_SetPropertyFunctionList(ctx, ret, js_plan_funcs, 4);

  /* Allocate plan in CacheMemoryContext for the same reason as parstate. */
  plan = MemoryContextAlloc(CacheMemoryContext, sizeof(pljs_plan));

  plan->parstate = parstate;
  plan->plan = saved;
  plan->param_typmods = saved_typmods;

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
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  pljs_plan *plan;
  JSValue params = {0};
  int nparams = 0;
  /*
   * Assigned inside the PG_TRY below and read after PG_END_TRY, which is the
   * pattern PostgreSQL's coding conventions require volatile for: without it a
   * compiler is free to keep it in a register that the longjmp clobbers.  Safe
   * in practice today because the read only happens on the non-longjmp path,
   * but that is a property of the current control flow rather than of the code.
   */
  Portal volatile cursor = NULL;
  bool cleanup_params = false;
  MemoryContext m_mcontext;
  MemoryContext param_context;
  pljs_subxact sx;
  ErrorData *edata;

  /* Held until the cursor is open; see pljs_plan_execute(). */
  JSValue handle = JS_GetPropertyStr(ctx, this_val, "plan");

  plan = JS_GetOpaque(handle, js_prepared_statement_handle_id);

  if (plan == NULL || plan->plan == NULL) {
    JS_FreeValue(ctx, handle);

    /* A JavaScript exception, not an ereport: see the note in the PG_TRY. */
    return js_throw("plan unexpectedly null", ctx);
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

  /* Reading the array's length threw: hand that back to JavaScript. */
  if (nparams < 0) {
    JS_FreeValue(ctx, handle);

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return JS_EXCEPTION;
  }

  m_mcontext = CurrentMemoryContext;
  pljs_subxact_init(&sx);

  /*
   * The portal copies the parameters, so they are needed only to open it; see
   * pljs_plan_execute().  The values themselves were never freed here.
   */
  param_context = AllocSetContextCreate(m_mcontext, "PLJS cursor parameters",
                                        ALLOCSET_SMALL_SIZES);

  PG_TRY();
  {
    if (!IsTransactionOrTransactionBlock()) {
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("transaction lock failure")));
    }

    /*
     * Open the cursor inside a subtransaction, the way pljs_execute() runs its
     * query.  Catching an error without rolling back to a savepoint leaves
     * whatever the failed operation had acquired attached to the current
     * resource owner: opening a cursor plans the statement, and the planner
     * holds a pg_proc syscache reference across constant folding, so an error
     * raised while folding (SELECT 1/$1 with $1 = 0, say) escaped past its
     * ReleaseSysCache().  Nothing then released it, and the reference survived
     * to the end of the statement as
     * "WARNING: resource was not closed: cache pg_proc".  The rollback below
     * is what cleans that up.  On success the subtransaction is released and
     * the portal is reassigned to the parent, so the cursor outlives it.
     */
    pljs_subxact_begin(&sx, false);
    MemoryContextSwitchTo(param_context);

    /*
     * The argument-count check and the bind-parameter conversion run inside
     * the PG_TRY, not before it; see pljs_plan_execute().
     */
    Datum *values;
    char *nulls;

    pljs_plan_params_to_datums(plan, handle, params, nparams, &values, &nulls,
                               ctx);

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
    edata = pljs_subxact_abort(&sx);

    /*
     * This path rolls back only an internal subtransaction and then returns a
     * JavaScript error, so the enclosing transaction -- and therefore
     * CurrentMemoryContext -- lives on.  Nothing else will reclaim these.
     */
    MemoryContextDelete(param_context);
    JS_FreeValue(ctx, handle);

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return pljs_throw_caught_error(edata, ctx);
  }

  PG_END_TRY();

  edata = pljs_subxact_commit(&sx);

  /* The portal holds its own copies; these were only needed for the open. */
  MemoryContextDelete(param_context);

  if (edata != NULL) {
    JS_FreeValue(ctx, handle);

    if (cleanup_params) {
      JS_FreeValue(ctx, params);
    }

    return pljs_throw_caught_error(edata, ctx);
  }

  JSValue ret = JS_NewObject(ctx);
  JSValue str = JS_NewString(ctx, cursor->name);
  JS_SetPropertyStr(ctx, ret, "name", str);
  JS_SetPropertyFunctionList(ctx, ret, js_cursor_funcs, 4);

  /*
   * Keep the plan handle reachable from the cursor.
   *
   * pljs_plan_handle_finalizer() calls SPI_freeplan(), and it runs as soon as
   * the plan handle becomes unreachable -- which the natural idiom makes
   * immediate:
   *
   *   var c = pljs.prepare('select ... where id = $1', ['int']).cursor([1]);
   *   while (c.fetch()) { ... }
   *
   * The plan object is unreachable from that second line on, but the portal
   * opened from it is still live and is re-entered by fetch/move/close.
   * SPI_freeplan -> DropCachedPlan sets plansource->magic = 0 and deletes the
   * plansource's context, and SPI_freeplan's own contract says a plan in use
   * must not be freed.  The portal holds a refcount on the CachedPlan, not on
   * the CachedPlanSource, so whether that crashes or merely reads freed memory
   * depends on plancache internals that are not part of any contract.
   *
   * Holding an owned reference here means the plan cannot be collected before
   * the cursor is, so the finalizer cannot run underneath an open portal.
   * JS_SetPropertyStr consumes the reference held since the start, so this
   * transfers cleanly without touching the error paths above.
   */
  JS_SetPropertyStr(ctx, ret, "plan", handle);

  if (cleanup_params) {
    JS_FreeValue(ctx, params);
  }

  return ret;
}

/**
 * @brief Reads the count of rows that `cursor.fetch()` or `cursor.move()` is
 * given, and its direction.
 *
 * Before the cursor is found: reading it runs the value's valueOf(), which
 * can close the cursor, and the portal found first was used after it had
 * been dropped.  A valueOf() that threw was ignored, and the rows fetched
 * with the count left at 0 while its exception was pending.
 *
 * The count is read as a number, with the fraction cut off, and one larger
 * than an int is as many rows as an int can count, forward or backward.  It
 * was read with ToInt32(), which wraps: 2**31 fetched backward, 2**32 + 2
 * fetched two rows, and Infinity fetched none.
 *
 * @param ctx #JSContext - Javascript context
 * @param value #JSValueConst - the count, negative to go backward
 * @param count @c int* - set to the number of rows
 * @param forward @c bool* - set to whether to go forward
 * @returns @c bool - false with an exception pending
 */
static bool pljs_cursor_count(JSContext *ctx, JSValueConst value, int *count,
                              bool *forward) {
  double rows;

  if (JS_ToFloat64(ctx, &rows, value) < 0) {
    return false;
  }

  if (isnan(rows)) {
    rows = 0;
  }

  if (rows < 0) {
    rows = -rows;
    *forward = false;
  }

  *count = rows >= PG_INT32_MAX ? PG_INT32_MAX : (int)rows;

  return true;
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
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  int nfetch = 1;
  bool forward = true, wantarray = false;

  /* See pljs_cursor_count(). */
  if (argc >= 1) {
    wantarray = true;

    if (!pljs_cursor_count(ctx, argv[0], &nfetch, &forward)) {
      return JS_EXCEPTION;
    }
  }

  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *plan_name = JS_ToCString(ctx, name);

  JS_FreeValue(ctx, name);

  if (plan_name == NULL) {
    return JS_EXCEPTION;
  }

  Portal cursor = SPI_cursor_find(plan_name);

  JS_FreeCString(ctx, plan_name);

  if (cursor == NULL) {
    return js_throw("Unable to find cursor", ctx);
  }

  /*
   * Guard the fetch with an internal subtransaction (same pattern as
   * pljs_execute).  The previous code called SPI_rollback()+SPI_finish() in
   * the error handler, which (a) double-finishes the SPI connection owned by
   * call_function and (b) raises "invalid transaction termination" in an
   * atomic context, masking the real error (e.g. a division-by-zero that
   * surfaces lazily during the fetch).  With the subtransaction we can roll
   * back cleanly and re-raise the actual error into JS so it is catchable.
   * Only a subtransaction it began: in parallel mode, before PostgreSQL 17,
   * none can be, and it rolled back the caller's instead.
   */
  JSValue ret = JS_UNDEFINED;
  pljs_subxact sx;
  ErrorData *edata;

  pljs_subxact_init(&sx);

  PG_TRY();
  {
    pljs_subxact_begin(&sx, false);
    SPI_cursor_fetch(cursor, forward, nfetch);

    /* Converted inside the PG_TRY; see pljs_execute(). */
    if (SPI_processed > 0) {
      if (!wantarray) {
        ret = pljs_tuple_to_jsvalue(SPI_tuptable->tupdesc,
                                    SPI_tuptable->vals[0], ctx);
      } else {
        /*
         * SPI_cursor_fetch() returns no status, and what it fetched are a
         * SELECT's rows.  The row count was passed as the status, so a count
         * that happened to equal a status with rows -- 5 is SPI_OK_SELECT --
         * gave the rows, and any other gave back the count itself.
         */
        ret = pljs_spi_result_to_jsvalue(SPI_OK_SELECT, ctx);
      }
    }

    SPI_freetuptable(SPI_tuptable);
  }
  PG_CATCH();
  {
    return pljs_throw_caught_error(pljs_subxact_abort(&sx), ctx);
  }
  PG_END_TRY();

  edata = pljs_subxact_commit(&sx);

  if (edata != NULL) {
    JS_FreeValue(ctx, ret);
    return pljs_throw_caught_error(edata, ctx);
  }

  return ret;
}

/**
 * @brief Javascript function `cursor.move`.
 *
 * Javascript function that executes a move of the current cursor
 * @returns #JSValue containing result of the query
 */
static JSValue pljs_plan_cursor_move(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  int nmove = 1;
  bool forward = true;

  /* See pljs_cursor_count(). */
  if (argc >= 1 && !pljs_cursor_count(ctx, argv[0], &nmove, &forward)) {
    return JS_EXCEPTION;
  }

  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *cursor_name = JS_ToCString(ctx, name);

  JS_FreeValue(ctx, name);

  if (cursor_name == NULL) {
    return JS_EXCEPTION;
  }

  Portal cursor = SPI_cursor_find(cursor_name);

  JS_FreeCString(ctx, cursor_name);

  if (cursor == NULL) {
    return js_throw("Unable to find plan", ctx);
  }

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  /* Subtransaction guard so a move error rolls back cleanly and re-raises the
   * real error into JS (see pljs_plan_cursor_fetch). */
  pljs_subxact sx;
  ErrorData *edata;

  pljs_subxact_init(&sx);

  PG_TRY();
  {
    pljs_subxact_begin(&sx, false);
    SPI_cursor_move(cursor, forward, nmove);
  }
  PG_CATCH();
  {
    return pljs_throw_caught_error(pljs_subxact_abort(&sx), ctx);
  }
  PG_END_TRY();

  edata = pljs_subxact_commit(&sx);

  if (edata != NULL) {
    return pljs_throw_caught_error(edata, ctx);
  }

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
  /*
   * Nothing runs once the call has to end with an error: closing a portal can
   * finish a data-modifying CTE's writes.
   */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  JSValue name = JS_GetPropertyStr(ctx, this_val, "name");
  const char *cursor_name = JS_ToCString(ctx, name);

  JS_FreeValue(ctx, name);

  /*
   * A name that could not be read -- a getter that threw, a Symbol -- is
   * that error, as for fetch() and move(), rather than a cursor not found.
   */
  if (cursor_name == NULL) {
    return JS_EXCEPTION;
  }

  Portal cursor = SPI_cursor_find(cursor_name);

  JS_FreeCString(ctx, cursor_name);

  if (!cursor) {
    return js_throw("Unable to find cursor", ctx);
  }

  /* Subtransaction guard; the previous SPI_rollback()+SPI_finish() here
   * double-finished the connection owned by call_function and raised
   * "invalid transaction termination" in an atomic context (see
   * pljs_plan_cursor_fetch). */
  pljs_subxact sx;
  ErrorData *edata;

  pljs_subxact_init(&sx);

  PG_TRY();
  {
    pljs_subxact_begin(&sx, false);
    SPI_cursor_close(cursor);
  }
  PG_CATCH();
  {
    return pljs_throw_caught_error(pljs_subxact_abort(&sx), ctx);
  }
  PG_END_TRY();

  edata = pljs_subxact_commit(&sx);

  if (edata != NULL) {
    return pljs_throw_caught_error(edata, ctx);
  }

  JSValue ret = JS_NewInt32(ctx, 1);

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
 * Not while a procedure's result is being converted.  Converting it can run
 * JavaScript -- a getter, or toString(), on what the procedure returned --
 * and while it does, the conversion holds resources of the transaction it
 * started in, such as a composite column's pinned row type.  Ending that
 * transaction under it is not safe, so pljs_commit() and pljs_rollback()
 * refuse to; PL/pgSQL runs no user code at that point at all.
 *
 * pljs_converting_result is the backend's, not a JSContext's.  The flag was
 * kept on the storage found through the global `pljs` object of the context
 * that called commit(), so JavaScript that replaced globalThis.pljs, or that
 * called a function compiled in another user's context, found none and
 * committed half way through the conversion: the CALL failed, and what it had
 * written stayed committed.
 *
 * @returns #JSValue containing `undefined`
 */
static JSValue pljs_commit(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  MemoryContext m_mcontext = CurrentMemoryContext;

  /*
   * Not the work of a call that has to end with an error, which rolls it
   * back; see pljs_call_is_ending().
   */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  PG_TRY();
  {
    if (pljs_converting_result) {
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
               errmsg("cannot commit while a procedure's result is being "
                      "converted")));
    }

    // HoldPinnedPortals();
    SPI_commit();
    SPI_start_transaction();
  }
  PG_CATCH();
  {
    /*
     * The caught error MUST be flushed off the errordata stack.  Returning
     * without FlushErrorState() leaves the entry there permanently: the stack
     * is only ERRORDATA_STACK_SIZE (5) deep and is not unwound until the
     * enclosing statement finishes, so five caught commit failures inside a
     * single call make the sixth ereport() PANIC with "ERRORDATA_STACK_SIZE
     * exceeded" -- which kills every backend in the cluster, not just this
     * session.  A procedure that retries a failing commit in a loop (a
     * deadlock, serialization failure, or full disk) hits this.
     *
     * Report the real Postgres error rather than a generic string, so the
     * caller can tell a deadlock from a disk-full.
     *
     * The resource owner is left as it is.  A commit that failed has rolled
     * its transaction back and begun the next, whose owner is the current
     * one; the owner current before it was the failed transaction's, which
     * went with it, and every builtin after the failure used it again:
     * "ResourceOwnerEnlarge called after release started".
     */
    ErrorData *edata = pljs_catch_error(m_mcontext);

    return pljs_throw_caught_error(edata, ctx);
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
  MemoryContext m_mcontext = CurrentMemoryContext;

  /* See pljs_commit(). */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  PG_TRY();
  {
    if (pljs_converting_result) {
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_TRANSACTION_TERMINATION),
               errmsg("cannot roll back while a procedure's result is being "
                      "converted")));
    }

    // HoldPinnedPortals();
    SPI_rollback();
    SPI_start_transaction();
  }
  PG_CATCH();
  {
    /*
     * Flush the caught error, and leave the resource owner as it is; see
     * pljs_commit().
     */
    ErrorData *edata = pljs_catch_error(m_mcontext);

    return pljs_throw_caught_error(edata, ctx);
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
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  if (argc < 1) {
    return JS_UNDEFINED;
  }
  char *signature = pljs_string_arg(ctx, argv[0]);
  JSValue func = JS_UNDEFINED;
  pljs_subxact sx;

  if (signature == NULL) {
    return JS_EXCEPTION;
  }

  /*
   * None is begun here, but compiling the function begins one of its own,
   * and an error can leave another memory context or resource owner current;
   * see pljs_subxact_abort().
   */
  pljs_subxact_init(&sx);

  PG_TRY();
  {
    Oid funcoid;

    /*
     * NB: never `return` from inside a PG_TRY block.  Doing so leaves
     * PG_exception_stack pointing at this frame's (now dead) sigjmp_buf, so
     * the next ereport(ERROR) siglongjmp()s into a freed stack frame and the
     * backend crashes.  When permission is denied we leave `func` as
     * JS_UNDEFINED and fall through to the shared cleanup/return below.
     */
    if (pljs_has_permission_to_execute(signature)) {
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
    /*
     * Flush the caught error (see pljs_commit(): five unflushed catches PANIC
     * the cluster) and surface the real Postgres error.  The old generic
     * "javascript function is not found" message also hid unrelated failures,
     * e.g. a permission or syscache error.
     */
    ErrorData *edata = pljs_subxact_abort(&sx);

    pfree(signature);

    return pljs_throw_caught_error(edata, ctx);
  }
  PG_END_TRY();

  pfree(signature);

  return func;
}

/**
 * @brief Works out the name of a single-column set's column, and its key.
 *
 * A single-column RETURNS TABLE collapses to a scalar return type in the
 * catalog, so the descriptor frequently carries no column name at all, and
 * the function's own name for it -- TABLE (a int4), or an OUT parameter's --
 * is used.  Once for the set, when a row object first needs it: it was looked
 * up for every call, bare values or not, and a key made for every row.  The
 * key is let go when the call ends; see pljs_run_with_storage().
 *
 * @param ctx #JSContext - Javascript context
 * @param state #pljs_return_state - the set, of one column
 * @param attr #Form_pg_attribute - its column
 * @returns @c bool - false with an exception pending
 */
static bool pljs_single_column_name(JSContext *ctx, pljs_return_state *state,
                                    Form_pg_attribute attr) {
  MemoryContext old_context;
  char *name;

  if (state->column_known) {
    return true;
  }

  old_context = MemoryContextSwitchTo(GetMemoryChunkContext(state));
  name = NameStr(attr->attname)[0] != '\0'
             ? pstrdup(NameStr(attr->attname))
             : get_func_result_name(state->fn_oid);
  MemoryContextSwitchTo(old_context);

  if (name != NULL) {
    JSAtom atom = pljs_name_atom(ctx, name);

    if (atom == JS_ATOM_NULL) {
      pfree(name);
      return false;
    }

    state->column_atom = atom;
  }

  state->column_name = name;
  state->column_known = true;

  return true;
}

/**
 * @brief Returns the value a row of a single-column set gives for its column.
 *
 * Used for rows passed to return_next() and rows a set-returning function
 * returns, which have to agree: returning `[{a: 7}]` from a function
 * returning TABLE (a int4) converted the object itself as the int4, while
 * passing the same object to return_next() worked.
 *
 * @param ctx #JSContext - Javascript context to execute in
 * @param row #JSValueConst - the row: a bare value, or a `{column: value}`
 * object
 * @param state #pljs_return_state - the set, of one column
 * @param caller @c const char* - what to name in an error
 * @returns #JSValue - an owned reference to the column's value, or
 * JS_EXCEPTION with an exception pending -- a getter or trap that threw; a
 * row that gives no value for the column raises
 */
JSValue pljs_single_column_value(JSContext *ctx, JSValueConst row,
                                 pljs_return_state *state, const char *caller) {
  Form_pg_attribute attr = TupleDescAttr(state->tuple_desc, 0);
  Oid coltype = attr->atttypid;
  JSValue value = JS_DupValue(ctx, row);

  /*
   * A single-column set is not "composite", so its rows are converted
   * directly as the column value.  That silently mangled the
   * `{column: value}` row object a multi-column set requires: the whole
   * object went through the scalar conversion, yielding "[object Object]" for
   * a text column and 0 for an int/bigint one, with no error.  Code that
   * builds a row object in a loop and calls return_next(row) -- the natural
   * shape, and the only one that works for 2+ columns -- broke as soon as the
   * set happened to have exactly one column.
   *
   * Accept both forms.  The row-object reading applies only to a *plain*
   * object (a brand check, so a Date for a timestamp, a typed array for a
   * bytea and an Array for an array type are still values, not row objects)
   * and only when the column type is not itself object-shaped: a json or
   * jsonb column takes an object as its legitimate value.  So does a domain
   * over one, so look through a domain to its base type.
   *
   * A Proxy is a row object too, as it is for a composite set: it was
   * converted as the column's value itself, so the same Proxy row that a
   * two-column set read was "cannot convert NaN" in a single-column one.
   * Unless it is an array's, which is converted as the array, as it is for a
   * function that returns one; JS_IsArray() sees through a Proxy, as
   * Array.isArray() does.
   */
  bool row_object = pljs_jsvalue_is_plain_object(row);

  if (pljs_jsvalue_is_proxy(row)) {
    int is_array = JS_IsArray(ctx, row);

    /* A revoked Proxy. */
    if (is_array < 0) {
      JS_FreeValue(ctx, value);
      return JS_EXCEPTION;
    }

    row_object = !is_array;
  }

  /*
   * Worked out once for the set: it was looked up for every row.  A set of
   * one column is never of a composite type, which is a set of its columns.
   */
  if (row_object && !state->column_type_known) {
    state->column_takes_objects = pljs_type_is_json(coltype);
    state->column_type_known = true;
  }

  if (row_object) {
    if (!state->column_takes_objects) {
      bool resolved = false;
      bool is_value = false;

      if (!pljs_single_column_name(ctx, state, attr)) {
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
      }

      /*
       * The column's name, whenever there is one, decides, read as a
       * composite set reads a column: an own property, or a getter of the
       * row's class, but nothing every object inherits; and a Proxy's through
       * its get trap, which can make the column up; see pljs_row_column().  A
       * single-column set read only an own, enumerable property, so a class's
       * getter was passed over, and its one other property stored in its
       * place without a word.
       *
       * A function there is a method -- a class's, found the same way -- and
       * no column takes one.  It is an error, rather than a reason to look
       * for the value elsewhere, which stored another property under the
       * column's name, again without a word.
       */
      if (state->column_atom != JS_ATOM_NULL) {
        bool found = false;
        JSValue column =
            pljs_row_get_column(ctx, row, state->column_atom, &found);

        if (JS_IsException(column)) {
          JS_FreeValue(ctx, value);
          return JS_EXCEPTION;
        }

        /*
         * Raised as PostgreSQL's, as for a set of several columns, with its
         * SQLSTATE: thrown to JavaScript, it came out as XX000.
         */
        if (found && pljs_column_refuses(ctx, column, coltype)) {
          JS_FreeValue(ctx, column);
          JS_FreeValue(ctx, value);
          pljs_function_column_error(state->column_name, caller);
        }

        if (found) {
          JS_FreeValue(ctx, value);
          value = column;
          resolved = true;
        }
      }

      /*
       * Without the column, an object that converts itself is a value; see
       * pljs_converts_itself().
       */
      if (!resolved) {
        int converts = pljs_converts_itself(ctx, row);

        if (converts < 0) {
          JS_FreeValue(ctx, value);
          return JS_EXCEPTION;
        }

        is_value = converts > 0;
      }

      /*
       * Otherwise a row object with exactly one property is unambiguous, so
       * accept it.
       */
      if (!resolved && !is_value) {
        JSPropertyEnum *props = NULL;
        uint32_t nprops = 0;

        if (JS_GetOwnPropertyNames(ctx, &props, &nprops, row,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
          JS_FreeValue(ctx, value);
          return JS_EXCEPTION;
        }

        if (nprops == 1) {
          JSValue sole = JS_GetProperty(ctx, row, props[0].atom);

          /*
           * Unless that property is a function, which no column takes.  An
           * object with one was converted whole, as a value that converts
           * itself: a column that held a function stored "[object Object]",
           * or NaN.
           */
          if (JS_IsFunction(ctx, sole)) {
            JS_FreeValue(ctx, sole);
          } else {
            JS_FreeValue(ctx, value);
            value = sole;
            resolved = true;
          }
        }

        pljs_free_prop_enum(ctx, props, nprops);
      }

      /*
       * Neither the column name nor a sole property identified a value.  That
       * is a mistake -- raise instead of converting the object itself and
       * storing "[object Object]" or 0.
       */
      if (!resolved && !is_value) {
        JS_FreeValue(ctx, value);

        if (state->column_name != NULL) {
          ereport(ERROR,
                  (errcode(ERRCODE_DATATYPE_MISMATCH),
                   errmsg("%s: object does not identify a value for the "
                          "result column \"%s\" (property names are case "
                          "sensitive; a single-column set also accepts the "
                          "bare value)",
                          caller, state->column_name)));
        }

        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("%s: object does not identify a value for the single "
                        "result column (give an object with one property, "
                        "or the bare value)",
                        caller)));
      }
    }
  }

  return value;
}

/**
 * @brief Works out how a row of a set of a domain over a composite type is
 * read by the domain's constraints.
 *
 * Once for the set, and again whenever the typcache has loaded the row type
 * again, which it does after any change to it.  The row type it was worked
 * out for, and how a row of the set converts to it, are kept with the set.
 * A row type that no longer matches the set's is an error, as it is for
 * PL/pgSQL.
 *
 * @param state #pljs_return_state - the set
 */
static void pljs_domain_rowtype(pljs_return_state *state) {
  Oid base = pljs_type_base(state->rettype);
  TypeCacheEntry *typentry = lookup_type_cache(base, TYPECACHE_TUPDESC);
  const char *message =
      gettext_noop("returned record type does not match expected record type");
  MemoryContext caller_context = CurrentMemoryContext;
  MemoryContext rowtype_context;
  TupleConversionMap *map;
  TupleDesc rowtype;
  uint64 identifier = typentry->tupDesc_identifier;

  if (state->domain_map_known && state->domain_rowtype_id == identifier) {
    return;
  }

  /*
   * In a context of its own, kept with the set, where the map reads the row
   * type from, and deleted, with whatever the conversion had built, if it
   * does not match.  Checked as the row type returned for the set's, so that
   * the message names the row type's new column types as returned ones.  That
   * check and the map the other way are not the same: a column added with a
   * default needs a conversion one way and not the other, and a map taken to
   * be needed both ways was NULL, and the backend crashed.  The map can raise
   * too, typmods matching one way only, so what the set had is replaced only
   * once both are done.
   */
  rowtype_context =
      AllocSetContextCreate(GetMemoryChunkContext(state),
                            "PLJS Domain Row Type", ALLOCSET_SMALL_SIZES);

  PG_TRY();
  {
    MemoryContextSwitchTo(rowtype_context);

    rowtype = CreateTupleDescCopy(typentry->tupDesc);
    convert_tuples_by_position(rowtype, state->tuple_desc, message);
    map = convert_tuples_by_position(state->tuple_desc, rowtype, message);

    MemoryContextSwitchTo(caller_context);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(caller_context);
    MemoryContextDelete(rowtype_context);
    PG_RE_THROW();
  }
  PG_END_TRY();

  if (state->domain_context != NULL) {
    MemoryContextDelete(state->domain_context);
  }

  state->domain_context = rowtype_context;
  state->domain_map = map;
  state->domain_rowtype = rowtype;
  state->domain_rowtype_id = identifier;
  state->domain_map_known = true;
}

/**
 * @brief Puts a row of a set of a domain over a composite type into the set.
 *
 * The row is built as any composite set's is, with the set's descriptor, which
 * checks that it has every column as it reads each one, and then checked
 * against the domain's constraints.  A null row is checked too -- it fails a
 * NOT NULL domain -- and put with every column NULL, as return_next() puts one
 * for any composite set.
 *
 * It was converted as the domain, with the row type as the catalog has it now,
 * and put into the set, which the executor reads with the descriptor it
 * expected when the call began.  A function that altered the row type's
 * columns and then returned a row had it read as the old type, and the
 * backend crashed.  The two have to match now, as PL/pgSQL requires, since the
 * domain's CHECK constraints read the row with the catalog's.  A row with
 * every column was also read twice, once to check it and once to convert it,
 * which ran a Proxy's get trap twice for each column.
 *
 * @param state #pljs_return_state - the set, whose rettype is the domain
 * @param row #JSValueConst - the row
 * @param ctx #JSContext - Javascript context to execute in
 * @param caller @c const char* - what the row was given to, for a message
 */
void pljs_put_domain_row(pljs_return_state *state, JSValueConst row,
                         JSContext *ctx, const char *caller) {
  /*
   * The row is converted in the set's context for it, which is emptied
   * however the conversion ends.  One was created and deleted for every row,
   * and not deleted at all for a row that did not convert, which a
   * return_next() in a loop that caught its errors left behind each time.  A
   * getter run by the conversion can return_next() a row of its own, which
   * gets a context of its own.
   */
  bool nested = state->row_context_busy;
  MemoryContext row_context =
      nested ? AllocSetContextCreate(CurrentMemoryContext, "PLJS Domain Row",
                                     ALLOCSET_SMALL_SIZES)
             : state->row_context;
  MemoryContext old_context = MemoryContextSwitchTo(row_context);

  state->row_context_busy = true;

  PG_TRY();
  {
    TupleDesc tupdesc = state->tuple_desc;
    int natts = tupdesc->natts;
    bool *nulls = (bool *)palloc0(sizeof(bool) * natts);

    if (JS_IsNull(row) || JS_IsUndefined(row)) {
      Datum *values = (Datum *)palloc0(sizeof(Datum) * natts);

      pljs_type_domain_check(state->rettype, (Datum)0, true);

      memset(nulls, true, sizeof(bool) * natts);
      tuplestore_putvalues(state->tuple_store_state, tupdesc, values, nulls);
    } else {
      Datum *values = pljs_jsvalue_to_datums(NULL, (JSValue)row, &nulls,
                                             tupdesc, ctx, caller);
      HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
      HeapTuple checked = tuple;

      /*
       * After the row is converted, whose getters can alter its type.  A row
       * type that matches the set's with its columns laid out differently --
       * dropped ones elsewhere -- has the row rebuilt for the check.
       */
      pljs_domain_rowtype(state);

      if (state->domain_map != NULL) {
        checked = execute_attr_map_tuple(tuple, state->domain_map);
      } else {
        HeapTupleHeaderSetTypeId(checked->t_data,
                                 state->domain_rowtype->tdtypeid);
        HeapTupleHeaderSetTypMod(checked->t_data,
                                 state->domain_rowtype->tdtypmod);
      }

      pljs_type_domain_check(state->rettype, HeapTupleGetDatum(checked), false);

      tuplestore_puttuple(state->tuple_store_state, tuple);
    }
  }
  PG_FINALLY();
  {
    /* The tuplestore has copied the row. */
    MemoryContextSwitchTo(old_context);

    if (nested) {
      MemoryContextDelete(row_context);
    } else {
      MemoryContextReset(row_context);
      state->row_context_busy = false;
    }
  }
  PG_END_TRY();
}

/**
 * @brief Adds a row to the set being returned; see pljs_return_next().
 *
 * @param ctx #JSContext - Javascript context to execute in
 * @param retstate #pljs_return_state - the set being returned
 * @param row #JSValueConst - the row
 * @returns #JSValue containing `undefined`, or JS_EXCEPTION
 */
static JSValue pljs_return_next_internal(JSContext *ctx,
                                         pljs_return_state *retstate,
                                         JSValueConst row) {
  if (retstate->is_composite) {
    /*
     * null/undefined emits an all-NULL row rather than raising "argument must
     * be an object".  A composite-returning set can legitimately contain a NULL
     * row -- plpgsql writes exactly that with RETURN NEXT NULL -- and there was
     * no way to express it, so a caller mapping a nullable source row had to
     * skip it or invent a sentinel.  This also matches the rest of the
     * conversion surface, where null and undefined are SQL NULL everywhere.
     */
    if (JS_IsNull(row) || JS_IsUndefined(row)) {
      if (retstate->is_domain) {
        pljs_put_domain_row(retstate, row, ctx, "return_next");
        return JS_UNDEFINED;
      }

      bool *nulls = (bool *)palloc(sizeof(bool) * retstate->tuple_desc->natts);
      Datum *values =
          (Datum *)palloc0(sizeof(Datum) * retstate->tuple_desc->natts);

      for (int i = 0; i < retstate->tuple_desc->natts; i++) {
        nulls[i] = true;
      }

      tuplestore_putvalues(retstate->tuple_store_state, retstate->tuple_desc,
                           values, nulls);

      pfree(nulls);
      pfree(values);

      return JS_UNDEFINED;
    }

    /*
     * With the SQLSTATE a returned row's gets; thrown to JavaScript, it came
     * out as XX000.
     */
    if (!JS_IsObject(row)) {
      ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                      errmsg("argument must be an object")));
    }

    /*
     * Every column has to be the row's, which is checked as each is read; see
     * pljs_row_column().
     */
    if (retstate->is_domain) {
      pljs_put_domain_row(retstate, row, ctx, "return_next");
      return JS_UNDEFINED;
    }

    bool *nulls = (bool *)palloc0(sizeof(bool) * retstate->tuple_desc->natts);
    Datum *values = pljs_jsvalue_to_datums(
        NULL, (JSValue)row, &nulls, retstate->tuple_desc, ctx, "return_next");

    tuplestore_putvalues(retstate->tuple_store_state, retstate->tuple_desc,
                         values, nulls);

    pfree(nulls);
    pfree(values);
  } else {
    Oid coltype = TupleDescAttr(retstate->tuple_desc, 0)->atttypid;
    JSValue value = pljs_single_column_value(ctx, row, retstate, "return_next");

    if (JS_IsException(value)) {
      return value;
    }

    bool is_null = false;
    Datum result = pljs_jsvalue_to_datum_free(coltype, value, &is_null, ctx);

    tuplestore_putvalues(retstate->tuple_store_state, retstate->tuple_desc,
                         &result, &is_null);
  }
  return JS_UNDEFINED;
}

/**
 * @brief Whether converting a row of a set can run a domain's CHECK
 * constraint, and so has to be done in a subtransaction.
 *
 * Only a CHECK constraint can run SQL.  Every domain was taken to, so a set
 * of a domain with none -- or with NOT NULL alone -- paid for a subtransaction
 * per row for nothing.  A constraint can be added while the set is being
 * returned, by the function itself, so the answer is worked out again
 * whenever one can have been.
 *
 * @param retstate #pljs_return_state - the set being returned
 * @returns @c bool
 */
static bool pljs_return_next_needs_subtransaction(pljs_return_state *retstate) {
  /* Read first, so that a change while this runs is seen next time. */
  uint64 generation = pljs_type_domain_generation();
  bool needed = false;

  if (retstate->convert_known && retstate->convert_generation == generation) {
    return retstate->convert_in_subtransaction;
  }

  if (retstate->is_domain) {
    /* The domain's own constraints, and those of the row type's columns. */
    needed = pljs_type_may_check_domain(retstate->rettype);
  } else {
    for (int i = 0; i < retstate->tuple_desc->natts && !needed; i++) {
      Form_pg_attribute attr = TupleDescAttr(retstate->tuple_desc, i);

      if (!attr->attisdropped) {
        needed = pljs_type_may_check_domain(attr->atttypid);
      }
    }
  }

  retstate->convert_in_subtransaction = needed;
  retstate->convert_generation = generation;
  retstate->convert_known = true;

  return needed;
}

/**
 * @brief Javascript function `pljs.return_next`.
 *
 * Adds a value to return for a Set Returning Function.
 *
 * Wraps pljs_return_next_internal() so that a PostgreSQL error raised while
 * converting the row -- an out-of-range integer, an unparseable numeric string,
 * a NUL byte in a text value -- becomes a JavaScript exception instead of a
 * longjmp.
 *
 * This is not defensive tidying.  return_next is a C function that QuickJS
 * called, so QuickJS has live JSStackFrame structures on the C stack between us
 * and the interpreter, linked from the runtime.  An ereport(ERROR) here
 * siglongjmps straight past them, leaving rt->current_stack_frame pointing at
 * frames that no longer exist.  The session then looks fine until anything
 * walks that list -- which is what constructing an Error does, via
 * build_backtrace -- so a later, completely unrelated `throw new Error(...)`,
 * typically in a trigger, segfaults the backend:
 *
 *     build_backtrace <- js_error_constructor <- JS_Call <- call_trigger
 *
 * A failing SETOF call followed by any trigger reproduces it in two statements.
 * pljs_execute() has always converted PostgreSQL errors to JavaScript
 * exceptions for the same reason; return_next did not, and became able to raise
 * once the conversion paths started rejecting bad values instead of silently
 * mangling them.
 *
 * Catching an error is only safe when whatever raised it held nothing that the
 * error left behind, and a domain's CHECK constraint can run any SQL: a
 * PL/pgSQL function that raised, or a pljs function whose result could not be
 * converted, left its SPI connection on the stack, and the set-returning
 * function carried on with it as its own -- "transaction left non-empty SPI
 * stack", "improper call to spi_printtup".  So when a row's conversion can
 * run a domain's CHECK constraint, it runs in a subtransaction, as
 * pljs.execute() runs its query, and an error rolls that back.  Other rows do
 * without one, which costs a subtransaction per row; see
 * pljs_return_next_needs_subtransaction().
 */
static JSValue pljs_return_next(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  pljs_storage *storage = pljs_current_storage();
  pljs_return_state *retstate = storage != NULL ? storage->return_state : NULL;
  JSValue result = JS_UNDEFINED;
  pljs_subxact sx;
  ErrorData *edata;

  /*
   * Whether an error ends the call rather than being thrown to JavaScript;
   * see the PG_CATCH.  Assigned in the PG_TRY and read in the PG_CATCH, hence
   * volatile.
   */
  volatile bool unprotected = true;

  /*
   * Watching for a CHECK constraint run all the same, in a row converted
   * without a subtransaction; see the PG_CATCH.  Assigned in the PG_TRY and
   * read in the PG_CATCH, hence volatile.
   */
  pljs_domain_watch watch;
  volatile bool watching = false;
  volatile bool checked = false;

  /* A DO block, a trigger, or a function that does not return a set. */
  if (retstate == NULL) {
    return js_throw("return_next called in context that cannot accept a set",
                    ctx);
  }

  pljs_subxact_init(&sx);

  PG_TRY();
  {
    /*
     * Inside the PG_TRY: working it out reads the catalogs, and loading a
     * domain's constraints can run the IMMUTABLE functions in them.  Nothing
     * can roll back what an error there leaves, so it ends the call, as any
     * error does until the row is converted in a subtransaction.
     */
    if (pljs_return_next_needs_subtransaction(retstate)) {
      /*
       * Where none can be begun -- in parallel mode, before PostgreSQL 17 --
       * the row is converted without one, rather than failing every row of a
       * PARALLEL SAFE function's set, and any error converting it ends the
       * call, as it does in PL/pgSQL, which cannot catch an error there at
       * all.
       */
      pljs_subxact_begin(&sx, true);
      unprotected = !sx.begun;
    } else {
      unprotected = false;
      pljs_type_domain_watch_start(&watch);
      watching = true;
    }

    /*
     * NB: assign, do not return, from inside PG_TRY -- a return here would
     * leave PG_exception_stack pointing at this frame's dead sigjmp_buf.
     */
    result = pljs_return_next_internal(ctx, retstate,
                                       argc > 0 ? argv[0] : JS_UNDEFINED);

    if (watching) {
      watching = false;
      checked = pljs_type_domain_watch_end(&watch);
    }

    /*
     * Any error converting the row, one JavaScript raised too -- a getter
     * that threw -- as a set of several columns raises one: raised here as
     * PostgreSQL's, to end the call with below.  So too once a CHECK
     * constraint ran without a subtransaction, which a set of one column let
     * JavaScript catch, and a set of several did not.
     */
    if ((unprotected || checked) && JS_IsException(result) &&
        !pljs_call_is_ending()) {
      pljs_ereport_js_exception(ctx);
    }
  }
  PG_CATCH();
  {
    /*
     * An error with nothing to roll back what raised it -- a CHECK
     * constraint's PL/pgSQL function, which left its SPI connection on top
     * of the stack -- and JavaScript that caught it went on with that:
     * "transaction left non-empty SPI stack", or buffer pins and relations
     * not closed.  So the call ends instead; see pljs_throw_fatal_error().
     * Telling which errors left nothing took more than it could: some of a
     * domain's input function's errors are its constraints', and some of
     * its constraints' are not.
     *
     * So too for a row that needed none when that was worked out, but ran a
     * CHECK constraint all the same, with none: a getter gave its domain one
     * while the row was converted.
     */
    if (watching) {
      watching = false;
      checked = pljs_type_domain_watch_end(&watch);
    }

    if (unprotected || checked) {
      pljs_subxact_rollback(&sx);
      return pljs_throw_fatal_error(ctx);
    }

    return pljs_throw_caught_error(pljs_subxact_abort(&sx), ctx);
  }
  PG_END_TRY();

  /* Not committed when the call has to end; see pljs_subxact_ending(). */
  if (pljs_subxact_ending(&sx, ctx)) {
    return JS_EXCEPTION;
  }

  /*
   * Nor when converting the row threw -- a single-column row's getter -- but
   * rolled back, as an error PostgreSQL raised is, and as a set of several
   * columns raises the same getter's: whatever a row's getters did is kept
   * or not by whether the row was added, in any set that checks its rows in
   * a subtransaction.  It was kept for a set of one column and not for a set
   * of several.  A rollback that fails ends the call.
   */
  if (JS_IsException(result)) {
    PG_TRY();
    {
      pljs_subxact_rollback(&sx);
    }
    PG_CATCH();
    {
      pljs_subxact_rollback(&sx);
      return pljs_throw_fatal_error(ctx);
    }
    PG_END_TRY();

    return result;
  }

  /*
   * A subtransaction that did not commit is rolled back, but the row it
   * checked is in the set already: the tuplestore is not transactional.
   * Thrown to JavaScript, the error said the row had not been added, and a
   * retry added it twice, so the call ends with it instead.
   */
  edata = pljs_subxact_commit(&sx);

  if (edata != NULL) {
    /* Which ends the call with a copy of it. */
    result = pljs_throw_fatal_error_data(edata, ctx);
    FreeErrorData(edata);
  }

  return result;
}

/**
 * @brief Returns the storage of the window function call running now.
 *
 * A window object's methods act on the window of the call running now, which
 * need not be the call that made the object: JavaScript can keep one in a
 * global and use it from any later call.  They read that call's
 * FunctionCallInfo as its WindowObject whatever kind of call it was, and
 * outside of any call its storage was NULL, and either crashed the backend.
 *
 * @param ctx #JSContext - Javascript context
 * @returns #pljs_storage, or NULL with a JavaScript exception thrown
 */
static pljs_storage *pljs_window_call_storage(JSContext *ctx) {
  pljs_storage *storage = pljs_current_storage();

  /*
   * Or of a call ending with an error from the executor, whose state is not
   * to be used again; see pljs_throw_fatal_error().  js_throw() throws that.
   */
  if (storage == NULL || storage->window_object == NULL ||
      storage->fatal_error != NULL) {
    js_throw("window object used outside of a window function call", ctx);
    return NULL;
  }

  return storage;
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

  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  WindowObject winobj = storage->window_object;

  pljs_window_storage *window_storage;
  MemoryContext m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    window_storage = (pljs_window_storage *)WinGetPartitionLocalMemory(
        winobj, size + sizeof(pljs_window_storage));
  }
  PG_CATCH();
  {
    /* Flush the caught error; see pljs_commit() for why this is mandatory. */
    return pljs_throw_caught_error(pljs_catch_error(m_mcontext), ctx);
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
  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  WindowObject winobj = storage->window_object;

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  JSValue js = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);

  if (JS_IsException(js)) {
    return js;
  }

  const char *str = JS_ToCString(ctx, js);

  if (str == NULL) {
    JS_FreeValue(ctx, js);
    return JS_EXCEPTION;
  }

  size_t str_size = strlen(str);

  size_t size = str_size;

  pljs_window_storage *window_storage;
  MemoryContext m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    window_storage = (pljs_window_storage *)WinGetPartitionLocalMemory(
        winobj, size + sizeof(pljs_window_storage));
  }
  PG_CATCH();
  {
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, js);

    return pljs_throw_caught_error(pljs_catch_error(m_mcontext), ctx);
  }
  PG_END_TRY();

  /*
   * max_length is what the data can hold, the header aside, however the
   * memory was allocated.  Compared with the size of the data and the header
   * together, a value set before any get_partition_local() in a partition
   * made every later value of its size or more an overflow.
   */
  if (window_storage->max_length != 0 && window_storage->max_length < size) {
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, js);

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
  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  WindowObject winobj = storage->window_object;
  MemoryContext m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    pos = WinGetCurrentPosition(winobj);
  }
  PG_CATCH();
  {
    return pljs_throw_caught_error(pljs_catch_error(m_mcontext), ctx);
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
  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  WindowObject winobj = storage->window_object;
  MemoryContext m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    pos = WinGetPartitionRowCount(winobj);
  }
  PG_CATCH();
  {
    /* It reads the rest of the partition; see pljs_window_caught_error(). */
    return pljs_window_caught_error(ctx, m_mcontext);
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

  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  WindowObject winobj = storage->window_object;
  MemoryContext m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    WinSetMarkPosition(winobj, mark_pos);
  }
  PG_CATCH();
  {
    return pljs_throw_caught_error(pljs_catch_error(m_mcontext), ctx);
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

  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  WindowObject winobj = storage->window_object;
  MemoryContext m_mcontext = CurrentMemoryContext;

  PG_TRY();
  {
    res = WinRowsArePeers(winobj, pos1, pos2);
  }
  PG_CATCH();
  {
    /*
     * It reads the rows into the partition and runs the ORDER BY's equality
     * functions; see pljs_window_caught_error().
     */
    return pljs_window_caught_error(ctx, m_mcontext);
  }
  PG_END_TRY();

  return JS_NewBool(ctx, res);
}

/**
 * @brief Resolves the SQL type of window function argument @a argno for the
 * current call.
 *
 * The cached pljs_func.argtypes[] is filled once at compile time.  For a
 * polymorphic or `"any"` argument, the concrete type lives on this call's
 * expression tree, so ask that first.  The cached type is only a fallback,
 * and only when @a argno is one of the declared arguments -- extra VARIADIC
 * `"any"` arguments have no cached slot.
 *
 * @param storage #pljs_storage - execution storage for the current context
 * @param argno @c int - argument number, already range checked
 * @returns #Oid of the argument type for this call
 */
static Oid pljs_window_arg_type(pljs_storage *storage, int argno) {
  FunctionCallInfo fcinfo = storage->fcinfo;

  if (fcinfo != NULL && fcinfo->flinfo != NULL) {
    Oid argtype = get_fn_expr_argtype(fcinfo->flinfo, argno);

    if (OidIsValid(argtype)) {
      return argtype;
    }
  }

  if (argno >= 0 && argno < storage->function->inargs) {
    return storage->function->argtypes[argno];
  }

  return InvalidOid;
}

/**
 * @brief Range checks an argument number that came from JavaScript.
 *
 * Use the call's actual argument count (`fcinfo->nargs` / `PG_NARGS()`), not
 * the declared `inargs`.  A VARIADIC `"any"` window function has `inargs == 1`
 * even when the call supplies more.
 *
 * @param fcinfo #FunctionCallInfo - the call in progress
 * @param argno @c int - argument number supplied by the caller
 * @returns @c true when @a argno names an argument of the running function
 */
static bool pljs_window_arg_number_is_valid(FunctionCallInfo fcinfo,
                                            int argno) {
  if (fcinfo == NULL) {
    return false;
  }

  return argno >= 0 && argno < PG_NARGS();
}

/**
 * @brief Reads an argument number from a JavaScript value.
 *
 * Require a finite integer in int32 range.  `JS_ToInt32()` is the ECMAScript
 * ToInt32 coercion, which would turn `'x'` and `4294967296` into 0.
 *
 * @param ctx #JSContext - Javascript context
 * @param val #JSValue - the value the caller passed
 * @param argno @c int* - filled with the argument number on success
 * @returns @c true on success, @c false with an exception pending
 */
static bool pljs_window_arg_number(JSContext *ctx, JSValueConst val,
                                   int *argno) {
  double d;

  if (JS_ToFloat64(ctx, &d, val)) {
    return false;
  }

  if (!isfinite(d) || d != trunc(d) || d < INT32_MIN || d > INT32_MAX) {
    JS_ThrowRangeError(ctx, "argument number must be an integer");

    return false;
  }

  *argno = (int)d;

  return true;
}

/**
 * @brief Throws what a window object's method that runs the executor raised.
 *
 * An error from the executor -- evaluating an argument, reading rows into the
 * partition -- ends the call; see pljs_window_get_func_arg().  One that
 * PostgreSQL's window code raises itself leaves nothing behind -- a seek
 * before the mark, "cannot fetch row before WindowObject's mark position", or
 * rows_are_peers()'s "specified position is out of window" -- and JavaScript
 * can catch it, as it can the same mistake made by set_mark_position().
 * Every error ended the call, those included.  Told apart by the file that
 * raised it, whose name PostgreSQL records without its path and does not
 * translate: the window API and the WindowAgg node share one, and anything
 * the executor runs underneath them is elsewhere.
 *
 * For a PG_CATCH.
 *
 * @param ctx #JSContext - Javascript context
 * @param mcontext #MemoryContext - the method's
 * @returns #JSValue - JS_EXCEPTION
 */
static JSValue pljs_window_caught_error(JSContext *ctx,
                                        MemoryContext mcontext) {
  ErrorData *edata;
  bool own;

  MemoryContextSwitchTo(mcontext);

  edata = CopyErrorData();
  own = edata->filename != NULL &&
        strcmp(edata->filename, "nodeWindowAgg.c") == 0;
  FreeErrorData(edata);

  if (own) {
    return pljs_throw_caught_error(pljs_catch_error(mcontext), ctx);
  }

  return pljs_throw_fatal_error(ctx);
}

/*
 * Which of WinGetFuncArgInPartition(), WinGetFuncArgInFrame() and
 * WinGetFuncArgCurrent() pljs_window_get_func_arg() calls.
 */
typedef enum pljs_window_arg_kind {
  PLJS_WINDOW_ARG_IN_PARTITION,
  PLJS_WINDOW_ARG_IN_FRAME,
  PLJS_WINDOW_ARG_CURRENT
} pljs_window_arg_kind;

/**
 * @brief Reads a window function's argument and converts it to JavaScript.
 *
 * Reading the argument runs the executor of the query that called the window
 * function, which evaluates the argument's expression and reads rows into the
 * partition, and either can call any function.  An error there ends the call;
 * see pljs_throw_fatal_error().  Only a seek type that is not one is checked
 * first, so that JavaScript can still catch it.
 *
 * Converting the value can raise too -- a multidimensional array, a value its
 * type's output function cannot render -- and an error that escapes a
 * function QuickJS called unwinds past the interpreter's live frames; see
 * pljs_return_next().  That one leaves nothing behind, and JavaScript can
 * catch it.
 *
 * @param ctx #JSContext - Javascript context
 * @param storage #pljs_storage - the window function call's storage
 * @param kind #pljs_window_arg_kind - which row to read the argument of
 * @param argno @c int - the argument, already range checked
 * @param relpos @c int - for a partition or frame, the row relative to seektype
 * @param seektype @c int - for a partition or frame, WINDOW_SEEK_*
 * @param set_mark @c bool - for a partition or frame, whether to set the mark
 * @returns #JSValue of the argument, `undefined` for a row outside of the
 * partition or frame, or JS_EXCEPTION
 */
static JSValue pljs_window_get_func_arg(JSContext *ctx, pljs_storage *storage,
                                        pljs_window_arg_kind kind, int argno,
                                        int relpos, int seektype,
                                        bool set_mark) {
  WindowObject winobj = storage->window_object;
  MemoryContext m_mcontext = CurrentMemoryContext;
  JSValue ret = JS_UNDEFINED;
  bool isnull = false, isout = false;
  Datum res = (Datum)0;

  /* The message PostgreSQL raises for it. */
  if (kind != PLJS_WINDOW_ARG_CURRENT && seektype != WINDOW_SEEK_CURRENT &&
      seektype != WINDOW_SEEK_HEAD && seektype != WINDOW_SEEK_TAIL) {
    return js_throw(psprintf("unrecognized window seek type: %d", seektype),
                    ctx);
  }

  PG_TRY();
  {
    switch (kind) {
    case PLJS_WINDOW_ARG_IN_PARTITION:
      res = WinGetFuncArgInPartition(winobj, argno, relpos, seektype, set_mark,
                                     &isnull, &isout);
      break;
    case PLJS_WINDOW_ARG_IN_FRAME:
      res = WinGetFuncArgInFrame(winobj, argno, relpos, seektype, set_mark,
                                 &isnull, &isout);
      break;
    default:
      res = WinGetFuncArgCurrent(winobj, argno, &isnull);
      break;
    }
  }
  PG_CATCH();
  {
    return pljs_window_caught_error(ctx, m_mcontext);
  }
  PG_END_TRY();

  /* Return undefined to tell it's out of the partition or frame. */
  if (isout) {
    return JS_UNDEFINED;
  }

  PG_TRY();
  {
    ret = pljs_datum_to_jsvalue(pljs_window_arg_type(storage, argno), res,
                                isnull, true, ctx);
  }
  PG_CATCH();
  {
    return pljs_throw_caught_error(pljs_catch_error(m_mcontext), ctx);
  }
  PG_END_TRY();

  return ret;
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
  int relpos;
  int seektype;

  if (!pljs_window_arg_number(ctx, argv[0], &argno) ||
      JS_ToInt32(ctx, &relpos, argv[1]) ||
      JS_ToInt32(ctx, &seektype, argv[2])) {
    return JS_EXCEPTION;
  }

  bool set_mark = JS_ToBool(ctx, argv[3]);

  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  if (!pljs_window_arg_number_is_valid(storage->fcinfo, argno)) {
    JS_ThrowRangeError(
        ctx, "argument number out of range for get_func_arg_in_partition");
    return JS_EXCEPTION;
  }

  return pljs_window_get_func_arg(ctx, storage, PLJS_WINDOW_ARG_IN_PARTITION,
                                  argno, relpos, seektype, set_mark);
}

static JSValue pljs_window_get_func_arg_in_frame(JSContext *ctx,
                                                 JSValueConst this_val,
                                                 int argc, JSValueConst *argv) {
  /* Since we return undefined in "isout" case, throw if arg isn't enough. */
  if (argc < 4) {
    return js_throw("not enough arguments for get_func_arg_in_partition", ctx);
  }

  int argno;
  int relpos;
  int seektype;

  if (!pljs_window_arg_number(ctx, argv[0], &argno) ||
      JS_ToInt32(ctx, &relpos, argv[1]) ||
      JS_ToInt32(ctx, &seektype, argv[2])) {
    return JS_EXCEPTION;
  }

  bool set_mark = JS_ToBool(ctx, argv[3]);

  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  if (!pljs_window_arg_number_is_valid(storage->fcinfo, argno)) {
    JS_ThrowRangeError(
        ctx, "argument number out of range for get_func_arg_in_frame");
    return JS_EXCEPTION;
  }

  return pljs_window_get_func_arg(ctx, storage, PLJS_WINDOW_ARG_IN_FRAME, argno,
                                  relpos, seektype, set_mark);
}

static JSValue pljs_window_get_func_arg_current(JSContext *ctx,
                                                JSValueConst this_val, int argc,
                                                JSValueConst *argv) {
  if (argc < 1) {
    return JS_UNDEFINED;
  }

  int argno;

  if (!pljs_window_arg_number(ctx, argv[0], &argno)) {
    return JS_EXCEPTION;
  }

  pljs_storage *storage = pljs_window_call_storage(ctx);

  if (storage == NULL) {
    return JS_EXCEPTION;
  }

  if (!pljs_window_arg_number_is_valid(storage->fcinfo, argno)) {
    JS_ThrowRangeError(ctx,
                       "argument number out of range for get_func_arg_current");
    return JS_EXCEPTION;
  }

  return pljs_window_get_func_arg(ctx, storage, PLJS_WINDOW_ARG_CURRENT, argno,
                                  0, 0, false);
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
  pljs_storage *storage = pljs_current_storage();

  if (storage == NULL || storage->window_object == NULL) {
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
  /* Nothing runs once the call has to end with an error. */
  if (pljs_call_is_ending()) {
    return pljs_throw_ending(ctx);
  }

  if (argc < 1) {
    return JS_UNDEFINED;
  }

  if (!IsTransactionOrTransactionBlock()) {
    return js_throw("out of transaction", ctx);
  }

  if (!JS_IsFunction(ctx, argv[0])) {
    return JS_UNDEFINED;
  }

  pljs_subxact sx;
  ErrorData *edata;
  JSValue result = JS_UNDEFINED;

  /*
   * Whether the callback threw, and its subtransaction is being rolled back.
   * Assigned in the PG_TRY and read in the PG_CATCH, hence volatile.
   */
  volatile bool rolling_back = false;

  pljs_subxact_init(&sx);

  /*
   * A PostgreSQL error must not escape this function.  pljs.subtransaction() is
   * a C function that QuickJS called, so an ereport(ERROR) here siglongjmps
   * past QuickJS's live stack frames and leaves the runtime's frame list
   * pointing at dead ones; the session then crashes later, in whatever next
   * builds an Error's backtrace.  See pljs_return_next() for the full
   * mechanism.
   *
   * BeginInternalSubTransaction() and ReleaseCurrentSubTransaction() both raise
   * in ordinary operation, so this needs no bad input to reach.
   */
  PG_TRY();
  {
    pljs_subxact_begin(&sx, false);

    result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);

    if (JS_IsException(result)) {
      rolling_back = true;
      pljs_subxact_rollback(&sx);
    }
  }
  PG_CATCH();
  {
    /*
     * A rollback that failed leaves no valid state to resume into, so the call
     * ends with the error rather than handing back a catchable exception and
     * letting the function continue.  Not by re-throwing it, which unwinds
     * past QuickJS's live frames; see pljs_throw_fatal_error().
     */
    if (rolling_back) {
      pljs_subxact_rollback(&sx);
      return pljs_throw_fatal_error(ctx);
    }

    return pljs_throw_caught_error(pljs_subxact_abort(&sx), ctx);
  }
  PG_END_TRY();

  /*
   * Committed only if the callback returned, and the call does not have to
   * end; see pljs_subxact_ending().
   */
  if (pljs_subxact_ending(&sx, ctx)) {
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
  }

  edata = pljs_subxact_commit(&sx);

  if (edata != NULL) {
    JS_FreeValue(ctx, result);
    return pljs_throw_caught_error(edata, ctx);
  }

  return result;
}

#ifdef EXPOSE_GC
static JSValue pljs_gc(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
  JS_RunGC(JS_GetRuntime(ctx));

  return JS_UNDEFINED;
}
#endif

static JSValue pljs_import(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  if (argc != 1) {
    return js_throw("pljs.import() expects exactly one argument", ctx);
  }

  if (!JS_IsString(argv[0])) {
    return js_throw("pljs.import() expects a string", ctx);
  }

  const char *path = JS_ToCString(ctx, argv[0]);
  JSValue ret = pljs_module_import(ctx, path);
  JS_FreeCString(ctx, path);

  return ret;
}

static JSValue pljs_require(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
  if (argc != 1) {
    return js_throw("pljs.require() expects exactly one argument", ctx);
  }

  if (!JS_IsString(argv[0])) {
    return js_throw("pljs.require() expects a string", ctx);
  }

  const char *path = JS_ToCString(ctx, argv[0]);
  JSValue ret = pljs_module_require(ctx, path);
  JS_FreeCString(ctx, path);

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
  pljs_cache_free_all();
  JS_FreeRuntime(rt);

  pljs_runtime_init();

  PG_RETURN_VOID();
}
