-- SET CONTEXT
CREATE OR REPLACE FUNCTION set_context(key TEXT, value TEXT) RETURNS VOID AS
$$
  pljs.elog(NOTICE, JSON.stringify(pljs.data));
  if (!pljs.data) {
    pljs.data = { };
  }

  pljs.data[key] = value;
$$ LANGUAGE pljs;

-- GET CONTEXT
CREATE OR REPLACE FUNCTION get_context(key TEXT) RETURNS TEXT AS
$$
  pljs.elog(NOTICE, JSON.stringify(pljs.data));
  if (!pljs.data) {
    pljs.data = { };
  }

  ret = pljs.data[key];

  return ret;
$$ LANGUAGE pljs;

SELECT set_context('foo', 'bar');

SELECT get_context('foo');

-- pljs_reset() - set state, reset, verify state is gone
DO $$
  pljs.data = { test_key: 'before_reset' };
  pljs.elog(NOTICE, 'before reset: ' + pljs.data.test_key);
$$ LANGUAGE pljs;

SELECT pljs_reset();

DO $$
  var val = (pljs.data && pljs.data.test_key) ? pljs.data.test_key : 'undefined';
  pljs.elog(NOTICE, 'after reset: ' + val);
$$ LANGUAGE pljs;

-- verify functions still work after reset
CREATE FUNCTION post_reset_test() RETURNS int AS $$ return 42; $$ LANGUAGE pljs;
SELECT post_reset_test();
DROP FUNCTION post_reset_test();

-- pljs_reset() cannot be called from inside PL/JS code, which would be running
-- in the contexts it throws away.  It says so, and the caller carries on.
CREATE FUNCTION calls_reset() RETURNS text AS $$
  var kept = 'kept';
  try {
    pljs.execute('SELECT pljs_reset()');
    return 'reset was allowed';
  } catch (e) {
    return kept + ': ' + e.message;
  }
$$ LANGUAGE pljs;
SELECT calls_reset();

-- Nor from a function in another language that a PL/JS function calls.
CREATE FUNCTION plpgsql_reset() RETURNS void AS $$
BEGIN
  PERFORM pljs_reset();
END;
$$ LANGUAGE plpgsql;
CREATE FUNCTION calls_reset_through_plpgsql() RETURNS text AS $$
  try {
    pljs.execute('SELECT plpgsql_reset()');
    return 'reset was allowed';
  } catch (e) {
    return e.message;
  }
$$ LANGUAGE pljs;
SELECT calls_reset_through_plpgsql();

-- Or from a DO block, or when the error is not caught.
DO $$
  try {
    pljs.execute('SELECT pljs_reset()');
    pljs.elog(ERROR, 'reset was allowed');
  } catch (e) {
    pljs.elog(NOTICE, 'do block: ' + e.message);
  }
$$ LANGUAGE pljs;

CREATE FUNCTION calls_reset_uncaught() RETURNS int AS $$
  pljs.execute('SELECT pljs_reset()');
  return 1;
$$ LANGUAGE pljs;
SELECT calls_reset_uncaught();

-- The refusals reset nothing: state set before them is still there.
DO $$ pljs.data = { survivor: 'still here' }; $$ LANGUAGE pljs;
SELECT calls_reset();
DO $$ pljs.elog(NOTICE, 'after refusals: ' + pljs.data.survivor); $$ LANGUAGE pljs;

-- And a reset from a top-level statement still works afterwards.
SELECT pljs_reset();
DO $$
  pljs.elog(NOTICE, 'after reset: ' + (pljs.data ? pljs.data.survivor : 'gone'));
$$ LANGUAGE pljs;

DROP FUNCTION calls_reset();
DROP FUNCTION calls_reset_through_plpgsql();
DROP FUNCTION plpgsql_reset();
DROP FUNCTION calls_reset_uncaught();

-- Per-(user_id, owner_id) context scoping
CREATE ROLE pljs_ctx_owner_a;
GRANT USAGE, CREATE ON SCHEMA public TO pljs_ctx_owner_a;

SET ROLE pljs_ctx_owner_a;
CREATE FUNCTION owner_a_init() RETURNS text AS $$
  globalThis.owner_tag = 'owner_a';
  return globalThis.owner_tag;
$$ LANGUAGE pljs;

CREATE FUNCTION owner_a_helper(x int) RETURNS int AS $$
  return x + (globalThis.owner_tag === 'owner_a' ? 100 : 0);
$$ LANGUAGE pljs;
RESET ROLE;

CREATE FUNCTION caller_scope_check() RETURNS jsonb AS $$
  const fn = pljs.find_function('owner_a_helper');
  return {
    owner_tag: typeof globalThis.owner_tag,
    find_fn_result: fn(5)
  };
$$ LANGUAGE pljs;

SELECT owner_a_init();
SELECT owner_a_helper(5);
SELECT caller_scope_check();

DROP FUNCTION caller_scope_check();
DROP FUNCTION owner_a_helper(int);
DROP FUNCTION owner_a_init();
REVOKE ALL ON SCHEMA public FROM pljs_ctx_owner_a;
DROP ROLE pljs_ctx_owner_a;

-- Bounded context cache with LRU eviction (pljs.max_cached_contexts)
SET pljs.max_cached_contexts = 2;
SELECT (pljs_info()->>'max_cached_contexts')::int AS max_ctx,
       (pljs_info()->>'cached_contexts')::int <= 2 AS bounded;

CREATE ROLE pljs_lru_1;
CREATE ROLE pljs_lru_2;
CREATE ROLE pljs_lru_3;
GRANT USAGE, CREATE ON SCHEMA public TO pljs_lru_1, pljs_lru_2, pljs_lru_3;

SET ROLE pljs_lru_1;
CREATE FUNCTION lru_fn_1() RETURNS int AS $$
  const v = pljs.execute('SELECT lru_fn_2() AS v')[0].v;
  return v + 100;
$$ LANGUAGE pljs;
RESET ROLE;

SET ROLE pljs_lru_2;
CREATE FUNCTION lru_fn_2() RETURNS int AS $$
  const v = pljs.execute('SELECT lru_fn_3() AS v')[0].v;
  return v + 20;
$$ LANGUAGE pljs;
RESET ROLE;

SET ROLE pljs_lru_3;
CREATE FUNCTION lru_fn_3() RETURNS int AS $$
  const info = pljs.execute('SELECT pljs_info() AS i')[0].i;
  pljs.elog(NOTICE, 'Deepest context count (active): ' + info.cached_contexts);
  return 3;
$$ LANGUAGE pljs;
RESET ROLE;

-- Nested calls across multiple owner contexts.
CREATE FUNCTION lru_active_caller() RETURNS int AS $$
  const r1 = pljs.execute('SELECT lru_fn_1() AS v')[0].v;
  return r1;
$$ LANGUAGE pljs;

SELECT lru_active_caller();
SELECT (pljs_info()->>'cached_contexts')::int <= 2 AS bounded_after_nested;

RESET pljs.max_cached_contexts;
DROP FUNCTION lru_active_caller();
DROP FUNCTION lru_fn_1();
DROP FUNCTION lru_fn_2();
DROP FUNCTION lru_fn_3();
REVOKE ALL ON SCHEMA public FROM pljs_lru_1, pljs_lru_2, pljs_lru_3;
DROP ROLE pljs_lru_1, pljs_lru_2, pljs_lru_3;
SELECT pljs_reset();
