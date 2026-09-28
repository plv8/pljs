-- A value that cannot be converted is released all the same.
--
-- A value read from JavaScript was released once its conversion returned, so
-- a conversion that raised left it in the runtime for the life of the
-- backend: a function's result, a row a set-returning function returned, a
-- trigger's NEW, an array's element, a record's column, a query's parameter,
-- and every array and object a jsonb value was inside.  The domain and typmod
-- checks raise for ordinary bad input, and a loop that caught the errors ran
-- pljs.memory_limit out.  Each case below fails 2,000 times on a value of
-- about 20kB, which leaked 40MB and more.

CREATE FUNCTION cel_mb() RETURNS float8 LANGUAGE sql AS $$
  SELECT (pljs_info() ->> 'malloc_size')::float8 / (1024 * 1024);
$$;
CREATE FUNCTION cel_gc() RETURNS void LANGUAGE pljs AS $$ pljs.gc(); $$;

-- Runs `code` n times, catching what it throws, and counts the failures.
CREATE FUNCTION cel_run(code text, n int4) RETURNS int4 LANGUAGE pljs AS $$
  const f = new Function(code);
  let failed = 0;

  for (let i = 0; i < n; i++) {
    try {
      f();
    } catch (e) {
      failed++;
    }
  }

  return failed;
$$;

CREATE FUNCTION cel_released(code text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
  before float8;
  failed int4;
BEGIN
  PERFORM cel_run(code, 1);
  PERFORM cel_gc();
  before := cel_mb();
  failed := cel_run(code, 2000);
  PERFORM cel_gc();
  RETURN format('%s failed, released: %s', failed, cel_mb() - before < 5);
END $$;

-- 1) A function's result.
CREATE FUNCTION cel_result() RETURNS int4[] LANGUAGE pljs AS $$
  const a = new Array(1000).fill(1);
  a.push('x'.repeat(10000));
  return a;
$$;
SELECT cel_released($$ pljs.execute('SELECT cel_result()') $$);

-- 2) A row a set-returning function returned.
CREATE TYPE cel_pair AS (a int4, b text);
CREATE FUNCTION cel_rows() RETURNS SETOF cel_pair LANGUAGE pljs AS $$
  return [{a: 1, b: 'x'.repeat(10000)}, {a: 'y'.repeat(10000), b: 'z'}];
$$;
SELECT cel_released($$ pljs.execute('SELECT * FROM cel_rows()') $$);

-- 3) A trigger's NEW.
CREATE TABLE cel_tbl (v varchar(3));
CREATE FUNCTION cel_trig() RETURNS trigger LANGUAGE pljs AS $$
  NEW.v = 'x'.repeat(20000);
  return NEW;
$$;
CREATE TRIGGER cel_trig BEFORE INSERT ON cel_tbl
  FOR EACH ROW EXECUTE FUNCTION cel_trig();
SELECT cel_released($$ pljs.execute("INSERT INTO cel_tbl VALUES ('abc')") $$);

-- 4) An element of an array, and a column of a record, passed to
-- return_next().
CREATE FUNCTION cel_next_array() RETURNS SETOF int4[] LANGUAGE pljs AS $$
  pljs.return_next([1, 'x'.repeat(20000)]);
$$;
SELECT cel_released($$ pljs.execute('SELECT * FROM cel_next_array()') $$);

CREATE FUNCTION cel_next_row() RETURNS SETOF cel_pair LANGUAGE pljs AS $$
  pljs.return_next({a: 'x'.repeat(20000), b: 'y'});
$$;
SELECT cel_released($$ pljs.execute('SELECT * FROM cel_next_row()') $$);

CREATE FUNCTION cel_next_value() RETURNS SETOF int4 LANGUAGE pljs AS $$
  pljs.return_next('x'.repeat(20000));
$$;
SELECT cel_released($$ pljs.execute('SELECT * FROM cel_next_value()') $$);

-- 5) Every level of a jsonb value above a getter that threw.
CREATE FUNCTION cel_jsonb() RETURNS jsonb LANGUAGE pljs AS $$
  return {a: {b: ['x'.repeat(20000), {get c() { throw new Error('c'); }}]}};
$$;
SELECT cel_released($$ pljs.execute('SELECT cel_jsonb()') $$);

-- 6) A query's parameter, and a prepared plan's.
SELECT cel_released($$
  pljs.execute('SELECT $1::int4 AS x', ['x'.repeat(20000)])
$$);
SELECT cel_released($$
  const plan = pljs.prepare('SELECT $1 AS x', ['int4']);

  try {
    plan.execute(['x'.repeat(20000)]);
  } finally {
    plan.free();
  }
$$);
SELECT cel_released($$
  const plan = pljs.prepare('SELECT $1 AS x', ['int4']);

  try {
    plan.cursor(['x'.repeat(20000)]);
  } finally {
    plan.free();
  }
$$);

-- And from PostgreSQL to JavaScript: an array or a row being built when one
-- of its elements or columns cannot be converted -- here a multidimensional
-- array.  Only the outermost row of a query's result was released, so a row
-- inside an array, or the row whose column raised, stayed in the runtime.
CREATE TYPE cel_holder AS (s text, a int4[]);
SELECT cel_released($$
  pljs.execute(`SELECT ARRAY[ROW(repeat('x', 20000), '{{1,2},{3,4}}')::cel_holder] AS a`)
$$);
SELECT cel_released($$
  pljs.execute(`SELECT ROW(repeat('x', 20000), '{{1,2},{3,4}}')::cel_holder AS r`)
$$);

DROP TABLE cel_tbl;
DROP FUNCTION cel_mb(), cel_gc(), cel_run(text, int4), cel_released(text),
  cel_result(), cel_rows(), cel_trig(), cel_next_array(), cel_next_row(),
  cel_next_value(), cel_jsonb();
DROP TYPE cel_pair, cel_holder;
