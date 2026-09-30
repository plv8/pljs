-- A function's error is extracted while its SPI connection is still open.
--
-- Extracting the error runs JavaScript: the thrown value's toString(), and
-- getters for its message, detail and sqlstate.  call_function(),
-- call_trigger() and call_srf_function() closed the SPI connection first, as
-- they once did before converting the result, so pljs.commit() from there
-- dereferenced the missing connection and crashed the backend, and
-- pljs.execute() ran on whichever connection was current.

-- 1) A function.
CREATE FUNCTION ees_func() RETURNS int4 LANGUAGE pljs AS $$
  throw {
    toString() {
      try {
        pljs.commit();
      } catch (e) {
        pljs.elog(NOTICE, 'commit: ' + e.message);
      }
      return 'thrown from ees_func, ' +
             pljs.execute('SELECT 40 + 2 AS v')[0].v;
    }
  };
$$;
SELECT ees_func();

-- Uncaught, it is an error without a message, instead of a crashed backend.
CREATE FUNCTION ees_func_uncaught() RETURNS int4 LANGUAGE pljs AS $$
  throw { toString() { pljs.commit(); return 'x'; } };
$$;
SELECT ees_func_uncaught();

-- 2) A procedure, whose connection is nonatomic: the commit is allowed.
CREATE TABLE ees_log (n int4);
CREATE PROCEDURE ees_proc() LANGUAGE pljs AS $$
  pljs.execute('INSERT INTO ees_log VALUES (1)');
  throw {
    toString() {
      pljs.commit();
      pljs.execute('INSERT INTO ees_log VALUES (2)');
      return 'thrown from ees_proc';
    }
  };
$$;
CALL ees_proc();
SELECT n FROM ees_log ORDER BY n;

-- 3) A set-returning function.
CREATE FUNCTION ees_srf() RETURNS SETOF int4 LANGUAGE pljs AS $$
  throw {
    toString() {
      return 'thrown from ees_srf, ' +
             pljs.execute('SELECT 40 + 2 AS v')[0].v;
    }
  };
$$;
SELECT * FROM ees_srf();

-- 4) A trigger.
CREATE TABLE ees_tbl (a int4);
CREATE FUNCTION ees_trig() RETURNS trigger LANGUAGE pljs AS $$
  throw {
    toString() {
      return 'thrown from ees_trig, ' +
             pljs.execute('SELECT count(*)::int4 AS c FROM ees_tbl')[0].c;
    }
  };
$$;
CREATE TRIGGER ees_trig BEFORE INSERT ON ees_tbl
  FOR EACH ROW EXECUTE FUNCTION ees_trig();
INSERT INTO ees_tbl VALUES (1);

SELECT 1 AS still_connected;

DROP TABLE ees_log, ees_tbl;
DROP PROCEDURE ees_proc();
DROP FUNCTION ees_func(), ees_func_uncaught(), ees_srf(), ees_trig();
