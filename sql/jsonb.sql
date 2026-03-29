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

-- Verify that keys and values are working.
CREATE FUNCTION pljs_test(keys TEXT[], vals TEXT[]) RETURNS JSONB AS $$
    var o = {};
    for(var i=0; i<keys.length; i++){
        o[keys[i]] = vals[i];
    }
    return o;
$$ LANGUAGE pljs IMMUTABLE STRICT;

SELECT pljs_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
