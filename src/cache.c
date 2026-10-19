#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

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

static uint64 pljs_context_lru_clock = 0;

static void pljs_make_context_key(pljs_context_key *key, Oid user_id,
                                  Oid owner_id) {
  memset(key, 0, sizeof(pljs_context_key));
  key->user_id = user_id;
  key->owner_id = OidIsValid(owner_id) ? owner_id : user_id;
}

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

  // Key size for contexts: composite (user_id, owner_id).
  context_ctl.keysize = sizeof(pljs_context_key);

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
 * @brief Fully tear down a cached JavaScript context.
 *
 * Releases all cached function #JSValue references in the entry's
 * function hash table and drops the cache's #JSContext reference via
 * #JS_FreeContext.  Any remaining internal realm cycles (e.g. from
 * global functions or loaded ES modules) are collected by QuickJS's
 * cycle collector during #JS_FreeRuntime or #JS_RunGC.
 *
 * This function is called by #pljs_cache_free_all during extension
 * reset and by #pljs_cache_evict_excess during LRU eviction.
 *
 * @param cache_entry  Pointer to the #pljs_context_cache_value to tear down.
 *                     The @c ctx field must be non-NULL.
 */
static void pljs_cleanup_context(pljs_context_cache_value *cache_entry) {
  JSContext *ctx = cache_entry->ctx;
  JSRuntime *runtime = JS_GetRuntime(ctx);
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
      if (fn_entry->prosrc != NULL) {
        pfree(fn_entry->prosrc);
        fn_entry->prosrc = NULL;
      }
    }
  }

  /*
   * Overwrite all own properties on pljs and globalThis with undefined so
   * user-attached closures and functions release their realm references
   * immediately.
   */
  JSPropertyEnum *tab;
  uint32_t len;
  if (!JS_IsUndefined(pljs) && !JS_IsNull(pljs) &&
      JS_GetOwnPropertyNames(ctx, &tab, &len, pljs,
                             JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) == 0) {
    for (uint32_t i = 0; i < len; i++) {
      if (JS_SetProperty(ctx, pljs, tab[i].atom, JS_UNDEFINED) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
      }
      JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
  }

  if (JS_GetOwnPropertyNames(ctx, &tab, &len, global_obj,
                             JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) == 0) {
    for (uint32_t i = 0; i < len; i++) {
      if (JS_SetProperty(ctx, global_obj, tab[i].atom, JS_UNDEFINED) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
      }
      JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
  }

  JS_FreeValue(ctx, pljs);
  JS_FreeValue(ctx, global_obj);

  JS_FreeContext(ctx);
  if (runtime != NULL) {
    JS_RunGC(runtime);
  }
}

/**
 * @brief Drops the cached compiled form of one function (and any downstream
 * functions transpiled through it as a custom language handler), in every
 * user's cache.
 *
 * Used when a function is created or replaced, so the next call recompiles it
 * and any downstream functions whose source was transpiled by it.
 *
 * The alternative -- pljs_cache_reset() -- destroys every per-user JSContext
 * and rebuilds it on the next call.  JS_FreeContext() will not free a context
 * that still has live references into it, so the old one is not necessarily
 * reclaimed, and a backend doing repeated DDL grows without bound.  Removing
 * individual entries keeps every JSContext alive and owned, so nothing is
 * orphaned and nothing dangles -- including when the DDL is executed from
 * inside a running pljs function via pljs.execute(), where freeing the context
 * we are executing in would be fatal.
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

  while ((ctx_hvalue = (pljs_context_cache_value *)hash_seq_search(&status)) !=
         NULL) {
    HASH_SEQ_STATUS fstatus;
    pljs_function_cache_value *value;

    if (ctx_hvalue->function_hash_table == NULL) {
      continue;
    }

    hash_seq_init(&fstatus, ctx_hvalue->function_hash_table);

    while ((value = (pljs_function_cache_value *)hash_seq_search(&fstatus)) !=
           NULL) {
      bool should_remove = (value->fn_oid == fn_oid);

      for (int i = 0; !should_remove && i < value->nhandlers; i++) {
        if (value->handlers[i].fn_oid == fn_oid) {
          should_remove = true;
        }
      }

      if (!should_remove) {
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

      Oid entry_oid = value->fn_oid;
      hash_search(ctx_hvalue->function_hash_table, &entry_oid, HASH_REMOVE,
                  NULL);
    }
  }
}

/**
 * @brief Destroy and reinitialize all caches.
 *
 * Frees every cached compiled function and #JSContext, destroys the context
 * hash table and its backing #MemoryContext, then calls #pljs_cache_init to
 * create fresh, empty caches.  Unlike #pljs_cache_free_all, this does not
 * scrub the global and @c pljs objects or drain leaked references before
 * freeing each context.
 *
 * @warning Any pointers to cache entries become invalid after this call.
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

    while ((ctx_hvalue =
                (pljs_context_cache_value *)hash_seq_search(&status)) != NULL) {
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
 * @brief Evict least-recently-used inactive contexts until at most
 * @p max_contexts remain in the cache.
 *
 * Contexts that are currently active on the call stack (checked via
 * #pljs_context_is_active) are never evicted.
 */
void pljs_cache_evict_excess(int max_contexts) {
  if (pljs_context_HashTable == NULL || max_contexts < 0) {
    return;
  }

  while (hash_get_num_entries(pljs_context_HashTable) > max_contexts) {
    HASH_SEQ_STATUS status;
    pljs_context_cache_value *entry;
    pljs_context_cache_value *victim = NULL;

    hash_seq_init(&status, pljs_context_HashTable);
    while ((entry = (pljs_context_cache_value *)hash_seq_search(&status)) !=
           NULL) {
      if (entry->ctx == NULL || pljs_context_is_active(entry->ctx)) {
        continue;
      }
      if (victim == NULL || entry->lru_tick < victim->lru_tick) {
        victim = entry;
      }
    }

    if (victim == NULL) {
      break;
    }

    pljs_context_key victim_key = victim->key;
    pljs_cleanup_context(victim);
    victim->ctx = NULL;
    pljs_cache_context_remove(victim_key.user_id, victim_key.owner_id);

    if (rt != NULL) {
      JS_RunGC(rt);
    }
  }
}

long pljs_cache_context_count(void) {
  if (pljs_context_HashTable == NULL) {
    return 0;
  }
  return hash_get_num_entries(pljs_context_HashTable);
}

/**
 * @brief Add a new JavaScript context to the cache for a (user_id, owner_id)
 * pair.
 *
 * Creates a #pljs_context_cache_value entry keyed by (@p user_id, @p owner_id),
 * stores the given #JSContext in it, and sets up a child
 * #MemoryContext and hash table for caching compiled functions
 * belonging to this context.
 *
 * @param user_id   The effective PostgreSQL role OID executing the code.
 * @param owner_id  The PostgreSQL role OID that owns the function (proowner).
 * @param ctx       The #JSContext to cache.  Ownership is transferred to
 *                  the cache; the caller must not free it directly.
 */
void pljs_cache_context_add(Oid user_id, Oid owner_id, JSContext *ctx) {
  pljs_context_key key;
  pljs_context_cache_value *hvalue;
  MemoryContext function_memory_context;
  HTAB *function_hash_table;
  HASHCTL function_ctl = {0};
  bool found;

  pljs_make_context_key(&key, user_id, owner_id);

  // If it found that means we're trying to create a context that
  // already exists for `(user_id, owner_id)`.  This should never happen.
  if (hash_search(pljs_context_HashTable, (void *)&key, HASH_FIND, NULL) !=
      NULL) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("a context cache entry already exists for user_id %u, "
                   "owner_id %u",
                   key.user_id, key.owner_id));
  }

  if (configuration.max_cached_contexts > 0) {
    pljs_cache_evict_excess(configuration.max_cached_contexts - 1);
  }

  /*
   * What the entry holds is made before the entry is: one left half filled by
   * an error -- running out of memory -- pointed at a context that its caller
   * then freed; see pljs_create_context().
   */

  // Create a #MemoryContext to store the function data.
  function_memory_context =
      AllocSetContextCreate(cache_memory_context, "PLJS Function Cache Context",
                            ALLOCSET_SMALL_SIZES);

  // The key is the `fn_oid`, so an #Oid.
  function_ctl.keysize = sizeof(Oid);
  function_ctl.entrysize = sizeof(pljs_function_cache_value);
  function_ctl.hcxt = function_memory_context;

  // Create a hash table for #pljs_function_cache_value entries,
  // stored by `fn_oid`.
  function_hash_table =
      hash_create("PLJS Function Cache",
                  128, // Arbitrary guess at functions per user.
                  &function_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

  hvalue = (pljs_context_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&key, HASH_ENTER, &found);

  hvalue->key = key;
  hvalue->ctx = ctx;
  hvalue->function_memory_context = function_memory_context;
  hvalue->function_hash_table = function_hash_table;
  hvalue->lru_tick = ++pljs_context_lru_clock;
}

