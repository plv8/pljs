-- pljs represents a SQL array as a flat JavaScript array. An array whose shape
-- cannot be represented that way must raise, not silently change value.
--
-- Both directions were wrong, in opposite ways.
--
-- Reading: deconstruct_array() flattens, so a multidimensional array arrived in
-- JavaScript as a one-dimensional one -- {{1,2},{3,4}} became [1,2,3,4] -- and
-- writing it back produced a different value than was read.
--
-- Writing: a nested JavaScript array aimed at a scalar element type was passed to
-- the array conversion anyway. The element loop then converted each *inner array*
-- to the element type, so `return [[1,2],[3,4]]` for int[] produced two integers
-- that were really the inner arrays' pointers. Numbers that look like data.
CREATE EXTENSION IF NOT EXISTS pljs;

\set VERBOSITY terse

-- Reading a multidimensional array.
CREATE FUNCTION shp_read() RETURNS text AS $$
  return JSON.stringify(pljs.execute("SELECT '{{1,2},{3,4}}'::int[] AS v")[0].v);
$$ LANGUAGE pljs;

SELECT shp_read();

-- Writing a nested array to a scalar element type.
CREATE FUNCTION shp_write() RETURNS int[] AS $$ return [[1, 2], [3, 4]]; $$ LANGUAGE pljs;

SELECT shp_write();

-- A JavaScript array aimed at a plain scalar, which took the same path.
CREATE FUNCTION shp_scalar() RETURNS int AS $$ return [1, 2]; $$ LANGUAGE pljs;

SELECT shp_scalar();

CREATE FUNCTION shp_text() RETURNS text AS $$ return ['a', 'b']; $$ LANGUAGE pljs;

SELECT shp_text();

\set VERBOSITY default

-- One-dimensional arrays are unaffected, in both directions.
CREATE FUNCTION shp_ok_write() RETURNS int[] AS $$ return [1, 2, 3]; $$ LANGUAGE pljs;

SELECT shp_ok_write() AS one_dimensional_out;

CREATE FUNCTION shp_ok_read() RETURNS text AS $$
  return JSON.stringify(pljs.execute("SELECT '{1,2,3}'::int[] AS v")[0].v);
$$ LANGUAGE pljs;

SELECT shp_ok_read() AS one_dimensional_in;

-- And a nested array is still valid for json and jsonb, which is the one target
-- that can represent it.
CREATE FUNCTION shp_jsonb() RETURNS jsonb AS $$ return [[1, 2], [3, 4]]; $$ LANGUAGE pljs;

SELECT shp_jsonb() AS nested_jsonb;

CREATE FUNCTION shp_jsonb_arr() RETURNS jsonb[] AS $$ return [[1, 2], [3]]; $$ LANGUAGE pljs;

SELECT shp_jsonb_arr() AS nested_jsonb_array;

DROP FUNCTION shp_read, shp_write, shp_scalar, shp_text, shp_ok_write, shp_ok_read,
              shp_jsonb, shp_jsonb_arr;
