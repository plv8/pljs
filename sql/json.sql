CREATE FUNCTION valid_json(json text) RETURNS boolean
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  try {
    JSON.parse(json);
    return true;
  } catch(e) {
    return false;
  }
$$;

SELECT valid_json('{"foo": "bar"}'::TEXT);

CREATE FUNCTION get_key(key text, json_data json) RETURNS json
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  var val = json_data[key];
  var ret = {};
  ret[key] = val;
  return JSON.stringify(ret);
$$;

CREATE TABLE jsononly (
    data json
);

COPY jsononly (data) FROM stdin;
{"ok": true}
\.

-- Call twice to test the function cache.
SELECT get_key('ok', data) FROM jsononly;
SELECT get_key('ok', data) FROM jsononly;