/**
 * @brief Remove a cached JavaScript context for a (user_id, owner_id) pair.
 *
 * Removes the #pljs_context_cache_value for (@p user_id, @p owner_id) from the
 * context hash table and destroys its function hash table (which
 * also frees the child #MemoryContext holding function cache data).
 *
 * @note This does @b not free the #JSContext itself.  The caller is
 *       responsible for calling #JS_FreeContext if the context should
 *       be released.
 */
void pljs_cache_context_remove(Oid user_id, Oid owner_id) {
  pljs_context_key key;

  pljs_make_context_key(&key, user_id, owner_id);

  pljs_context_cache_value *hvalue = (pljs_context_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&key, HASH_FIND, NULL);

  if (hvalue) {
    HTAB *function_hash_table = hvalue->function_hash_table;
    MemoryContext function_memory_context = hvalue->function_memory_context;

    hash_search(pljs_context_HashTable, (void *)&key, HASH_REMOVE, NULL);
    hash_destroy(function_hash_table);
    MemoryContextDelete(function_memory_context);
  }
}

/**
 * @brief Look up the cached JavaScript context for a (user_id, owner_id) pair.
 */
pljs_context_cache_value *pljs_cache_context_find(Oid user_id, Oid owner_id) {
  pljs_context_key key;

  pljs_make_context_key(&key, user_id, owner_id);

  pljs_context_cache_value *value = (pljs_context_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&key, HASH_FIND, NULL);

  if (value != NULL) {
    value->lru_tick = ++pljs_context_lru_clock;
  }

  return value;
}

