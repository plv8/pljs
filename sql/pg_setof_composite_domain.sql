-- A set of a domain over a composite type.
--
-- Its rows are rows of the composite type, each of which has to satisfy the
-- domain.  The function's type class, TYPEFUNC_COMPOSITE_DOMAIN, was taken
-- for a single-column set: the first property of each returned row object
-- was converted, and that one Datum was passed as a row of every column of
-- the composite type.  tuplestore_putvalues() read the rest off the stack,
-- and the backend was killed.  The domain's constraints were never checked,
-- by return_next() either.

CREATE TYPE scd_pair AS (a int4, b text);
CREATE DOMAIN scd_dpair AS scd_pair CHECK ((VALUE).a > 0);

-- 1) Rows the function returns.
CREATE FUNCTION scd_returned() RETURNS SETOF scd_dpair LANGUAGE pljs AS $$
  return [{a: 1, b: 'one'}, {a: 2, b: 'two'}];
$$;
SELECT * FROM scd_returned();

-- A row that fails the domain's CHECK.
CREATE FUNCTION scd_returned_bad() RETURNS SETOF scd_dpair LANGUAGE pljs AS $$
  return [{a: 1, b: 'one'}, {a: -2, b: 'minus two'}];
$$;
SELECT * FROM scd_returned_bad();

-- 2) Rows passed to return_next().
CREATE FUNCTION scd_next() RETURNS SETOF scd_dpair LANGUAGE pljs AS $$
  pljs.return_next({a: 3, b: 'three'});
  pljs.return_next(null);
  pljs.return_next({a: 4, b: 'four'});
$$;
SELECT * FROM scd_next();

CREATE FUNCTION scd_next_bad() RETURNS SETOF scd_dpair LANGUAGE pljs AS $$
  pljs.return_next({a: 3, b: 'three'});
  try {
    pljs.return_next({a: -4, b: 'minus four'});
  } catch (e) {
    pljs.elog(NOTICE, 'return_next: ' + e.message);
  }
  pljs.return_next({a: 5, b: 'five'});
$$;
SELECT * FROM scd_next_bad();

-- A missing column is named, as for any other composite set.
CREATE FUNCTION scd_next_missing() RETURNS SETOF scd_dpair LANGUAGE pljs AS $$
  pljs.return_next({a: 6});
$$;
SELECT * FROM scd_next_missing();

-- 3) A NOT NULL domain rejects a null row.
CREATE DOMAIN scd_nn_pair AS scd_pair NOT NULL;
CREATE FUNCTION scd_next_null() RETURNS SETOF scd_nn_pair LANGUAGE pljs AS $$
  pljs.return_next({a: 7, b: 'seven'});
  pljs.return_next(null);
$$;
SELECT * FROM scd_next_null();

-- A null row the function returns, rather than passes to return_next(), is
-- left out, as for any other composite set -- once the domain has allowed it.
-- A NOT NULL domain's set dropped it without a word.
CREATE FUNCTION scd_returned_null() RETURNS SETOF scd_nn_pair LANGUAGE pljs AS $$
  return [{a: 9, b: 'nine'}, null];
$$;
SELECT * FROM scd_returned_null();

CREATE FUNCTION scd_returned_null_ok() RETURNS SETOF scd_dpair LANGUAGE pljs AS $$
  return [{a: 10, b: 'ten'}, null, undefined];
$$;
SELECT * FROM scd_returned_null_ok();

-- 4) A domain over a single-column composite type is still checked.
CREATE TYPE scd_single AS (a int4);
CREATE DOMAIN scd_dsingle AS scd_single CHECK ((VALUE).a > 0);
CREATE FUNCTION scd_single_bad() RETURNS SETOF scd_dsingle LANGUAGE pljs AS $$
  return [{a: 8}, {a: -5}];
$$;
SELECT * FROM scd_single_bad();

-- 5) Two integer columns, where the second column was stack garbage.
CREATE TYPE scd_ints AS (a int4, b int4);
CREATE DOMAIN scd_dints AS scd_ints;
CREATE FUNCTION scd_ints() RETURNS SETOF scd_dints LANGUAGE pljs AS $$
  return [{a: 1, b: 10}, {a: 2, b: 20}];
$$;
SELECT * FROM scd_ints();

DROP FUNCTION scd_returned(), scd_returned_bad(), scd_next(), scd_next_bad(),
  scd_next_missing(), scd_next_null(), scd_returned_null(),
  scd_returned_null_ok(), scd_single_bad(), scd_ints();
DROP DOMAIN scd_dpair, scd_nn_pair, scd_dsingle, scd_dints;
DROP TYPE scd_pair, scd_single, scd_ints;
