-- A set-returning function's result type is the one this call resolves.
--
-- The type class was kept from the function's first call, so a polymorphic
-- SETOF anyelement function called with a scalar and then with a row put the
-- row as a single column: tuplestore_putvalues() read the rest off the stack,
-- (1, 2) came back as (1, <garbage>), and a text column crashed the backend.
-- Called first with a domain over a composite type, every later scalar was
-- read as a tuple header.

CREATE TYPE spr_pair AS (a int4, b int4);
CREATE TYPE spr_named AS (a int4, b text);

-- 1) A scalar first, then rows, through return_next() and a returned array.
CREATE FUNCTION spr_next(x anyelement) RETURNS SETOF anyelement
LANGUAGE pljs AS $$
  pljs.return_next(x);
$$;

SELECT * FROM spr_next(1);
SELECT * FROM spr_next(ROW(1, 2)::spr_pair);
SELECT * FROM spr_next(ROW(3, 'three')::spr_named);
SELECT * FROM spr_next('abc'::text);

CREATE FUNCTION spr_returned(x anyelement) RETURNS SETOF anyelement
LANGUAGE pljs AS $$
  return [x, x];
$$;

SELECT * FROM spr_returned(1);
SELECT * FROM spr_returned(ROW(4, 'four')::spr_named);

-- 2) A domain over a composite type first, then scalars.
CREATE DOMAIN spr_positive_pair AS spr_pair CHECK ((VALUE).a > 0);

CREATE FUNCTION spr_domain_first(x anyelement) RETURNS SETOF anyelement
LANGUAGE pljs AS $$
  pljs.return_next(x);
$$;

SELECT * FROM spr_domain_first(ROW(1, 2)::spr_positive_pair);
SELECT * FROM spr_domain_first(5);
SELECT * FROM spr_domain_first('x'::text);

-- 3) record: a column definition list is needed on every call, not only on
-- the first.
CREATE FUNCTION spr_record() RETURNS SETOF record LANGUAGE pljs AS $$
  pljs.return_next({a: 1});
$$;

SELECT * FROM spr_record() AS t(a int4);
SELECT spr_record();
SELECT * FROM spr_record() AS t(a int4);

SELECT 1 AS still_connected;

DROP FUNCTION spr_record();
DROP FUNCTION spr_domain_first(anyelement);
DROP FUNCTION spr_returned(anyelement);
DROP FUNCTION spr_next(anyelement);
DROP DOMAIN spr_positive_pair;
DROP TYPE spr_named;
DROP TYPE spr_pair;