/**
 * @brief Look up a cached context entry by its #JSContext pointer.
 */
pljs_context_cache_value *pljs_cache_context_find_by_ctx(JSContext *ctx) {
  HASH_SEQ_STATUS status;
  pljs_context_cache_value *entry;

  if (pljs_context_HashTable == NULL || ctx == NULL) {
    return NULL;
  }

  hash_seq_init(&status, pljs_context_HashTable);
  while ((entry = (pljs_context_cache_value *)hash_seq_search(&status)) !=
         NULL) {
    if (entry->ctx == ctx) {
      hash_seq_term(&status);
      entry->lru_tick = ++pljs_context_lru_clock;
      return entry;
    }
  }

  return NULL;
}

/**
 * @brief Cache a compiled JavaScript function.
 */
void pljs_cache_function_add(pljs_context *context) {
  bool found;
  pljs_context_cache_value *ctx_hvalue =
      pljs_cache_context_find_by_ctx(context->ctx);

  if (ctx_hvalue == NULL) {
    ctx_hvalue = pljs_cache_context_find(context->function->user_id,
                                         context->function->owner_id);
  }

  // If we are unable to find a context, then that
  // is probably a bad sign and we should error out.
  if (ctx_hvalue == NULL) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("unable to find context for user %u, owner %u",
                   context->function->user_id, context->function->owner_id));
  }

  // Ask the cache to create e new entry.
  pljs_function_cache_value *hvalue = (pljs_function_cache_value *)hash_search(
      ctx_hvalue->function_hash_table, &context->function->fn_oid, HASH_ENTER,
      &found);

  // If an entry already exists for this OID (for example, if a custom language
  // handler re-entrantly compiled the same function while transpiling it),
  // release the previous entry's QuickJS reference and prosrc copy before
  // overwriting it.
  if (found) {
    JS_FreeValue(hvalue->ctx, hvalue->fn);
    hvalue->fn = JS_UNDEFINED;
    if (hvalue->prosrc != NULL) {
      pfree(hvalue->prosrc);
      hvalue->prosrc = NULL;
    }
  } else {
    hvalue->ctx = NULL;
    hvalue->fn = JS_UNDEFINED;
    hvalue->prosrc = NULL;
  }

  // Switch to the cache memory context for this javascript context.
  MemoryContext old_memory_context =
      MemoryContextSwitchTo(ctx_hvalue->function_memory_context);

  PG_TRY();
  {
    // Fill the cache entry with the values in the context.
    pljs_context_to_function_cache(hvalue, context);
  }
  PG_CATCH();
  {
    hash_search(ctx_hvalue->function_hash_table, &context->function->fn_oid,
                HASH_REMOVE, NULL);
    MemoryContextSwitchTo(old_memory_context);
    PG_RE_THROW();
  }
  PG_END_TRY();

  // Switch back to the calling memory context.
  MemoryContextSwitchTo(old_memory_context);
}

