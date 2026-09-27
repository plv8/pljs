-- A function's arguments are released after it is called.
--
-- JS_Call() takes references of its own to the arguments it is given, and
-- the references the call handler held were never released, so every call
-- left its arguments in the runtime for the life of the backend: 20MB over
-- 20,000 calls with a 1kB text argument, which pljs.gc() did not reclaim,
-- until pljs.memory_limit was exhausted for good.  A trigger's ten arguments,
-- NEW and OLD among them, leaked the same way.

CREATE FUNCTION al_malloc_mb() RETURNS float8 LANGUAGE sql AS $$
  SELECT (pljs_info() ->> 'malloc_size')::float8 / (1024 * 1024);
$$;
CREATE FUNCTION al_gc() RETURNS void LANGUAGE pljs AS $$ pljs.gc(); $$;

CREATE FUNCTION al_len(s text) RETURNS int4 LANGUAGE pljs AS $$
  return s.length;
$$;
CREATE FUNCTION al_rows(s text) RETURNS SETOF int4 LANGUAGE pljs AS $$
  pljs.return_next(s.length);
$$;

-- Warm up the context, so what follows measures only the calls.
SELECT al_len('x'), al_rows('x'), al_gc();

CREATE TEMP TABLE al_before AS SELECT al_malloc_mb() AS mb;
SELECT count(al_len(repeat('x', 1000))) FROM generate_series(1, 20000);
SELECT count(*) FROM generate_series(1, 20000) AS i,
  LATERAL al_rows(repeat('x', 1000));
SELECT al_gc();
SELECT al_malloc_mb() - mb < 5 AS function_arguments_released FROM al_before;

-- A trigger's arguments.
CREATE TABLE al_tbl (id int4, payload text);
CREATE FUNCTION al_trig() RETURNS trigger LANGUAGE pljs AS $$
  return NEW;
$$;
CREATE TRIGGER al_trig BEFORE INSERT ON al_tbl
  FOR EACH ROW EXECUTE FUNCTION al_trig();

INSERT INTO al_tbl VALUES (0, 'x');
SELECT al_gc();
TRUNCATE al_before;
INSERT INTO al_before SELECT al_malloc_mb();
INSERT INTO al_tbl SELECT i, repeat('x', 1000) FROM generate_series(1, 20000) AS i;
SELECT al_gc();
SELECT al_malloc_mb() - mb < 5 AS trigger_arguments_released FROM al_before;

DROP TABLE al_tbl;
DROP FUNCTION al_malloc_mb(), al_gc(), al_len(text), al_rows(text), al_trig();
