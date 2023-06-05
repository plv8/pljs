#include "postgres.h"

#include "access/xlog_internal.h"
#include "catalog/pg_proc.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"

#include "pljs.h"

// hash table for storing caches
HTAB *pljs_context_HashTable = NULL;
HTAB *pljs_function_HashTable = NULL;

// initialization the caches
void pljs_cache_init(void) {
  // initialize context cache
  HASHCTL context_ctl = {0};

  // key size for contexts
  context_ctl.keysize = sizeof(pljs_context_cache_key);

  context_ctl.entrysize = sizeof(pljs_cache_value);
  context_ctl.hcxt = TopMemoryContext;
  // context_ctl.hash = uint32_hash;

  pljs_context_HashTable =
      hash_create("pljs context cache",
                  128, // arbitrary guess at number of users/roles
                  &context_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

  HASHCTL function_ctl = {0};

  // key size for functions
  function_ctl.keysize = sizeof(pljs_function_cache_key);
  function_ctl.entrysize = sizeof(pljs_cache_value);
  function_ctl.hcxt = TopMemoryContext;
  function_ctl.hash = tag_hash;

  pljs_function_HashTable =
      hash_create("pljs function cache",
                  128, // arbitrary guess at functions per user
                  &function_ctl, HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
}

// create the hash key for the function Oid and the JSContext *
static pljs_function_cache_key *create_cache_key(Oid fn_oid) {
  // switch to the top memory context
  MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);

  pljs_function_cache_key *key =
      (pljs_function_cache_key *)palloc(sizeof(pljs_function_cache_key));

  // switch back to the original context.
  MemoryContextSwitchTo(oldcontext);

  char **arguments;
  Oid *argtypes;
  char *argmodes;

  HeapTuple proctuple =
      SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  Form_pg_proc pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);

  key->nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);
  memcpy(key->argtypes, argtypes, sizeof(Oid) * key->nargs);

  key->user_id = GetUserId();
  strncpy(key->proname, pg_proc_entry->proname.data, NAMEDATALEN);

  key->trigger = false;

  elog(NOTICE, "hash key => { %s, %d, %d, %d, %d }", key->proname, key->trigger,
       key->user_id, key->rettype, key->nargs);

  ReleaseSysCache(proctuple);

  return key;
}

void pljs_cache_context_add(Oid user_id, JSContext *ctx) {
  bool found;

  // elog(NOTICE, "pljs_cache_context_add: user_id => %d", user_id);

  pljs_context_cache_key key = {
      user_id}; //(pljs_context_cache_key*)palloc(sizeof(pljs_context_cache_key));
  // key->user_id = user_id;

  pljs_cache_value *hvalue = (pljs_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&key, HASH_ENTER, &found);

  // if it exists, that's probably a bad sign
  if (found) {
    elog(WARNING, "trying to add a context that already exists");
  }

  // hvalue->key = (void*) key;
  hvalue->ctx = ctx;
}

void pljs_cache_context_remove(Oid user_id) {
  bool found;

  pljs_context_cache_key key = {user_id};

  pljs_cache_value *hvalue = (pljs_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&key, HASH_REMOVE, &found);

  if (hvalue) {
    pfree(hvalue->key);
    elog(NOTICE, "found entry to remove");
  } else {
    elog(NOTICE, "no entry found to remove");
  }
}

pljs_cache_value *pljs_cache_context_find(Oid user_id) {
  // elog(NOTICE, "pljs_cache_context_find: user_id => %d", user_id);

  pljs_context_cache_key key = {user_id};
  pljs_cache_value *value = (pljs_cache_value *)hash_search(
      pljs_context_HashTable, (void *)&key, HASH_FIND, NULL);

  return value;
}

void pljs_cache_function_add(Oid fn_oid, JSContext *ctx, JSValue fn) {
  bool found;
  elog(NOTICE, "in pljs_cache_function_add");

  pljs_function_cache_key *key = create_cache_key(fn_oid);
  // pljs_function_cache_key* nkey =
  // (pljs_function_cache_key*)palloc(sizeof(pljs_function_cache_key));
  // memcpy(nkey, key, sizeof(pljs_function_cache_key));

  pljs_cache_value *hvalue = (pljs_cache_value *)hash_search(
      pljs_function_HashTable, (void *)key, HASH_ENTER, &found);

  // if it exists, that's probably a bad sign
  if (found) {
    elog(WARNING, "trying to add a context that already exists");
  }

  elog(NOTICE, "adding hash entry");
  hvalue->key = (void *)key;
  hvalue->ctx = ctx;
  hvalue->fn = fn;
}

void pljs_cache_function_remove(Oid fn_oid) {
  // create the key
  pljs_function_cache_key *key = create_cache_key(fn_oid);

  pljs_cache_value *hvalue = (pljs_cache_value *)hash_search(
      pljs_function_HashTable, (void *)key, HASH_REMOVE, NULL);

  if (hvalue) {
    elog(NOTICE, "found entry to remove");
    // if (hvalue->key) {
    //   pfree(hvalue->key);
    // }
  } else {
    elog(NOTICE, "no entry found to remove");
  }
}

pljs_cache_value *pljs_cache_function_find(Oid fn_oid) {
  pljs_function_cache_key *key = create_cache_key(fn_oid);

  bool found;
  pljs_cache_value *value = (pljs_cache_value *)hash_search(
      pljs_function_HashTable, (void *)key, HASH_FIND, &found);

  if (!found) {
    elog(NOTICE, "function not found");
  }

  return value;
}
