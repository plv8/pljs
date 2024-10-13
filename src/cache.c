#include "postgres.h"

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
 * @brief Clears all caches and recreates them.
 */
void pljs_cache_reset(void) {
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
 * @param user_id #Oid
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
 * @returns #pljs_context_cache_value that is found, or `NULL` if not found.
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
 * @returns #pljs_function_cache_value that is found, or `NULL` if not found.
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
  context->js_function = function_entry->fn;

  context->function->inargs = function_entry->nargs;
  for (int i = 0; i < function_entry->nargs; i++) {
    context->function->argtypes[i] = function_entry->argtypes[i];
    context->function->argmodes[i] = function_entry->argmodes[i];
  }

  memcpy(context->function->proname, function_entry->proname, NAMEDATALEN);

  context->function->prosrc =
      (char *)palloc(strlen(function_entry->prosrc) + 1);
  memcpy(context->function->prosrc, function_entry->prosrc,
         strlen(function_entry->prosrc));
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
