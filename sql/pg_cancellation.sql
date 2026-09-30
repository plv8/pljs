-- Regression: pljs must honor query cancellation / statement_timeout.
--
-- Previously _PG_init() installed its own signal(SIGINT/SIGTERM/SIGABRT)
-- handlers, clobbering PostgreSQL's own signal handling for the whole backend,
-- and the QuickJS interrupt handler only consulted pljs's private
-- os_pending_signals bitmask.  statement_timeout is delivered via SIGALRM ->
-- QueryCancelPending, which that bitmask never saw, so a runaway JS loop could
-- not be cancelled and the backend hung forever.  The interrupt handler now
-- reads QueryCancelPending/ProcDiePending directly, and each caller re-raises
-- the real error via CHECK_FOR_INTERRUPTS() once QuickJS has unwound to an
-- exception.  These tests must complete (with a cancel error) instead of
-- hanging.
CREATE EXTENSION IF NOT EXISTS pljs;

-- A runaway loop in an anonymous DO block (call_anonymous_function path) must
-- be cancelled by statement_timeout.
SET statement_timeout = '400ms';
DO $$ var x = 0; while (true) { x++; } $$ LANGUAGE pljs;
RESET statement_timeout;

-- ... and in a regular function (call_function path).
CREATE FUNCTION spin() RETURNS int LANGUAGE pljs AS $$
  var x = 0;
  while (true) { x++; }
  return x;
$$;
SET statement_timeout = '400ms';
SELECT spin();
RESET statement_timeout;

-- The backend survived both cancellations and is still usable.
SELECT 1 AS alive;

-- A normal JS error must NOT be masked by the interrupt check (no cancel is
-- pending, so CHECK_FOR_INTERRUPTS() is a no-op and the real JS error surfaces).
DO $$
  try { throw new Error('plain error'); }
  catch (e) { pljs.elog(NOTICE, 'caught: ' + e.message); }
$$ LANGUAGE pljs;

DROP FUNCTION spin();
