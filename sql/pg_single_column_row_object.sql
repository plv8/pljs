-- A single-column set must accept the same `{column: value}` row object that a
-- multi-column set requires.
--
-- A set with one column is not composite, so return_next() converted its argument
-- directly as the column value. Handed a row object, the whole object went through
-- the scalar conversion: "[object Object]" for a text column, 0 for an integer
-- one. No error, and the row count was right, so a test that counted rows saw
-- nothing wrong.
--
-- The shape that breaks is the natural one -- build a row object in a loop and
-- call return_next(row) -- and it is the only shape that works for two or more
-- columns. It stopped working as soon as the set happened to have one column.
--
-- The bare value keeps working, and an object is only read as a row object when it
-- is a plain object and the column type is not itself object-shaped: json, jsonb
-- and composite columns take an object as their legitimate value.
CREATE EXTENSION IF NOT EXISTS pljs;

-- The case that silently stored 0.
CREATE FUNCTION sc_int() RETURNS TABLE(a int) AS $$
  for (let i = 1; i <= 3; i++) pljs.return_next({ a: i * 10 });
$$ LANGUAGE pljs;

SELECT a FROM sc_int();

-- And the one that stored "[object Object]".
CREATE FUNCTION sc_text() RETURNS TABLE(t text) AS $$
  pljs.return_next({ t: 'hello' });
$$ LANGUAGE pljs;

SELECT t FROM sc_text();

-- Resolution is by arity, not by name, and that is not a shortcut: a
-- single-column RETURNS TABLE collapses to a scalar return type, so the
-- descriptor carries no column name to match against. A one-property object is
-- unambiguous regardless, which is what makes it safe to accept.
--
-- An object with several properties therefore cannot be resolved, and saying so
-- is the point -- that is the case that used to store 0.
\set VERBOSITY terse
CREATE FUNCTION sc_extra() RETURNS TABLE(a int) AS $$
  pljs.return_next({ a: 7, ignored: 'x' });
$$ LANGUAGE pljs;

SELECT a FROM sc_extra();
\set VERBOSITY default

-- A single-attribute *composite* set is different: it goes through the
-- multi-column path, which does have names, so it resolves by name and ignores
-- the extra property.
CREATE TYPE sc_one AS (a int);

CREATE FUNCTION sc_composite() RETURNS SETOF sc_one AS $$
  pljs.return_next({ a: 7, ignored: 'x' });
$$ LANGUAGE pljs;

SELECT a FROM sc_composite();

-- The bare value is still accepted.
CREATE FUNCTION sc_bare() RETURNS TABLE(a int) AS $$
  pljs.return_next(5);
$$ LANGUAGE pljs;

SELECT a FROM sc_bare();

-- SETOF a scalar type has no column name in the descriptor; a one-property object
-- is unambiguous, so it is accepted on arity.
CREATE FUNCTION sc_setof() RETURNS SETOF int AS $$
  pljs.return_next({ anything: 9 });
$$ LANGUAGE pljs;

SELECT * FROM sc_setof();

-- An object that identifies no value is a mistake, and says so.

-- Object-shaped column types still take an object as the value, not as a row.
CREATE FUNCTION sc_jsonb() RETURNS TABLE(j jsonb) AS $$
  pljs.return_next({ k: 'v' });
$$ LANGUAGE pljs;

SELECT j FROM sc_jsonb();

-- A Date is a value for a timestamp column, not a row object.
CREATE FUNCTION sc_date() RETURNS TABLE(ts timestamptz) AS $$
  pljs.return_next(new Date(Date.UTC(2020, 0, 1)));
$$ LANGUAGE pljs;

SELECT ts = TIMESTAMPTZ '2020-01-01 00:00:00+00' AS date_is_a_value FROM sc_date();

-- Two columns behave as before.
CREATE FUNCTION sc_two() RETURNS TABLE(a int, b text) AS $$
  pljs.return_next({ a: 1, b: 'one' });
$$ LANGUAGE pljs;

SELECT a, b FROM sc_two();

DROP FUNCTION sc_int, sc_text, sc_extra, sc_composite, sc_bare, sc_setof, sc_jsonb,
              sc_date, sc_two;
DROP TYPE sc_one;
