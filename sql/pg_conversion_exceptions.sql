-- A value QuickJS cannot make raises, and is not kept as the value.
--
-- JSON nested past QuickJS's stack limit, or a string, object or bytea buffer
-- that pljs.memory_limit has no room for, is reported by returning
-- JS_EXCEPTION.  That was kept as the value -- an argument, a column, an array
-- element -- and reached JavaScript as a value that is not one, with the
-- exception still pending: a json argument nested 15,000 deep had a typeof
-- "unknown".
--
-- 15,000 is past QuickJS's limit, half of max_stack_depth, and short of the
-- JSON parser's own; either raising is right, and only a value coming back
-- is wrong.

CREATE FUNCTION cex_typeof(x json) RETURNS text LANGUAGE pljs AS $$
  return typeof x;
$$;

CREATE FUNCTION cex_typeof_element(x json[]) RETURNS text LANGUAGE pljs AS $$
  return typeof x[0];
$$;

CREATE TYPE cex_row AS (j json);

CREATE FUNCTION cex_typeof_column(x cex_row) RETURNS text LANGUAGE pljs AS $$
  return typeof x.j;
$$;

-- 1) An argument, an array element and a column.
DO $$
DECLARE
  deep json;
BEGIN
  deep := (repeat('[', 15000) || repeat(']', 15000))::json;

  BEGIN
    RAISE NOTICE 'argument returned %', cex_typeof(deep);
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'argument raised';
  END;

  BEGIN
    RAISE NOTICE 'element returned %', cex_typeof_element(ARRAY[deep]);
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'element raised';
  END;

  BEGIN
    RAISE NOTICE 'column returned %', cex_typeof_column(ROW(deep)::cex_row);
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'column raised';
  END;
EXCEPTION WHEN OTHERS THEN
  -- The JSON parser's own limit.
  RAISE NOTICE 'argument raised';
  RAISE NOTICE 'element raised';
  RAISE NOTICE 'column raised';
END $$;

-- 2) A query's column, which JavaScript can catch.
DO $$
  try {
    const row = pljs.execute(
      "SELECT (repeat('[', 15000) || repeat(']', 15000))::json AS j")[0];
    pljs.elog(NOTICE, 'query returned ' + typeof row.j);
  } catch (e) {
    pljs.elog(NOTICE, 'query raised');
  }

  pljs.elog(NOTICE, JSON.stringify(pljs.execute("SELECT '[1]'::json AS j")));
$$ LANGUAGE pljs;

-- 3) jsonb is built without JSON.parse(), and every key and value of it is
-- checked in the same way.
CREATE FUNCTION cex_jsonb(x jsonb) RETURNS text LANGUAGE pljs AS $$
  return JSON.stringify(x);
$$;

SELECT cex_jsonb('{"a": [1, {"b": "c", "1": 2}], "d": {"e": null}, "f": true}');
SELECT length(cex_jsonb((repeat('[', 3000) || repeat(']', 3000))::jsonb));

-- A number too large for a double raises, with nothing built so far left
-- behind.
SELECT cex_jsonb('{"a": {"b": [1, 1e400]}}');

SELECT 1 AS still_connected;

DROP FUNCTION cex_jsonb(jsonb);
DROP FUNCTION cex_typeof_column(cex_row);
DROP TYPE cex_row;
DROP FUNCTION cex_typeof_element(json[]);
DROP FUNCTION cex_typeof(json);
