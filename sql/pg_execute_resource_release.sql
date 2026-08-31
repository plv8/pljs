-- Resources acquired while running a query must be released when the query
-- finishes, whether it succeeded or failed.
--
-- Part 1: opening a cursor caught its error without rolling anything back.
-- Planning a statement takes a pg_proc syscache reference across constant
-- folding, so an error raised while folding escaped past its ReleaseSysCache()
-- and nothing released it.  It surfaced at the end of the statement as
-- "WARNING: resource was not closed: cache pg_proc".  The absence of that
-- warning below is the assertion; pljs.execute() already ran inside a
-- subtransaction and did not have the problem.
CREATE FUNCTION err_cursor() RETURNS text AS $$
  try {
    pljs.prepare('SELECT 1 / $1::int', ['int']).cursor([0]);
  } catch (e) {
    return 'caught: ' + e.message + ' [' + e.sqlstate + ']';
  }
  return 'no error';
$$ LANGUAGE pljs;

SELECT err_cursor();

-- A cursor opened the same way still works, still sees rows written earlier in
-- the same call, and survives the subtransaction it was opened in.
CREATE TABLE err_t (i int);

CREATE FUNCTION ok_cursor() RETURNS text AS $$
  pljs.execute('INSERT INTO err_t VALUES (1), (2), (3)');

  const c = pljs.prepare('SELECT i FROM err_t ORDER BY i').cursor();
  const seen = [];
  let row;

  while ((row = c.fetch())) {
    seen.push(row.i);
  }

  c.close();

  return 'fetched ' + JSON.stringify(seen);
$$ LANGUAGE pljs;

SELECT ok_cursor();
SELECT count(*) AS rows_committed FROM err_t;

-- Part 2: pljs.execute() left its result set and its per-call plan behind on
-- every call.  Both live in the SPI procedure context, which is only torn down
-- by SPI_finish() when the enclosing function returns, so a function looping
-- over pljs.execute() accumulated every result set and plan it had produced --
-- about 13KB a call, so roughly 660MB over 50,000 of them.
--
-- Measured from inside the loop's own call, because that is the only point at
-- which the context is still alive.  The check is a ratio rather than a size:
-- what matters is that the context does not grow with the number of queries.
CREATE FUNCTION spi_context_bytes(n int) RETURNS bigint AS $$
  for (let i = 0; i < n; i++) {
    pljs.execute('SELECT $1::int AS v', [i]);
  }

  return pljs.execute(
    "SELECT sum(total_bytes)::bigint AS b " +
    "FROM pg_backend_memory_contexts WHERE name LIKE 'SPI%'")[0].b;
$$ LANGUAGE pljs;

-- Flat in practice.  Allowing 4x leaves room for allocator noise while staying
-- far under the ~1400x this grew by before.
SELECT spi_context_bytes(10000) < spi_context_bytes(100) * 4 AS spi_context_bounded;

DROP FUNCTION err_cursor, ok_cursor, spi_context_bytes;
DROP TABLE err_t;
