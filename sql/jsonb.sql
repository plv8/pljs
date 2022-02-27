CREATE FUNCTION get_key(key text, json_data jsonb) RETURNS jsonb
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  var val = json_data[key];
  var ret = {};
  ret[key] = val;
  return JSON.stringify(ret);
$$;

CREATE TABLE jsonbonly (
    data jsonb
);

COPY jsonbonly (data) FROM stdin;
{"ok": true}
\.

-- Call twice to test the function cache.
SELECT get_key('ok', data) FROM jsonbonly;
SELECT get_key('ok', data) FROM jsonbonly;
