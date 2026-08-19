#include "postgres.h"

#include "access/htup_details.h"
#include "utils/memutils.h"

#include "pljs.h"

/**
 * @brief #HTAB hash table for storing a #plvs_context_cache_value `user_id`.
 *
 * A javascript context is stored here by `user_id`.  This allows for
 * javascript contexts to be separated by postgres user, keeping a
 * copy of the current javascript context and all functions defined in the
 * javascript context by `fn_oid`.
 */
HTAB *pljs_context_HashTable = NULL;

/**
 * @brief #MemoryContext where all cached memory is allocated.
 */
MemoryContext cache_memory_context = NULL;

/**
 * @brief Initializes the cache #HTAB along with the #MemoryContext
 * where cached memory is allocated.
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
 * @brief Drops the cached compiled form of one function, in every user's cache.
 *
 * Used when a function is created or replaced, so the next call recompiles it.
 *
 * The alternative -- pljs_cache_reset() -- destroys every per-user JSContext and
 * rebuilds it on the next call.  JS_FreeContext() will not free a context that
 * still has live references into it, so the old one is not necessarily reclaimed,
 * and a backend doing repeated DDL grows without bound.  Removing a single entry
 * keeps every JSContext alive and owned, so nothing is orphaned and nothing
 * dangles -- including when the DDL is executed from inside a running pljs
 * function via pljs.execute(), where freeing the context we are executing in
 * would be fatal.
 *
 * @param fn_oid #Oid - the function whose compiled form is now stale
 */
void pljs_cache_function_remove(Oid fn_oid) {
  HASH_SEQ_STATUS status;
  pljs_context_cache_value *ctx_hvalue;

  if (pljs_context_HashTable == NULL) {
    return;
  }

  hash_seq_init(&status, pljs_context_HashTable);

  while ((ctx_hvalue =
              (pljs_context_cache_value *)hash_seq_search(&status)) != NULL) {
    bool found = false;
    pljs_function_cache_value *value;

    if (ctx_hvalue->function_hash_table == NULL) {
      continue;
    }

    value = (pljs_function_cache_value *)hash_search(
        ctx_hvalue->function_hash_table, &fn_oid, HASH_FIND, &found);

    if (!found || value == NULL) {
      continue;
    }

    /*
     * Drop our reference to the compiled function before the entry goes away;
     * this is its only owner, so otherwise it leaks on the QuickJS heap.
     */
    JS_FreeValue(value->ctx, value->fn);

    if (value->prosrc != NULL) {
      pfree(value->prosrc);
      value->prosrc = NULL;
    }

    hash_search(ctx_hvalue->function_hash_table, &fn_oid, HASH_REMOVE, NULL);
  }
}

/**
 * @brief Clears all caches and recreates them.
 */
void pljs_cache_reset(void) {
  HASH_SEQ_STATUS status;
  pljs_context_cache_value *ctx_hvalue;

  /*
   * Free the QuickJS side before dropping the Postgres memory that points at
   * it.  hash_destroy() and MemoryContextDelete() below reclaim only the
   * palloc'd entries; the JSContexts and compiled functions they reference live
   * on the libc heap and would otherwise be orphaned inside the runtime with no
   * owner left to free them.
   */
  if (pljs_context_HashTable != NULL) {
    hash_seq_init(&status, pljs_context_HashTable);

    while ((ctx_hvalue = (pljs_context_cache_value *)hash_seq_search(&status)) !=
           NULL) {
      if (ctx_hvalue->function_hash_table != NULL) {
        HASH_SEQ_STATUS fstatus;
        pljs_function_cache_value *value;

        hash_seq_init(&fstatus, ctx_hvalue->function_hash_table);

        while ((value = (pljs_function_cache_value *)hash_seq_search(
                    &fstatus)) != NULL) {
          JS_FreeValue(value->ctx, value->fn);
        }
      }

      if (ctx_hvalue->ctx != NULL) {
        JS_FreeContext(ctx_hvalue->ctx);
        ctx_hvalue->ctx = NULL;
      }
    }
  }

  hash_destroy(pljs_context_HashTable);
  MemoryContextDelete(cache_memory_context);
  pljs_cache_init();
}