/**
 * @brief Checks whether every language handler recorded in a cached function's
 * dependency chain still matches the current catalog state.
 *
 * When a custom language handler (or any upstream handler in a chained
 * language hierarchy) is replaced via CREATE OR REPLACE FUNCTION in the same
 * or another backend, its pg_proc tuple's (xmin, tid) changes while the
 * downstream function's own pg_proc tuple remains untouched.  Walking the
 * recorded handler chain ensures the downstream function is invalidated and
 * re-transpiled on the next lookup.
 *
 * @param value Cached function entry to validate
 * @param proctuple Current pg_proc tuple of the downstream function
 * @param stale_handler_oid Set to the OID of the first stale handler found,
 *                          or InvalidOid if the language structure itself
 *                          changed
 * @returns true if all handlers in the chain are still valid, false otherwise
 */
static bool pljs_cache_handlers_valid(pljs_function_cache_value *value,
                                      HeapTuple proctuple,
                                      Oid *stale_handler_oid) {
  *stale_handler_oid = InvalidOid;

  if (value->nhandlers <= 0) {
    return true;
  }

  if (!HeapTupleIsValid(proctuple)) {
    return false;
  }

  Oid current_lang_oid = ((Form_pg_proc)GETSTRUCT(proctuple))->prolang;

  for (int i = 0; i < value->nhandlers; i++) {
    CHECK_FOR_INTERRUPTS();

    if (!OidIsValid(current_lang_oid)) {
      return false;
    }

    HeapTuple langtuple =
        SearchSysCache1(LANGOID, ObjectIdGetDatum(current_lang_oid));
    if (!HeapTupleIsValid(langtuple)) {
      return false;
    }

    Form_pg_language langstruct = (Form_pg_language)GETSTRUCT(langtuple);
    Oid handler_oid = langstruct->lanplcallfoid;
    ReleaseSysCache(langtuple);

    if (handler_oid != value->handlers[i].fn_oid) {
      *stale_handler_oid = value->handlers[i].fn_oid;
      return false;
    }

    HeapTuple htuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
    if (!HeapTupleIsValid(htuple)) {
      *stale_handler_oid = handler_oid;
      return false;
    }

    TransactionId h_xmin = HeapTupleHeaderGetRawXmin(htuple->t_data);
    ItemPointerData h_tid = htuple->t_self;
    Oid next_lang_oid = ((Form_pg_proc)GETSTRUCT(htuple))->prolang;
    ReleaseSysCache(htuple);

    if (h_xmin != value->handlers[i].fn_xmin ||
        !ItemPointerEquals(&h_tid, &value->handlers[i].fn_tid)) {
      *stale_handler_oid = handler_oid;
      return false;
    }

    current_lang_oid = next_lang_oid;
  }

  return true;
}

