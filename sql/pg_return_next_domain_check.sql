-- return_next() rolls back what a failed domain CHECK left behind.
--
-- return_next() hands an error from converting its row to JavaScript, and
-- converting a row of a domain checks the domain's constraints, which can run
-- any SQL.  A PL/pgSQL CHECK function that raised, or a pljs one whose result
-- could not be converted, left its SPI connection on the stack, and the
-- set-returning function carried on with it as its own.

-- 1) A pljs CHECK function whose result cannot be converted.
CREATE FUNCTION rnd_g(x int4) RETURNS int4 LANGUAGE pljs AS $$
  return 'not a number';
$$;
CREATE DOMAIN rnd_d AS int4 CHECK (rnd_g(VALUE) > 0);
CREATE FUNCTION rnd_f() RETURNS SETOF rnd_d LANGUAGE pljs AS $$
  try {
    pljs.return_next(5);
  } catch (e) {
    pljs.elog(NOTICE, 'return_next: ' + e.message);
  }
  pljs.elog(NOTICE, 'execute: ' + pljs.execute('SELECT 42 AS x')[0].x);
$$;
SELECT * FROM rnd_f();

-- From PL/pgSQL, whose query result went to the stale connection.
DO $$
DECLARE
  n int4;
BEGIN
  SELECT count(*) INTO n FROM rnd_f();
  RAISE NOTICE 'count %', n;
END $$;

-- From pljs.execute().
DO LANGUAGE pljs $$
  pljs.elog(NOTICE, 'rows ' +
            pljs.execute('SELECT count(*)::int4 AS n FROM rnd_f()')[0].n);
$$;

-- 2) A PL/pgSQL CHECK function that raises.
CREATE FUNCTION rnd_positive(x int4) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  IF x <= 0 THEN
    RAISE EXCEPTION 'not positive: %', x;
  END IF;
  RETURN true;
END $$;
CREATE DOMAIN rnd_pos AS int4 CHECK (rnd_positive(VALUE));
CREATE FUNCTION rnd_pos_set() RETURNS SETOF rnd_pos LANGUAGE pljs AS $$
  for (const v of [1, -2, 3]) {
    try {
      pljs.return_next(v);
    } catch (e) {
      pljs.elog(NOTICE, 'return_next: ' + e.message);
    }
  }
$$;
SELECT * FROM rnd_pos_set();

-- In a column of a composite set, and in an array.
CREATE TYPE rnd_row AS (id int4, v rnd_pos);
CREATE FUNCTION rnd_rows() RETURNS SETOF rnd_row LANGUAGE pljs AS $$
  for (const v of [1, -2, 3]) {
    try {
      pljs.return_next({id: v, v: v});
    } catch (e) {
      pljs.elog(NOTICE, 'return_next: ' + e.message);
    }
  }
$$;
SELECT * FROM rnd_rows();

CREATE FUNCTION rnd_arrays() RETURNS SETOF rnd_pos[] LANGUAGE pljs AS $$
  for (const v of [[1, 2], [3, -4], [5]]) {
    try {
      pljs.return_next(v);
    } catch (e) {
      pljs.elog(NOTICE, 'return_next: ' + e.message);
    }
  }
$$;
SELECT * FROM rnd_arrays();

-- And in a range, and a multirange, of the domain: a range's input function
-- runs its subtype's, which checks the domain.  Ranges were not looked into,
-- so their rows were converted without a subtransaction, and a failed CHECK
-- left "transaction left non-empty SPI stack".
CREATE TYPE rnd_range AS RANGE (subtype = rnd_pos);
CREATE FUNCTION rnd_ranges() RETURNS SETOF rnd_range LANGUAGE pljs AS $$
  for (const v of ['[1,5)', '[-2,3)', '[3,4)']) {
    try {
      pljs.return_next(v);
    } catch (e) {
      pljs.elog(NOTICE, 'return_next: ' + e.message);
    }
  }
  pljs.elog(NOTICE, 'execute: ' + pljs.execute('SELECT 42 AS x')[0].x);
$$;
SELECT * FROM rnd_ranges();

CREATE FUNCTION rnd_multiranges() RETURNS SETOF rnd_multirange LANGUAGE pljs AS $$
  for (const v of ['{[1,5)}', '{[3,4), [-2,3)}', '{[6,7)}']) {
    try {
      pljs.return_next(v);
    } catch (e) {
      pljs.elog(NOTICE, 'return_next: ' + e.message);
    }
  }
$$;
SELECT * FROM rnd_multiranges();

-- 3) Only a CHECK constraint can run SQL, so only a domain with one needs a
-- subtransaction per row; one with NOT NULL alone still rejects a NULL.
CREATE DOMAIN rnd_nn AS int4 NOT NULL;
CREATE FUNCTION rnd_nn_set() RETURNS SETOF rnd_nn LANGUAGE pljs AS $$
  for (const v of [1, null, 3]) {
    try {
      pljs.return_next(v);
    } catch (e) {
      pljs.elog(NOTICE, 'return_next: ' + e.message);
    }
  }
  pljs.elog(NOTICE, 'execute: ' + pljs.execute('SELECT 42 AS x')[0].x);
$$;
SELECT * FROM rnd_nn_set();

-- A CHECK constraint added while the set is being returned is seen, and the
-- rows after it are converted in a subtransaction.
CREATE DOMAIN rnd_late AS int4;
CREATE FUNCTION rnd_late_set() RETURNS SETOF rnd_late LANGUAGE pljs AS $$
  pljs.return_next(1);
  pljs.execute('ALTER DOMAIN rnd_late ADD CONSTRAINT rnd_late_positive ' +
               'CHECK (rnd_positive(VALUE))');
  for (const v of [-2, 3]) {
    try {
      pljs.return_next(v);
    } catch (e) {
      pljs.elog(NOTICE, 'return_next: ' + e.message);
    }
  }
  pljs.elog(NOTICE, 'execute: ' + pljs.execute('SELECT 42 AS x')[0].x);
$$;
SELECT * FROM rnd_late_set();

SELECT 1 AS still_connected;

DROP FUNCTION rnd_f(), rnd_pos_set(), rnd_rows(), rnd_arrays(), rnd_ranges(),
  rnd_multiranges(), rnd_nn_set(), rnd_late_set();
DROP TYPE rnd_row, rnd_range;
DROP DOMAIN rnd_d, rnd_pos, rnd_nn, rnd_late;
DROP FUNCTION rnd_g(int4), rnd_positive(int4);
