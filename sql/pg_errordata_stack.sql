-- Every PG_CATCH that reports a Postgres error to JavaScript must call
-- FlushErrorState().  Returning without it leaves the entry on the errordata
-- stack, which is only ERRORDATA_STACK_SIZE (5) deep and is not unwound until
-- the enclosing statement ends, so the sixth caught error inside a single call
-- raises "PANIC: ERRORDATA_STACK_SIZE exceeded" -- killing every backend in the
-- cluster, not just this session.
--
-- pljs_execute() was fixed for this earlier; pljs.commit(), pljs.rollback(),
-- pljs.find_function() and plan.cursor() were not.  Each loop below caught 12
-- errors, well past the limit, and PANICked.  A mirror procedure that retries a
-- failing commit (deadlock, serialization failure, full disk) reaches this.
--
-- These handlers also used to discard the real error behind a fixed string
-- ("Unable to commit", "Error executing"), so the tests assert the actual
-- Postgres message survives.
CREATE EXTENSION IF NOT EXISTS pljs;

-- 1) pljs.commit() inside a non-atomic context fails every time.
CREATE FUNCTION errstack_commit(n int) RETURNS text AS $$
  var caught = 0, last = '';
  for (var i = 0; i < n; i++) {
    try { pljs.commit(); } catch (e) { caught++; last = e.message; }
  }
  return caught + ' caught, last: ' + last;
$$ LANGUAGE pljs;

SELECT errstack_commit(12);

-- 2) pljs.rollback(), same context.
CREATE FUNCTION errstack_rollback(n int) RETURNS text AS $$
  var caught = 0, last = '';
  for (var i = 0; i < n; i++) {
    try { pljs.rollback(); } catch (e) { caught++; last = e.message; }
  }
  return caught + ' caught, last: ' + last;
$$ LANGUAGE pljs;

SELECT errstack_rollback(12);

-- 3) pljs.find_function() on a name that does not exist.
CREATE FUNCTION errstack_find_function(n int) RETURNS text AS $$
  var caught = 0, last = '';
  for (var i = 0; i < n; i++) {
    try { pljs.find_function('pljs_no_such_function'); }
    catch (e) { caught++; last = e.message; }
  }
  return caught + ' caught, last: ' + last;
$$ LANGUAGE pljs;

SELECT errstack_find_function(12);

-- 4) plan.cursor(): opening a cursor runs the query's start-up, so it fails
-- here on division by zero.  Two things are asserted: the loop survives, and
-- the real error reaches JavaScript instead of the old opaque "Error executing".
--
-- The open path also had no internal subtransaction, so everything each failed
-- attempt acquired leaked until end of transaction; 200 iterations used to emit
-- 200 "WARNING: resource was not closed: cache pg_proc ... has count N" lines.
-- A clean result here is the regression signal for that too.
CREATE FUNCTION errstack_cursor(n int) RETURNS text AS $$
  var caught = 0, last = '';
  var plan = pljs.prepare('SELECT 1 / $1::int AS v', ['int']);
  for (var i = 0; i < n; i++) {
    try { plan.cursor([0]); } catch (e) { caught++; last = e.message; }
  }
  plan.free();
  return caught + ' caught, last: ' + last;
$$ LANGUAGE pljs;

SELECT errstack_cursor(200);

-- A cursor that opens successfully still works, and is still usable after the
-- subtransaction that guarded its open was released.
DO $$
  var plan = pljs.prepare('SELECT g FROM generate_series(1, 5) g WHERE g > $1', ['int']);
  var cursor = plan.cursor([2]);
  var rows = [], row;

  while ((row = cursor.fetch()) !== undefined) {
    rows.push(row.g);
  }

  cursor.close();
  plan.free();
  pljs.elog(NOTICE, 'cursor rows after guarded open: ' + rows.join(','));
$$ LANGUAGE pljs;

-- The backend is still alive and the errordata stack is clean.
SELECT 'backend still usable' AS status, 1 + 1 AS math;

DROP FUNCTION errstack_commit(int);
DROP FUNCTION errstack_rollback(int);
DROP FUNCTION errstack_find_function(int);
DROP FUNCTION errstack_cursor(int);
