#include "postgres.h"

#include "utils/memutils.h"

#include "pljs.h"

/**
 * @brief Hash table mapping user OIDs to their JavaScript contexts.
 *
 * Each PostgreSQL user gets an isolated #JSContext so that JavaScript
 * state (global variables, compiled functions) is not shared across
 * users.  The key is the user's #Oid and the value is a
 * #pljs_context_cache_value containing the #JSContext and a nested
 * hash table of compiled functions.
 */
HTAB *pljs_context_HashTable = NULL;

/**
 * @brief Top-level memory context for all PLJS cache allocations.
 *
 * All context cache entries, function cache entries, and their
 * supporting data structures are allocated within this context
 * (or child contexts of it).  Deleting this context tears down
 * the entire cache.
 */
MemoryContext cache_memory_context = NULL;

/**
 * @brief Initialize the PLJS cache subsystem.
 *
 * Creates the top-level #MemoryContext for cache allocations and the
 * context hash table.  Must be called once during extension
 * initialization (from @c _PG_init) before any other cache functions
 * are used.
 */
void pljs_cache_init(void) {
  // Create the memory context to store pljs_context_cache_value entries
  // along with memory allocations for the hashed values themselves.
  cache_memory_context =
      AllocSetContextCreate(TopMemoryContext, "PLJS Function and Context Cache",
                            ALLOCSET_SMALL_SIZES);

  // Initialize context cache.
  HASHCTL context_ctl = {0};

  // Key size for contexts, we're storing by user_id, which is an Oid.
  context_ctl.keysize = sizeof(Oid);

  context_ctl.entrysize = sizeof(pljs_context_cache_value);
  context_ctl.hcxt = cache_memory_context;

  // We pass 64 as an arbitrary maximum number of roles to store
  // cached contexts for.  If we exceed this, it will expand the
  // hash table.
  pljs_context_HashTable =
      hash_create("PLJS Context Cache",
                  64, // Arbitrary guess at number of users/roles to cache.
                  &context_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/**
 * @brief Drain leaked reference counts on a JSValue down to a target.
 *
 * Repeatedly calls #JS_FreeValue until the internal reference count
 * reaches @p target.  This is used during context teardown to release
 * references that would otherwise prevent the #JSContext from being
 * freed.  Only operates on heap-allocated values (tag >= JS_TAG_FIRST);
 * immediates (ints, bools, etc.) are ignored.
 *
 * @param ctx  The #JSContext that owns @p val.
 * @param val  The #JSValue whose reference count should be drained.
 * @param target  The desired reference count to drain to.
 */
static void drain_refs(JSContext *ctx, JSValue val, int target) {
  if (JS_VALUE_GET_TAG(val) < JS_TAG_FIRST)
    return;
  JSRefCountHeader *p = (JSRefCountHeader *)JS_VALUE_GET_PTR(val);
  while (p->ref_count > target)
    JS_FreeValue(ctx, val);
}

/**
 * @brief Fully tear down a cached JavaScript context.
 *
 * Performs a complete cleanup of a #JSContext associated with a cache
 * entry.  The steps are:
 *
 *  1. Free all cached function #JSValue references in the entry's
 *     function hash table.
 *  2. Overwrite all own properties on the global object and the
 *     @c pljs namespace with @c undefined to release held values.
 *     SetProperty is used instead of DeleteProperty because global
 *     function declarations create non-configurable properties.
 *  3. Run garbage collection to free bytecode objects (each holds a
 *     @c JS_DupContext ref via @c b->realm).
 *  4. Drain any remaining leaked references on the @c pljs and global
 *     objects.
 *  5. Free the #JSContext.
 *
 * This function is called by #pljs_cache_free_all during extension
 * reset.
 *
 * @param cache_entry  Pointer to the #pljs_context_cache_value to tear down.
 *                     The @c ctx field must be non-NULL.
 */
static void pljs_cleanup_context(pljs_context_cache_value *cache_entry) {
  JSContext *ctx = cache_entry->ctx;
  JSValue global_obj = JS_GetGlobalObject(ctx);
  JSValue pljs = JS_GetPropertyStr(ctx, global_obj, "pljs");

  /* Free all cached function JSValues for this context. */
  if (cache_entry->function_hash_table != NULL) {
    HASH_SEQ_STATUS fn_status;
    pljs_function_cache_value *fn_entry;

    hash_seq_init(&fn_status, cache_entry->function_hash_table);
    while ((fn_entry = hash_seq_search(&fn_status)) != NULL) {
      JS_FreeValue(ctx, fn_entry->fn);
      fn_entry->fn = JS_UNDEFINED;
    }
  }

  /*
   * Overwrite all own properties on global and pljs with undefined.
   * We use SetProperty instead of DeleteProperty because global function
   * declarations create non-configurable properties that can't be deleted.
   * Setting to undefined releases the old value (and its JS_DupContext ref).
   */
  JSPropertyEnum *tab;
  uint32_t len;
  if (JS_GetOwnPropertyNames(ctx, &tab, &len, global_obj,
                              JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) == 0) {
    for (uint32_t i = 0; i < len; i++) {
      JS_SetProperty(ctx, global_obj, tab[i].atom, JS_UNDEFINED);
      JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
  }

  if (!JS_IsUndefined(pljs) && !JS_IsNull(pljs) &&
      JS_GetOwnPropertyNames(ctx, &tab, &len, pljs,
                              JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) == 0) {
    for (uint32_t i = 0; i < len; i++) {
      JS_SetProperty(ctx, pljs, tab[i].atom, JS_UNDEFINED);
      JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
  }

  /*
   * Run GC now to free bytecode objects.  Each function bytecode holds
   * a JS_DupContext ref (b->realm) that is released when the bytecode
   * is freed.  We must GC before draining context refs so the bytecode
   * realm refs are already released.
   */
  JS_RunGC(JS_GetRuntime(ctx));

  /* Drain leaked refs on pljs and global. */
  drain_refs(ctx, pljs, 1);
  JS_FreeValue(ctx, pljs);
  drain_refs(ctx, global_obj, 1);

  JS_FreeContext(ctx);
}

/**
 * @brief Destroy and reinitialize all caches.
 *
 * Destroys the context hash table and its backing #MemoryContext, then
 * calls #pljs_cache_init to create fresh, empty caches.  This does
 * @b not free the underlying #JSContext objects — use #pljs_cache_free_all
 * if JavaScript contexts need to be properly torn down first.
 *
 * @warning Any pointers to cache entries become invalid after this call.
 */
void pljs_cache_reset(void) {
  hash_destroy(pljs_context_HashTable);
  MemoryContextDelete(cache_memory_context);
  pljs_cache_init();
}

/**
 * @brief Tear down all cached JavaScript contexts and reset the cache.
 *
 * Iterates over every entry in the context hash table, calling
 * #pljs_cleanup_context on each to properly free the #JSContext and
 * all associated JavaScript values.  After all contexts are torn down,
 * destroys the hash table and #MemoryContext, and reinitializes the
 * cache via #pljs_cache_init.
 *
 * This is the safe way to fully reset the PLJS runtime state (e.g.
 * from @c pljs_reset()).
 *
 * @warning Any pointers to cache entries or #JSContext objects become
 *          invalid after this call.
 */
void pljs_cache_free_all(void) {
  HASH_SEQ_STATUS status;
  pljs_context_cache_value *entry;

  hash_seq_init(&status, pljs_context_HashTable);
  while ((entry = hash_seq_search(&status)) != NULL) {
    if (entry->ctx != NULL) {
      pljs_cleanup_context(entry);
      entry->ctx = NULL;
    }
  }

  hash_destroy(pljs_context_HashTable);
  MemoryContextDelete(cache_memory_context);
  pljs_cache_init();
}

/**
 * @brief Add a new JavaScript context to the cache for a user.
 *
 * Creates a #pljs_context_cache_value entry keyed by @p user_id,
 * stores the given #JSContext in it, and sets up a child
 * #MemoryContext and hash table for caching compiled functions
 * belonging to this context.
 *
 * Each user may have at most one cached context.  Attempting to add
 * a context for a user that already has one raises an ERROR.
 *
 * @param user_id  The PostgreSQL role OID to associate with this context.
 * @param ctx      The #JSContext to cache.  Ownership is transferred to
 *                 the cache; the caller must not free it directly.
 */
void pljs_cache_context_add(Oid user_id, JSContext *ctx) {
  bool found;

  // Ask for an empty #pljs_context_cache_value to fill.
  pljs_context_cache_value *hvalue = (pljs_context_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&user_id, HASH_ENTER, &found);

  // If it found that means we're trying to create a context that
  // already exists for a `user_id`.  This should never happen.
  if (found) {
    ereport(
        ERROR, errcode(ERRCODE_INTERNAL_ERROR),
        errmsg("a context cache entry already exists for user_id %d", user_id));
  }

  hvalue->ctx = ctx;
  hvalue->user_id = user_id;
  HASHCTL function_ctl = {0};

  // Create a #MemoryContext to store the function data.
  hvalue->function_memory_context =
      AllocSetContextCreate(cache_memory_context, "PLJS Function Cache Context",
                            ALLOCSET_SMALL_SIZES);

  // The key is the `fn_oid`, so an #Oid.
  function_ctl.keysize = sizeof(Oid);
  function_ctl.entrysize = sizeof(pljs_function_cache_value);
  function_ctl.hcxt = hvalue->function_memory_context;

  hvalue->ctx = ctx;

  // Create a hash table for #pljs_function_cache_value entries,
  // stored by `fn_oid`.
  hvalue->function_hash_table =
      hash_create("PLJS Function Cache",
                  128, // Arbitrary guess at functions per user.
                  &function_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/**
 * @brief Remove a cached JavaScript context for a user.
 *
 * Removes the #pljs_context_cache_value for @p user_id from the
 * context hash table and destroys its function hash table (which
 * also frees the child #MemoryContext holding function cache data).
 *
 * @note This does @b not free the #JSContext itself.  The caller is
 *       responsible for calling #JS_FreeContext if the context should
 *       be released.
 *
 * @param user_id  The PostgreSQL role OID whose cache entry to remove.
 *                 If no entry exists for this user, the call is a no-op.
 */
void pljs_cache_context_remove(Oid user_id) {
  bool found;

  pljs_context_cache_value *hvalue = (pljs_context_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&user_id, HASH_REMOVE, &found);

  if (hvalue) {
    // Destroys the cache and its #MemoryContext in the process.
    hash_destroy(hvalue->function_hash_table);
  }
}

/**
 * @brief Look up the cached JavaScript context for a user.
 *
 * Searches the context hash table for an entry matching @p user_id.
 *
 * @param user_id  The PostgreSQL role OID to look up.
 * @return Pointer to the #pljs_context_cache_value if found, or
 *         @c NULL if no context has been cached for this user.
 */
pljs_context_cache_value *pljs_cache_context_find(Oid user_id) {
  pljs_context_cache_value *value = (pljs_context_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&user_id, HASH_FIND, NULL);

  return value;
}

/**
 * @brief Cache a compiled JavaScript function.
 *
 * Creates a new #pljs_function_cache_value inside the function hash
 * table of the user's cached context, then copies the function
 * metadata and compiled #JSValue from @p context into it.  The
 * user is identified by @c context->function->user_id and the
 * function by @c context->function->fn_oid.
 *
 * The context cache entry for the user must already exist (created
 * via #pljs_cache_context_add).  If no context entry is found, or
 * if a function entry for this OID already exists, an ERROR is raised.
 *
 * Memory for the cache entry is allocated in the function
 * #MemoryContext owned by the user's context cache entry, so it
 * survives until the context is removed or the cache is reset.
 *
 * @param context  Pointer to a fully populated #pljs_context whose
 *                 @c function and @c js_function fields will be
 *                 copied into the cache.
 */
void pljs_cache_function_add(pljs_context *context) {
  bool found;

  pljs_context_cache_value *ctx_hvalue =
      (pljs_context_cache_value *)hash_search(pljs_context_HashTable,
                                              &context->function->user_id,
                                              HASH_FIND, &found);

  // If we are unable to find a context for the `user_id`, then that
  // is probably a bad sign and we should error out.
  if (!found) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("unable to find context for user %d",
                   context->function->user_id));
  }

  // Ask the cache to create e new entry.
  pljs_function_cache_value *hvalue = (pljs_function_cache_value *)hash_search(
      ctx_hvalue->function_hash_table, &context->function->fn_oid, HASH_ENTER,
      &found);

  // If we found one, then we already have an entry for this function
  // and something has gone wrong, we should error out.
  if (found) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("function cache entry already exists for oid %d",
                   context->function->fn_oid));
  }

  // Switch to the cache memory context for this javascript context.
  MemoryContext old_memory_context =
      MemoryContextSwitchTo(ctx_hvalue->function_memory_context);

  // Fill the cache entry with the values in the context.
  pljs_context_to_function_cache(hvalue, context);

  // Switch back to the calling memory context.
  MemoryContextSwitchTo(old_memory_context);
}

/**
 * @brief Look up a cached compiled function by user and function OID.
 *
 * Performs a two-level lookup: first finds the user's context cache
 * entry, then searches that entry's function hash table for @p fn_oid.
 *
 * @param user_id  The PostgreSQL role OID that owns the context.
 * @param fn_oid   The OID of the function to look up.
 * @return Pointer to the #pljs_function_cache_value if found, or
 *         @c NULL if the user has no cached context or the function
 *         is not in the cache.
 */
pljs_function_cache_value *pljs_cache_function_find(Oid user_id, Oid fn_oid) {
  bool found;

  // Search for the context.
  pljs_context_cache_value *ctx_hvalue =
      (pljs_context_cache_value *)hash_search(pljs_context_HashTable, &user_id,
                                              HASH_FIND, &found);

  // If the context does not exists in the cache, that's probably ok.
  if (!found) {
    return NULL;
  }

  // Search for the function inside of the context.
  pljs_function_cache_value *value = (pljs_function_cache_value *)hash_search(
      ctx_hvalue->function_hash_table, &fn_oid, HASH_FIND, &found);

  return value;
}

/**
 * @brief Restore a #pljs_context from a cached function entry.
 *
 * Populates @p context with the #JSContext, compiled #JSValue function,
 * and function metadata (OID, user, argument types/modes, name, source)
 * stored in @p function_entry.  The @c function field of @p context is
 * allocated in the current #MemoryContext via palloc.
 *
 * This is the inverse of #pljs_context_to_function_cache.
 *
 * @param context         Pointer to the #pljs_context to populate.
 * @param function_entry  Pointer to the #pljs_function_cache_value
 *                        containing the cached data.
 */
void pljs_function_cache_to_context(pljs_context *context,
                                    pljs_function_cache_value *function_entry) {
  context->ctx = function_entry->ctx;

  context->js_function = function_entry->fn;

  context->function = (pljs_func *)palloc(sizeof(pljs_func));

  context->function->fn_oid = function_entry->fn_oid;
  context->function->user_id = function_entry->user_id;
  context->function->trigger = function_entry->trigger;
  context->function->is_srf = function_entry->is_srf;
  context->function->typeclass = function_entry->typeclass;

  context->js_function = function_entry->fn;

  context->function->inargs = function_entry->nargs;
  for (int i = 0; i < function_entry->nargs; i++) {
    context->function->argtypes[i] = function_entry->argtypes[i];
    context->function->argmodes[i] = function_entry->argmodes[i];
  }

  memcpy(context->function->proname, function_entry->proname, NAMEDATALEN);

  context->function->prosrc = (char *)palloc(NAMEDATALEN);

  memcpy(context->function->prosrc, function_entry->prosrc, NAMEDATALEN);
}

/**
 * @brief Store a #pljs_context into a function cache entry.
 *
 * Copies the #JSContext pointer, compiled #JSValue function, and
 * function metadata (OID, user, argument types/modes, name, source)
 * from @p context into @p function_entry.  The source string is
 * deep-copied into the cache #MemoryContext so it outlives the
 * caller's memory context.
 *
 * This is the inverse of #pljs_function_cache_to_context.
 *
 * @param function_entry  Pointer to the #pljs_function_cache_value to fill.
 * @param context         Pointer to the #pljs_context containing the
 *                        data to cache.
 */
void pljs_context_to_function_cache(pljs_function_cache_value *function_entry,
                                    pljs_context *context) {
  MemoryContext old_context = MemoryContextSwitchTo(cache_memory_context);

  function_entry->ctx = context->ctx;

  function_entry->fn_oid = context->function->fn_oid;
  function_entry->user_id = context->function->user_id;
  function_entry->trigger = context->function->trigger;
  function_entry->is_srf = context->function->is_srf;
  function_entry->typeclass = context->function->typeclass;

  function_entry->fn = context->js_function;
  function_entry->nargs = context->function->inargs;
  for (int i = 0; i < function_entry->nargs; i++) {
    function_entry->argtypes[i] = context->function->argtypes[i];
    function_entry->argmodes[i] = context->function->argmodes[i];
  }

  memcpy(function_entry->proname, context->function->proname, NAMEDATALEN);

  function_entry->prosrc =
      (char *)palloc(strlen(context->function->prosrc) + 1);

  memcpy(function_entry->prosrc, context->function->prosrc,
         strlen(context->function->prosrc));

  MemoryContextSwitchTo(old_context);
}
