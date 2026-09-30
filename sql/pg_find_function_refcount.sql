-- pljs.find_function() must hand JavaScript a reference it owns.
--
-- The compiled function lives in the per-user cache, which is its only owner.
-- pljs_function_cache_to_context() borrows that reference, and
-- pljs_find_js_function() returned the borrowed value straight to the JS caller --
-- but a JSValue returned from a C function belongs to the caller, so the engine
-- decremented a count nobody had incremented.  After enough lookups the refcount
-- reached zero while the entry was still cached, and the next call through the
-- cache terminated the backend:
--
--   SELECT ff_loop(1000);
--   server closed the connection unexpectedly
--
-- Present on stock upstream too (reproduced on 9ec6f7f).  Mixing a plain call to
-- the target in among the lookups is what makes it fire quickly, which is why the
-- ordering below is deliberate rather than incidental.
CREATE EXTENSION IF NOT EXISTS pljs;

CREATE FUNCTION ffr_target() RETURNS int LANGUAGE pljs AS $$ return 7; $$;

CREATE FUNCTION ffr_loop(n int) RETURNS int LANGUAGE pljs AS $$
  let total = 0;
  for (let i = 0; i < n; i++) {
    const f = pljs.find_function('ffr_target');
    total += f();
  }
  return total;
$$;

SELECT ffr_loop(10) AS lookups_10;
-- A direct call in between, which is what drove the refcount down fastest.
SELECT ffr_target() AS direct_call;
SELECT ffr_loop(2000) AS lookups_2000;
SELECT ffr_target() AS direct_call_after;

-- Release the handed-out references and collect, then use the cache again.  If the
-- cached value had been freed, this is where it would be noticed.
CREATE FUNCTION ffr_gc() RETURNS int LANGUAGE pljs AS $$
  for (let i = 0; i < 2000; i++) { pljs.find_function('ffr_target'); }
  if (typeof pljs.gc === 'function') { pljs.gc(); }
  return pljs.find_function('ffr_target')();
$$;
SELECT ffr_gc() AS after_gc;
SELECT ffr_target() AS direct_call_after_gc;

-- Looking up a function that is replaced between lookups, so the entry is dropped
-- while references to the old compiled value are still outstanding.
CREATE FUNCTION ffr_churn() RETURNS int LANGUAGE pljs AS $$
  let last = 0;
  for (let i = 0; i < 50; i++) {
    const f = pljs.find_function('ffr_target');
    last = f();
    pljs.execute("CREATE OR REPLACE FUNCTION ffr_target() RETURNS int LANGUAGE pljs AS $b$ return 7; $b$");
  }
  return last;
$$;
SELECT ffr_churn() AS replaced_between_lookups;
SELECT ffr_target() AS still_callable;

DROP FUNCTION ffr_churn();
DROP FUNCTION ffr_gc();
DROP FUNCTION ffr_loop(int);
DROP FUNCTION ffr_target();
