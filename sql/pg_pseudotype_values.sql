-- A value of a pseudo-type is converted as what it is.
--
-- Every pseudo-type was taken for a row, since record is one.  record[] --
-- an array of anonymous rows -- and anyarray -- pg_stats' histogram_bounds --
-- had their arrays read as tuple headers, which named no type: "type with OID
-- 0 does not exist", and the JS_EXCEPTION that came back was kept as the
-- value, so the next one crashed the backend.  So did a cstring.

-- 1) record[], as a query's column, inside a row, and as an argument.
DO $$
  for (let i = 0; i < 5; i++) {
    pljs.execute("SELECT ARRAY[ROW('a', 1)] AS a");
  }

  pljs.elog(NOTICE, JSON.stringify(pljs.execute(
    "SELECT ARRAY[ROW('a', 1), ROW('b', 2)] AS a, ROW(1, ARRAY[ROW(2, 3)]) AS r"
  )[0]));
$$ LANGUAGE pljs;

CREATE FUNCTION ptv_records(x record[]) RETURNS text LANGUAGE pljs AS $$
  return JSON.stringify(x);
$$;
SELECT ptv_records(ARRAY[ROW(1, 'a'), ROW(2, 'b')]);

-- 2) anyarray: the array says what its elements are.
CREATE TABLE ptv_stats AS SELECT g AS x FROM generate_series(1, 1000) AS g;
ANALYZE ptv_stats;
DO $$
  const h = pljs.execute("SELECT histogram_bounds AS h FROM pg_stats " +
                         "WHERE tablename = 'ptv_stats' AND attname = 'x'")[0].h;

  pljs.elog(NOTICE, Array.isArray(h) + ' ' + typeof h[0] + ' ' + h[0] + ' ' +
            h[h.length - 1]);
$$ LANGUAGE pljs;

-- 3) A cstring, by its output function.
DO $$
  pljs.elog(NOTICE, JSON.stringify(pljs.execute(
    "SELECT textout('abc') AS c, NULL::cstring AS n")[0]));
$$ LANGUAGE pljs;

-- 4) An anonymous row cannot be built from JavaScript: there is nothing to
-- say what its columns are.
DO $$
  try {
    pljs.prepare('SELECT $1 AS x', ['record[]']).execute([[{a: 1}]]);
  } catch (e) {
    pljs.elog(NOTICE, e.message);
  }
$$ LANGUAGE pljs;

-- 5) void has no value.  Its output function writes an empty string, which is
-- what a void column arrived as once only record was taken for a row.
DO $$
  const row = pljs.execute('SELECT pg_sleep(0) AS v, 1 AS w')[0];

  pljs.elog(NOTICE, JSON.stringify(row) + ' ' + ('v' in row) + ' ' +
            (row.v === undefined));
$$ LANGUAGE pljs;

-- 6) anyarray, compressed: its element type is read from the header alone.
CREATE TABLE ptv_wide AS
  SELECT repeat('x', 500) || g AS x FROM generate_series(1, 1000) AS g;
ANALYZE ptv_wide;
DO $$
  const h = pljs.execute("SELECT histogram_bounds AS h FROM pg_stats " +
                         "WHERE tablename = 'ptv_wide' AND attname = 'x'")[0].h;

  pljs.elog(NOTICE, Array.isArray(h) + ' ' + h.length + ' ' + h[0].length);
$$ LANGUAGE pljs;

SELECT 1 AS still_connected;

DROP TABLE ptv_wide;
DROP TABLE ptv_stats;
DROP FUNCTION ptv_records(record[]);
