-- Regression for a backend PANIC in ordinary try/catch code.
--
-- PG_CATCH() restores PG_exception_stack, but it does not pop the errordata
-- stack -- only FlushErrorState() does that.  Every PG_CATCH() in functions.c
-- that returned a thrown JS value without flushing therefore consumed one of
-- the five slots ERRORDATA_STACK_SIZE provides, permanently, for the life of
-- the session.  The fifth error caught this way raised "ERRORDATA_STACK_SIZE
-- exceeded" at PANIC level, which restarts every backend in the cluster, not
-- just the one that ran the query.
--
-- Every loop below runs well past five, and they all run in one session on
-- purpose: the errordata stack is per-backend, so splitting these across
-- sessions would hide the bug.  A single unflushed site fails the test.
DO $$
  const n = 20;

  // pljs.execute() -- the subtransaction path.
  for (let i = 0; i < n; i++) {
    try { pljs.execute('SELECT * FROM ces_no_such_table'); } catch (e) { }
  }
  pljs.elog(NOTICE, 'execute: survived ' + n + ' caught errors');

  // plan.execute() -- the prepared-plan path, failing at run time.
  for (let i = 0; i < n; i++) {
    try { pljs.prepare('SELECT 1 / $1::int', ['int']).execute([0]); } catch (e) { }
  }
  pljs.elog(NOTICE, 'plan.execute: survived ' + n + ' caught errors');

  // pljs.prepare() -- failing while preparing.
  for (let i = 0; i < n; i++) {
    try { pljs.prepare('SELECT * FROM ces_no_such_table'); } catch (e) { }
  }
  pljs.elog(NOTICE, 'prepare: survived ' + n + ' caught errors');

  // pljs.find_function() -- the PG_CATCH() next to the PG_TRY() fixed above.
  for (let i = 0; i < n; i++) {
    try { pljs.find_function('ces_no_such_function()'); } catch (e) { }
  }
  pljs.elog(NOTICE, 'find_function: survived ' + n + ' caught errors');

  // pljs.commit() / pljs.rollback(), neither of which is legal here.
  for (let i = 0; i < n; i++) {
    try { pljs.commit(); } catch (e) { }
    try { pljs.rollback(); } catch (e) { }
  }
  pljs.elog(NOTICE, 'commit/rollback: survived ' + (n * 2) + ' caught errors');
$$ LANGUAGE pljs;

-- The backend is still up, and so is the rest of the cluster.
SELECT 1 AS alive;
