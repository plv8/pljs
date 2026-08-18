-- Regression: a PostgreSQL error raised inside pljs.return_next() escaped past
-- JavaScript instead of arriving as a catchable exception.
--
-- return_next is a C function that QuickJS called, so QuickJS has live
-- JSStackFrame structures on the C stack between it and the interpreter, linked
-- from the runtime.  An ereport(ERROR) there siglongjmps straight past them: any
-- JavaScript try/catch around the call never runs, and the runtime is left with
-- rt->current_stack_frame pointing at frames that no longer exist.
--
-- pljs.execute() has always converted PostgreSQL errors into JavaScript
-- exceptions for exactly this reason.  return_next now does the same.
--
-- The error used below is the one an array-typed column raises when the property
-- it is given is not an array: an ordinary ereport from inside the conversion,
-- which needs no other change to reach.
CREATE TYPE rnx_row AS (a int[]);

CREATE FUNCTION rnx_bad_srf() RETURNS SETOF rnx_row AS $$
  pljs.return_next({ a: 'not-an-array' });
$$ LANGUAGE pljs;

-- Still reported to the caller.
SELECT count(*) FROM rnx_bad_srf();

-- And now catchable in JavaScript, which is the part that changed: without the
-- guard the longjmp skips this catch entirely and the error surfaces in SQL.
CREATE FUNCTION rnx_caught() RETURNS SETOF rnx_row AS $$
  try {
    pljs.return_next({ a: 'not-an-array' });
  } catch (e) {
    pljs.elog(NOTICE, 'caught in JavaScript: ' + e.message);
  }
$$ LANGUAGE pljs;

SELECT count(*) FROM rnx_caught();

-- Rows emitted before the failure survive it, and the set still ends cleanly.
CREATE FUNCTION rnx_partial() RETURNS SETOF rnx_row AS $$
  pljs.return_next({ a: [1, 2] });
  try {
    pljs.return_next({ a: 'not-an-array' });
  } catch (e) {
    pljs.elog(NOTICE, 'second row rejected: ' + e.message);
  }
  pljs.return_next({ a: [3] });
$$ LANGUAGE pljs;

SELECT * FROM rnx_partial();

-- Constructing an Error afterwards walks the runtime's frame list, so it is the
-- operation that would fault on a list left dangling by the longjmp.
CREATE FUNCTION rnx_after() RETURNS text AS $$
  try { throw new Error('after'); } catch (e) { return 'frame list intact: ' + e.message; }
$$ LANGUAGE pljs;

SELECT rnx_after();

SELECT 1 AS alive;

DROP FUNCTION rnx_bad_srf, rnx_caught, rnx_partial, rnx_after;
DROP TYPE rnx_row;
