-- NB: some expected output below is QuickJS's own wording ("out of memory",
-- "stack overflow"), which is not part of any stable interface -- it is pinned
-- by the vendored deps/quickjs revision and will churn if that is bumped.  If a
-- QuickJS upgrade fails here, check the message text before assuming a
-- behaviour regression.
-- Regression: deep / unbounded JS recursion must raise a catchable
-- "stack overflow" error and leave the backend alive -- never crash it with a
-- C-stack SIGSEGV.
--
-- QuickJS only performs stack checks when built with CONFIG_STACK_CHECK, and
-- even then pljs previously relied on the vendored JS_DEFAULT_STACK_SIZE and
-- never set a limit explicitly.  pljs now sets an explicit QuickJS stack budget
-- (half of max_stack_depth, floored) in _PG_init, so pure-JS recursion trips
-- QuickJS's guard well before PostgreSQL's own C-stack limit and the kernel
-- stack limit are reached.
--
-- The derived bound is observable, and the section at the end of this file checks it.
--
-- It reads as though it could not be: the budget is derived once in _PG_init, so a
-- later SET cannot change it.  But _PG_init runs when the library is first loaded in
-- a given backend, which is the first pljs call in that session -- so a SET issued
-- before any pljs function is called is exactly what it reads.  \c gives a fresh
-- backend per setting, which is how the section below compares two of them.
--
-- Measured on PostgreSQL 17, the achievable pure-JS recursion depth tracks the
-- derived budget almost exactly linearly:
--
--     max_stack_depth   derived JS budget   depth reached
--     512kB             256kB (the floor)   268
--     1024kB            512kB               537
--     2048kB (default)  1024kB              1074
--     4096kB            2048kB              2148
--
-- At the 2048kB default the derived budget is 1MB, which is what
-- JS_DEFAULT_STACK_SIZE already is, so at default settings alone this file would
-- pass with or without the explicit JS_SetMaxStackSize() call.  That is exactly why
-- the comparison below uses two different settings.
CREATE EXTENSION IF NOT EXISTS pljs;

-- Unbounded self-recursion in an anonymous block.  Use terse verbosity: the
-- InternalError stack trace carried in DETAIL is machine dependent (its length
-- depends on frame size), so it must not appear in the portable expected file.
\set VERBOSITY terse
DO $$ function r(n) { return r(n + 1); } r(0); $$ LANGUAGE pljs;
\set VERBOSITY default

-- The backend survived the overflow and is still usable.
SELECT 1 AS alive;

-- The overflow surfaces as an ordinary, catchable JS error.
CREATE FUNCTION deep_recurse() RETURNS text LANGUAGE pljs AS $$
  function r(n) { return r(n + 1); }
  try { r(0); }
  catch (e) { return "caught: " + e.name + ": " + e.message; }
  return "no error";
$$;
SELECT deep_recurse();

-- Mutual JS<->SQL recursion is bounded as well: PostgreSQL's check_stack_depth
-- stops it and the error is caught and unwound at every level, so the top-level
-- call returns cleanly and the backend stays alive.
CREATE FUNCTION mutual() RETURNS text LANGUAGE pljs AS $$
  try { return pljs.execute("SELECT mutual()")[0].mutual; }
  catch (e) { return "depth-limited"; }
$$;
SELECT mutual();
SELECT 1 AS alive_after_mutual;

DROP FUNCTION mutual();
DROP FUNCTION deep_recurse();

-- ---------------------------------------------------------------------------
-- The derived budget is observable: a lower max_stack_depth must yield a
-- proportionally lower achievable JS recursion depth.
--
-- Absolute depths depend on per-frame stack usage and so vary by platform and build
-- flags; the assertion is the ratio, which is a property of the derivation.
-- ---------------------------------------------------------------------------
CREATE TABLE sd_depths (label text, d int);

CREATE FUNCTION sd_probe() RETURNS int LANGUAGE pljs AS $$
  let d = 0;
  function r() { d++; r(); }
  try { r(); } catch (e) { /* QuickJS stack guard */ }
  return d;
$$;

-- Fresh backend, so _PG_init has not run yet and the SET below is what it reads.
\c
SET max_stack_depth = 512;
INSERT INTO sd_depths SELECT 'low', sd_probe();

\c
SET max_stack_depth = 4096;
INSERT INTO sd_depths SELECT 'high', sd_probe();

-- 512kB floors the budget at 256kB, 4096kB gives 2048kB: an 8x budget, so the depth
-- should be several times larger.  Asserted loosely (>3x) so that a platform with
-- different frame sizes still passes while a build that ignores max_stack_depth
-- entirely -- where both depths would be identical -- fails.
SELECT (SELECT d FROM sd_depths WHERE label = 'high')
       > 3 * (SELECT d FROM sd_depths WHERE label = 'low') AS budget_tracks_max_stack_depth;

DROP FUNCTION sd_probe();
DROP TABLE sd_depths;
