-- The validator must validate the function being created, and must accept a pljs
-- body rather than requiring a standalone program.
--
-- Two defects, which masked each other:
--
--   1. It read fcinfo->flinfo->fn_oid -- its *own* OID -- and so fetched the
--      validator's pg_proc row, whose prosrc is the C symbol name
--      "pljs_call_validator".  That parses as a bare JavaScript identifier, so
--      validation always succeeded and any invalid body was accepted, with the
--      syntax error surfacing only on the first call.
--
--   2. Correcting the OID alone is not enough: a pljs body is a function *body*,
--      not a program, so `return 42;` is a syntax error at top level.  Validating
--      the raw prosrc rejects almost every valid function -- measured at 46 of 89
--      test files failing.  The body has to be wrapped the way compilation wraps
--      it, which is why the source builder is now shared with
--      pljs_compile_function().
CREATE EXTENSION IF NOT EXISTS pljs;

-- A syntactically invalid body is rejected at CREATE time.
\set VERBOSITY terse
CREATE FUNCTION val_bad() RETURNS int LANGUAGE pljs AS $$ this is ( not js $$;

-- An unterminated construct, likewise.
CREATE FUNCTION val_unclosed() RETURNS int LANGUAGE pljs AS $$ if (true) { $$;
\set VERBOSITY default

-- Valid bodies are accepted, including the shapes that a naive "validate the raw
-- prosrc" check would reject: a bare return, named arguments, and a trigger's
-- implicit variables.
CREATE FUNCTION val_return() RETURNS int LANGUAGE pljs AS $$ return 42; $$;
CREATE FUNCTION val_args(a int, b text) RETURNS text LANGUAGE pljs AS $$
  return b + a;
$$;
CREATE FUNCTION val_novoid() RETURNS void LANGUAGE pljs AS $$ pljs.elog(NOTICE, 'ok'); $$;

SELECT val_return() AS returns_ok, val_args(1, 'x') AS args_ok;

CREATE TABLE val_t (x int);
CREATE FUNCTION val_trigger() RETURNS trigger LANGUAGE pljs AS $$
  return NEW;
$$;
CREATE TRIGGER val_tr BEFORE INSERT ON val_t
  FOR EACH ROW EXECUTE FUNCTION val_trigger();

INSERT INTO val_t VALUES (1);
SELECT count(*)::int AS trigger_inserted FROM val_t;

-- check_function_bodies = off must skip validation entirely, which is what that
-- setting is for (restoring a dump whose functions reference objects not yet
-- created).
SET check_function_bodies = off;
CREATE FUNCTION val_deferred() RETURNS int LANGUAGE pljs AS $$ this is ( not js $$;
RESET check_function_bodies;

-- ... and the error then surfaces on first call instead.
\set VERBOSITY terse
SELECT val_deferred();
\set VERBOSITY default

-- Replacing a function repeatedly must not accumulate anything: the DDL path used
-- to reset the entire context cache, which could not free the old contexts, so a
-- loop of CREATE OR REPLACE grew the backend without bound and then crashed it.
-- 200 replacements is far too few to show the growth, but it does prove the
-- invalidation itself works -- each call must see the new body.
CREATE FUNCTION val_churn(n int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE i int; r int; bad int := 0;
BEGIN
  FOR i IN 1..n LOOP
    EXECUTE format('CREATE OR REPLACE FUNCTION val_v() RETURNS int LANGUAGE pljs AS $b$ return %s; $b$', i);
    EXECUTE 'SELECT val_v()' INTO r;
    IF r <> i THEN bad := bad + 1; END IF;
  END LOOP;
  RETURN bad;
END $$;

SELECT val_churn(200) AS stale_results;

DROP FUNCTION val_churn(int);
DROP FUNCTION val_v();
DROP TRIGGER val_tr ON val_t;
DROP FUNCTION val_trigger();
DROP TABLE val_t;
DROP FUNCTION val_return();
DROP FUNCTION val_args(int, text);
DROP FUNCTION val_novoid();
DROP FUNCTION val_deferred();