/**
 * @brief Adds a #pljs_context_cache_value for a `user_id`.
 *
 * Creates a #pljs_context_cache_value and fills it with the
 * javascript context and #HTAB storing #pljs_function_cache_value
 * entries by `fn_oid`.
 *
 * @param user_id #Oid - user ID for the #JSContext to be assigned to
 * @param ctx #JSContext - context to be added to the cache
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
 * @brief Removes a #pljs_context_cache_value for a `user_id`.
 *
 * Removes a cache entry from the cache by `user_id`.
 * @param user_id #Oid - user ID for which to remove the cache entries
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
 * @brief Finds a #pljs_context_cache_value for a `user_id`.
 *
 * @param user_id #Oid
 * @returns #pljs_context_cache_value that is found, or `NULL` if not found
 */
pljs_context_cache_value *pljs_cache_context_find(Oid user_id) {
  pljs_context_cache_value *value = (pljs_context_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&user_id, HASH_FIND, NULL);

  return value;
}

/**
 * @brief Adds a javascript function in the cache for a `user_id` and `fn_oid`.
 *
 * Adds a function by creating a #pljs_function_cache_value and populating
 * it from a #pljs_context.
 * @param context Pointer to #pljs_context
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
 * @brief Finds a #pljs_function_cache_value for a `user_id` and `fn_oid`.
 *
 * @param user_id #Oid
 * @param fn_oid #Oid
 * @returns #pljs_function_cache_value that is found, or `NULL` if not found
 */
pljs_function_cache_value *pljs_cache_function_find(Oid user_id, Oid fn_oid,
                                                    HeapTuple proctuple) {
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

  if (!found || value == NULL) {
    return NULL;
  }

  /*
   * A hit is only usable if it was compiled from the pg_proc tuple that is
   * current now.  CREATE OR REPLACE writes a new tuple version, and the OID is
   * unchanged, so the entry would otherwise still match and the backend would
   * go on running the previous body.
   *
   * The validator drops the entry directly, which covers the backend issuing
   * the DDL.  It cannot cover any other backend: pljs registers no syscache
   * invalidation callback, so nothing else tells them.  Before this check, a
   * session that had already called a function kept running the old body for
   * the rest of its life, however many times the function was replaced.
   *
   * This also settles DROP FUNCTION followed by OID reuse, where a new function
   * inherits the OID of the old one: the tuple is a different tuple, so the
   * mismatch is detected rather than the old body being run under the new name.
   */
  if (HeapTupleIsValid(proctuple) &&
      (value->fn_xmin != HeapTupleHeaderGetRawXmin(proctuple->t_data) ||
       !ItemPointerEquals(&value->fn_tid, &proctuple->t_self))) {
    pljs_cache_function_remove(fn_oid);
    return NULL;
  }

  return value;
}

/**
 * @brief Fills a #pljs_context from a #pljs_function_cache_value.
 *
 * Copies the data from a pljs context to a function cache value.
 * @param context Pointer to #pljs_context to copy from
 * @param function_entry Pointer to #pljs_function_cache_value to fill
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

  /*
   * prosrc is the (variable-length) function body, not a NAMEDATALEN name.
   * Copying a fixed NAMEDATALEN bytes truncated bodies longer than 63 chars
   * and over-read the source allocation for shorter ones.  pstrdup copies
   * exactly the right length now that the cached copy is NUL-terminated.
   */
  context->function->prosrc = pstrdup(function_entry->prosrc);
}

/**
 * @brief Fills a #pljs_function_cache_value from a #pljs_context
 *
 * Copies the data from a function cache value to a pljs context.
 * @param function_entry Pointer to #pljs_function_cache_value to copy from
 * @param context Pointer to #pljs_context to fill
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

  function_entry->fn_xmin = context->function->fn_xmin;
  function_entry->fn_tid = context->function->fn_tid;

  function_entry->fn = context->js_function;
  function_entry->nargs = context->function->inargs;
  for (int i = 0; i < function_entry->nargs; i++) {
    function_entry->argtypes[i] = context->function->argtypes[i];
    function_entry->argmodes[i] = context->function->argmodes[i];
  }

  memcpy(function_entry->proname, context->function->proname, NAMEDATALEN);

  /*
   * Copy the full body including its NUL terminator.  The previous code
   * allocated strlen+1 but memcpy'd only strlen bytes, leaving the final byte
   * uninitialized (palloc does not zero), so the cached string was not
   * NUL-terminated and any later read walked off the end.  We are in
   * cache_memory_context here, so pstrdup allocates in the right place.
   */
  function_entry->prosrc = pstrdup(context->function->prosrc);

  MemoryContextSwitchTo(old_context);
}
