-- Resource limits on custom language handlers: a runaway or memory-hungry
-- transpiler must be stopped without damaging the backend, and a chain of
-- handlers is bounded by PLJS_MAX_LANG_HANDLER_DEPTH (32).
CREATE EXTENSION IF NOT EXISTS pljs;

CREATE FUNCTION ok_fn() RETURNS int AS $$ return 7; $$ LANGUAGE pljs;

-- 1. A handler that never returns is cancelled by statement_timeout.
CREATE FUNCTION loop_handler() RETURNS language_handler AS $$
  while (true) {}
$$ LANGUAGE pljs;

CREATE TRUSTED LANGUAGE looplang
  HANDLER loop_handler
  INLINE pljs_inline_handler
  VALIDATOR pljs_call_validator;

-- During a call (the validator is skipped so the function can be created).
SET check_function_bodies = off;
CREATE FUNCTION loop_fn() RETURNS int AS $$ return 1; $$ LANGUAGE looplang;
RESET check_function_bodies;

SET statement_timeout = '500ms';
SELECT loop_fn();
-- The failed compilation is not cached; it is cancelled again.
SELECT loop_fn();
-- During CREATE FUNCTION validation.
CREATE FUNCTION loop_fn2() RETURNS int AS $$ return 1; $$ LANGUAGE looplang;
-- During an inline block.
DO $$ return 1; $$ LANGUAGE looplang;
RESET statement_timeout;

-- The backend is still usable, and nothing was left behind.
SELECT ok_fn();
SELECT count(*) AS loop_fn2_exists FROM pg_proc WHERE proname = 'loop_fn2';

-- 2. A handler that exhausts pljs.memory_limit is aborted.
CREATE FUNCTION mem_handler() RETURNS language_handler AS $$
  const s = [];
  while (true) {
    s.push(new ArrayBuffer(63));
  }
$$ LANGUAGE pljs;

CREATE TRUSTED LANGUAGE memlang
  HANDLER mem_handler
  INLINE pljs_inline_handler
  VALIDATOR pljs_call_validator;

SET pljs.memory_limit = 64;
SET check_function_bodies = off;
CREATE FUNCTION mem_fn() RETURNS int AS $$ return 1; $$ LANGUAGE memlang;
RESET check_function_bodies;

SELECT mem_fn();
CREATE FUNCTION mem_fn2() RETURNS int AS $$ return 1; $$ LANGUAGE memlang;
RESET pljs.memory_limit;

SELECT ok_fn();

-- 3. Handler chains are bounded.  Handler k0 is written in pljs; handler kN is
-- written in the language handled by k(N-1).
SELECT format('CREATE FUNCTION k%s() RETURNS language_handler AS $q$ return arguments[0]; $q$ LANGUAGE %s',
              i, CASE WHEN i = 0 THEN 'pljs' ELSE 'chain' || (i - 1) END),
       format('CREATE TRUSTED LANGUAGE chain%s HANDLER k%s INLINE pljs_inline_handler VALIDATOR pljs_call_validator',
              i, i)
FROM generate_series(0, 32) AS i
\gexec

-- 32 handlers deep (the limit) works.
CREATE FUNCTION depth_ok() RETURNS int AS $$ return 32; $$ LANGUAGE chain31;
SELECT depth_ok();

DO $$ pljs.elog(NOTICE, "deepest inline block works"); $$ LANGUAGE chain31;

-- One more is refused.
DO $$
BEGIN
  CREATE FUNCTION depth_too_deep() RETURNS int AS 'return 33;' LANGUAGE chain32;
  RAISE EXCEPTION 'expected the handler chain to be refused';
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE '%', SQLERRM;
END $$;

SELECT count(*) AS depth_too_deep_exists FROM pg_proc WHERE proname = 'depth_too_deep';

-- Shallower chains are unaffected.
SELECT depth_ok();
SELECT ok_fn();

-- Cleanup
DROP FUNCTION depth_ok();
SET client_min_messages = warning;
DROP LANGUAGE chain0 CASCADE;
RESET client_min_messages;
DROP FUNCTION k0();
DROP FUNCTION IF EXISTS loop_fn();
DROP LANGUAGE looplang;
DROP FUNCTION loop_handler();
DROP FUNCTION mem_fn();
DROP LANGUAGE memlang;
DROP FUNCTION mem_handler();
DROP FUNCTION ok_fn();
