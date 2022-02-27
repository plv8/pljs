#include "postgres.h"

#include "utils/jsonb.h"

#include "pljs.h"

static JSValue pljs_elog(JSContext*, JSValueConst, int, JSValueConst*);

void pljs_setup_namespace(JSContext* ctx) {
  // get a copy of the global object.
  JSValue global_obj = JS_GetGlobalObject(ctx);

  // set up the pljs namespace and functions.
  JSValue pljs = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, pljs, "elog",
                    JS_NewCFunction(ctx, pljs_elog, "elog", 2));

  JS_SetPropertyStr(ctx, global_obj, "pljs", pljs);

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

static JSValue pljs_elog(JSContext* ctx, JSValueConst this_val, int argc,
                         JSValueConst* argv) {
  if (argc) {
    int32_t level;

    JS_ToInt32(ctx, &level, argv[0]);

    const char* message = JS_ToCString(ctx, argv[1]);

    elog(level, "%s", message);
  }

  return JS_UNDEFINED;
}
