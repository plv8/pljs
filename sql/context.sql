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