static pljs_function_cache_value *
pljs_cache_function_find_in_context(pljs_context_cache_value *ctx_hvalue,
                                    Oid fn_oid, HeapTuple proctuple) {
  bool found;

  // If the context does not exist in the cache, that's ok.
  if (ctx_hvalue == NULL || ctx_hvalue->function_hash_table == NULL) {
    return NULL;
  }

  // Search for the function inside of the context.
  // TODO: figure out how to not cache in case of anonymous inline block or
  // language handler without oid using something other than this hack.
  if (!OidIsValid(fn_oid)) {
    return NULL;
  }

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

  /*
   * A function that imports modules has them bound as of when it was compiled.
   * Once the module tables have changed it would go on calling the old ones.
   */
  if (value->module_generation != 0 &&
      value->module_generation != pljs_modules_generation()) {
    pljs_cache_function_remove(fn_oid);
    return NULL;
  }

  /*
   * Also verify that every custom language handler in the function's
   * transpilation chain (if any) still matches its current pg_proc tuple.
   * If any handler in the chain was replaced (in this backend with
   * check_function_bodies = off, or in another backend), evict the stale
   * handler and all functions depending on it so they re-transpile.
   */
  Oid stale_handler_oid = InvalidOid;
  if (!pljs_cache_handlers_valid(value, proctuple, &stale_handler_oid)) {
    if (OidIsValid(stale_handler_oid)) {
      pljs_cache_function_remove(stale_handler_oid);
    }
    pljs_cache_function_remove(fn_oid);
    return NULL;
  }

  return value;
}

/**
 * @brief Look up a cached compiled function by (user_id, owner_id) and
 * function OID.
 */
pljs_function_cache_value *pljs_cache_function_find(Oid user_id, Oid owner_id,
                                                    Oid fn_oid,
                                                    HeapTuple proctuple) {
  pljs_context_cache_value *ctx_hvalue =
      pljs_cache_context_find(user_id, owner_id);

  return pljs_cache_function_find_in_context(ctx_hvalue, fn_oid, proctuple);
}

/**
 * @brief Look up a cached compiled function inside the specific #JSContext.
 */
pljs_function_cache_value *pljs_cache_function_find_by_ctx(
    JSContext *ctx, Oid fn_oid, HeapTuple proctuple) {
  pljs_context_cache_value *ctx_hvalue = pljs_cache_context_find_by_ctx(ctx);

  return pljs_cache_function_find_in_context(ctx_hvalue, fn_oid, proctuple);
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

  context->function = (pljs_func *)palloc0(sizeof(pljs_func));

  context->function->fn_oid = function_entry->fn_oid;
  context->function->user_id = function_entry->user_id;
  context->function->owner_id = function_entry->owner_id;
  context->function->trigger = function_entry->trigger;
  context->function->is_srf = function_entry->is_srf;
  context->function->module_generation = function_entry->module_generation;

  context->function->fn_xmin = function_entry->fn_xmin;
  context->function->fn_tid = function_entry->fn_tid;
  context->function->nhandlers = function_entry->nhandlers;
  for (int i = 0; i < function_entry->nhandlers; i++) {
    context->function->handlers[i] = function_entry->handlers[i];
  }

  context->js_function = function_entry->fn;

  context->function->inargs = function_entry->nargs;
  for (int i = 0; i < function_entry->nargs; i++) {
    context->function->argtypes[i] = function_entry->argtypes[i];
    context->function->argmodes[i] = function_entry->argmodes[i];
  }

  memcpy(context->function->proname, function_entry->proname, NAMEDATALEN);

  /*
   * Borrow the cached NUL-terminated prosrc pointer rather than copying a
   * potentially multi-hundred-KB transpiled source string on every row call.
   */
  context->function->prosrc = function_entry->prosrc;
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
  function_entry->owner_id = context->function->owner_id;
  function_entry->trigger = context->function->trigger;
  function_entry->is_srf = context->function->is_srf;

  function_entry->fn_xmin = context->function->fn_xmin;
  function_entry->fn_tid = context->function->fn_tid;
  function_entry->module_generation = context->function->module_generation;
  function_entry->nhandlers = context->function->nhandlers;
  for (int i = 0; i < context->function->nhandlers; i++) {
    function_entry->handlers[i] = context->function->handlers[i];
  }

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
