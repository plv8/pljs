-- Builtins in parallel mode.
--
-- PostgreSQL before 17 cannot begin a subtransaction in parallel mode.
-- return_next() began one for each row of a domain with a CHECK constraint,
-- so every row of a PARALLEL SAFE set of one failed there.  And
-- pljs.find_function() compiles without one there: when the function's
-- top-level code threw and JavaScript caught the error, the SPI connection
-- the compile had opened was left on the stack, and the transaction ended
-- with "transaction left non-empty SPI stack".  And cursor.fetch() rolled back
-- a subtransaction it had failed to begin: the caller's, and the backend
-- crashed.
--
-- Where no subtransaction can be begun, any error converting a row that
-- needs one ends the call -- one JavaScript raises, too, as a set of several
-- columns raises it -- as JavaScript cannot be let catch it: what a
-- CHECK constraint held when it raised -- the SPI connection of a PL/pgSQL
-- function it called -- is left, and JavaScript that caught the error went on
-- with it on top of the stack.  A PL/pgSQL block that catches an error cannot
-- run there at all.  Telling the errors that leave something from those that
-- do not took more than could be told: a domain's input function parses and
-- checks as one, and a constraint's function can raise any error.
-- PostgreSQL before 17 has an output of its own for that,
-- pg_parallel_mode_1.out; everything else is the same.

CREATE DOMAIN pm_positive AS int4 CHECK (VALUE > 0);

CREATE FUNCTION pm_rows() RETURNS SETOF pm_positive LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next(1);
  pljs.return_next(2);
$$;

CREATE FUNCTION pm_bad_row() RETURNS SETOF pm_positive LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next(1);

  try {
    pljs.return_next(-2);
  } catch (e) {
    pljs.return_next(3);
  }
$$;

CREATE FUNCTION pm_bad_input() RETURNS SETOF pm_positive LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next(1);

  try {
    pljs.return_next('not a number');
  } catch (e) {
    pljs.return_next(3);
  }
$$;

CREATE FUNCTION pm_bad_object() RETURNS SETOF pm_positive LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next(1);

  try {
    pljs.return_next({x: 2, y: 2});
  } catch (e) {
    pljs.return_next(3);
  }
$$;

CREATE FUNCTION pm_throwing_getter() RETURNS SETOF pm_positive LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next(1);

  try {
    pljs.return_next({get toString() { throw new Error('getter'); }});
  } catch (e) {
    pljs.return_next(3);
  }
$$;

CREATE DOMAIN pm_inet AS inet CHECK (family(VALUE) = 4);

CREATE FUNCTION pm_bad_inet() RETURNS SETOF pm_inet LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next('10.0.0.1');

  try {
    pljs.return_next('garbage');
  } catch (e) {
    pljs.return_next('10.0.0.3');
  }
$$;

CREATE DOMAIN pm_not_null AS int4 NOT NULL CHECK (VALUE > 0);

CREATE FUNCTION pm_null_row() RETURNS SETOF pm_not_null LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next(1);

  try {
    pljs.return_next(null);
  } catch (e) {
    pljs.return_next(3);
  }
$$;

CREATE FUNCTION pm_check(v int4) RETURNS bool LANGUAGE plpgsql
PARALLEL SAFE AS $$
BEGIN
  IF v <= 0 THEN
    RAISE EXCEPTION 'not positive: %', v;
  END IF;

  RETURN true;
END $$;

CREATE DOMAIN pm_checked AS int4 CHECK (pm_check(VALUE));

CREATE FUNCTION pm_checked_rows() RETURNS SETOF pm_checked LANGUAGE pljs
PARALLEL SAFE AS $$
  pljs.return_next(1);

  try {
    pljs.return_next(-2);
  } catch (e) {
    pljs.return_next(3);
  }
$$;

CREATE FUNCTION pm_throws() RETURNS int4 LANGUAGE pljs PARALLEL SAFE AS $$
  return 1;
} throw new Error('top level'); function pm_unused() {
$$;

CREATE FUNCTION pm_find() RETURNS text LANGUAGE pljs PARALLEL SAFE AS $$
  try {
    pljs.find_function('pm_throws');
    return 'found';
  } catch (e) {
    return 'caught';
  }
$$;

SET debug_parallel_query = on;

SELECT * FROM pm_rows();

SELECT * FROM pm_bad_input();
SELECT * FROM pm_bad_object();
SELECT * FROM pm_throwing_getter();
SELECT * FROM pm_bad_inet();
SELECT * FROM pm_null_row();

\set VERBOSITY terse
SELECT * FROM pm_bad_row();
SELECT * FROM pm_checked_rows();
\set VERBOSITY default

SELECT pm_find();

RESET debug_parallel_query;

-- A cursor opened before, fetched from the leader, with a savepoint of the
-- caller's open.
CREATE TABLE pm_table AS SELECT 1 AS i;
CREATE TABLE pm_saved (x int4);

CREATE FUNCTION pm_open() RETURNS void LANGUAGE pljs AS $$
  globalThis.pm_cursor = pljs.prepare('SELECT 1 AS x').cursor();
$$;

CREATE FUNCTION pm_fetch(i int4) RETURNS text LANGUAGE pljs
PARALLEL RESTRICTED AS $$
  try {
    globalThis.pm_cursor.fetch();
  } catch (e) {
    // Where no subtransaction can be begun.
  }

  return 'fetched';
$$;

BEGIN;
SELECT pm_open();
SAVEPOINT pm_savepoint;
INSERT INTO pm_saved VALUES (1);
SET LOCAL debug_parallel_query = on;
SELECT pm_fetch(i) FROM pm_table;
SELECT count(*) AS saved FROM pm_saved;
RELEASE SAVEPOINT pm_savepoint;
COMMIT;

SELECT 1 AS still_connected;

DROP FUNCTION pm_fetch(int4), pm_open(), pm_find(), pm_throws(),
  pm_checked_rows(), pm_null_row(), pm_bad_inet(), pm_throwing_getter(),
  pm_bad_object(),
  pm_bad_input(),
  pm_bad_row(), pm_rows();
DROP TABLE pm_saved, pm_table;
DROP DOMAIN pm_positive, pm_checked, pm_inet, pm_not_null;
DROP FUNCTION pm_check(int4);
