-- A function's result is converted while its SPI connection is still open.
--
-- Converting a result can run JavaScript: a getter, valueOf() or toString()
-- on what the function returned.  call_function(), call_trigger() and
-- call_srf_function() ran that conversion after SPI_finish(), so JavaScript
-- run from it had no SPI connection of its own:
--
--   - pljs.commit() dereferenced the missing connection and crashed the
--     backend, on main as well, and
--   - pljs.execute() ran on whichever connection was current -- the caller's,
--     for a pljs function called from a PL/pgSQL query.
--
-- A procedure still cannot end its transaction from there: the conversion
-- holds resources of the transaction it started in.

-- 1) COMMIT and ROLLBACK from a getter on a procedure's result are refused.
CREATE PROCEDURE rcs_commit(INOUT x int4, INOUT y int4) LANGUAGE pljs AS $$
  return {
    get x() {
      try {
        pljs.commit();
      } catch (e) {
        pljs.elog(NOTICE, e.message);
      }
      return 1;
    },
    y: 2
  };
$$;
CALL rcs_commit(NULL, NULL);

CREATE PROCEDURE rcs_rollback(INOUT x int4, INOUT y int4) LANGUAGE pljs AS $$
  return {
    get x() {
      try {
        pljs.rollback();
      } catch (e) {
        pljs.elog(NOTICE, e.message);
      }
      return 1;
    },
    y: 2
  };
$$;
CALL rcs_rollback(NULL, NULL);

-- Uncaught, the refusal is the error, instead of a crashed backend.
CREATE PROCEDURE rcs_commit_uncaught(INOUT x int4, INOUT y int4)
  LANGUAGE pljs AS $$
  return { get x() { pljs.commit(); return 1; }, y: 2 };
$$;
CALL rcs_commit_uncaught(NULL, NULL);
SELECT 1 AS still_connected;

-- The procedure's own COMMIT still works.
CREATE TABLE rcs_log (n int4);
CREATE PROCEDURE rcs_body_commit(INOUT x int4) LANGUAGE pljs AS $$
  pljs.execute('INSERT INTO rcs_log VALUES (1)');
  pljs.commit();
  return { x: 1 };
$$;
CALL rcs_body_commit(NULL);
SELECT count(*) FROM rcs_log;

-- 2) A query from a getter on a function's result, with the function called
-- from a PL/pgSQL query: it ran on PL/pgSQL's SPI connection.
CREATE FUNCTION rcs_getter(OUT a int4, OUT b int4) LANGUAGE pljs AS $$
  return { get a() { return pljs.execute('SELECT 40 + 2 AS v')[0].v; }, b: 2 };
$$;
DO $$
DECLARE
  s int4;
BEGIN
  SELECT sum((rcs_getter()).a) INTO s FROM generate_series(1, 3);
  RAISE NOTICE 'sum %', s;
END $$;

-- 3) The same from a trigger's NEW.
CREATE TABLE rcs_tbl (a int4, b int4);
CREATE FUNCTION rcs_trig() RETURNS trigger LANGUAGE pljs AS $$
  return {
    a: NEW.a,
    get b() {
      return pljs.execute('SELECT count(*)::int4 AS c FROM rcs_tbl')[0].c;
    }
  };
$$;
CREATE TRIGGER rcs_trig BEFORE INSERT ON rcs_tbl
  FOR EACH ROW EXECUTE FUNCTION rcs_trig();
INSERT INTO rcs_tbl (a) VALUES (1), (2), (3);
SELECT * FROM rcs_tbl ORDER BY a;

-- 4) And from rows a set-returning function returns rather than passes to
-- return_next().
CREATE FUNCTION rcs_srf() RETURNS SETOF int4 LANGUAGE pljs AS $$
  return [{ valueOf() { return pljs.execute('SELECT 7 AS v')[0].v; } }];
$$;
SELECT * FROM rcs_srf();

DROP TABLE rcs_log, rcs_tbl;
DROP PROCEDURE rcs_commit, rcs_rollback, rcs_commit_uncaught, rcs_body_commit;
DROP FUNCTION rcs_getter, rcs_trig, rcs_srf;
