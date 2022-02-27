#include "postgres.h"

#include "catalog/pg_proc.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/syscache.h"
#include "utils/hsearch.h"

#include "pljs.h"

extern HTAB* pljs_HashTable;

static pljs_cache_key* create_cache_key(Oid fn_oid) {
  static pljs_cache_key key = {0};

  memset(&key, 0, sizeof(pljs_cache_key));

  if (fn_oid == InvalidOid) {
    elog(NOTICE, "invalid oid");
    key.fn_oid = fn_oid;
    key.user_id = GetUserId();
    key.nargs = 0;

    return &key;
  }

  HeapTuple proctuple;
  Form_pg_proc pg_proc_entry;
  int nargs;
  char** arguments;
  Oid* argtypes;
  char* argmodes;

  proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  if (!HeapTupleIsValid(proctuple)) {
    elog(ERROR, "cache lookup failed for function %u", fn_oid);
  }

  nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);

  pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);

  ReleaseSysCache(proctuple);

  key.trigger = false;
  key.nargs = nargs;
  memcpy(key.argtypes, argtypes, sizeof(Oid) * nargs);

  elog(NOTICE, "key: %x, %x, %d", fn_oid, key.user_id, nargs);
  return &key;
}

pljs_cache_entry* pljs_hash_table_search(Oid fn_oid) {
  pljs_cache_key* key;
  pljs_cache_entry* entry;

  key = create_cache_key(fn_oid);
  entry = (pljs_cache_entry*)hash_search(pljs_HashTable, (void*)key, HASH_FIND,
                                         NULL);

  return entry;
}

void pljs_hash_table_create(Oid fn_oid, JSContext* ctx, JSValue jsfunc) {
  HeapTuple proctuple;
  Form_pg_proc pg_proc_entry;
  int nargs;
  char** arguments;
  Oid* argtypes;
  char* argmodes;

  pljs_cache_key* key = create_cache_key(fn_oid);

  pljs_cache_entry* hentry;
  bool found;

  MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);

  // search for the entry
  hentry = (pljs_cache_entry*)hash_search(pljs_HashTable, (void*)key,
                                          HASH_ENTER, &found);

  // if it exists, that's probably a bad sign
  if (found) {
    elog(WARNING, "trying to insert a function that already exists");
  }

  MemSet(&hentry->fn, 0, sizeof(pljs_function));

  hentry->fn.fn_oid = fn_oid;
  hentry->fn.ctx = ctx;

  if (fn_oid != InvalidOid) {
    proctuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

    if (!HeapTupleIsValid(proctuple)) {
      elog(ERROR, "cache lookup failed for function %u", fn_oid);
    }

    nargs = get_func_arg_info(proctuple, &argtypes, &arguments, &argmodes);

    pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);

    strcpy(hentry->fn.proname, NameStr(pg_proc_entry->proname));
    hentry->fn.nargs = nargs;
    hentry->fn.func = jsfunc;

    ReleaseSysCache(proctuple);
  }

  hash_update_hash_key(pljs_HashTable, hentry, key);

  MemoryContextSwitchTo(oldcontext);
}


void pljs_hash_table_remove(Oid fn_oid) {
  pljs_cache_key* key = create_cache_key(fn_oid);

  bool found;

  MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);

  // search for the entry
  hash_search(pljs_HashTable, (void*)key,
                                          HASH_REMOVE, &found);

  if (found) {
    elog(NOTICE, "found entry to remove");
  } else {
    elog(NOTICE, "no entry found to remove");
  }

  MemoryContextSwitchTo(oldcontext);
}