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
