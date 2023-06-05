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
