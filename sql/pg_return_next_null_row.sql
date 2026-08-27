-- return_next(null) emits a NULL row for a composite-returning set.
--
-- A composite set raised "argument must be an object" for null and undefined, so
-- there was no way to emit a NULL row at all.  plpgsql expresses exactly that
-- with RETURN NEXT NULL, and a caller mapping a nullable source row had to skip
-- the row or invent a sentinel value instead.
--
-- null and undefined now produce an all-NULL row, consistent with the rest of
-- the conversion surface, where both mean SQL NULL everywhere.  A non-null value
-- of the wrong shape still raises.
CREATE EXTENSION IF NOT EXISTS pljs;
CREATE TYPE rnn_ct AS (a int, b text);

-- 1) SETOF composite: null and undefined are NULL rows, interleaved with real
-- rows in the right order.
CREATE FUNCTION rnn_setof() RETURNS SETOF rnn_ct LANGUAGE pljs AS $$
  pljs.return_next(null);
  pljs.return_next({a: 1, b: 'x'});
  pljs.return_next(undefined);
$$;
SELECT coalesce(a::text, 'NULL') AS a, coalesce(b, 'NULL') AS b FROM rnn_setof();

-- the emitted rows really are NULL rows, not rows of NULL columns by accident.
SELECT count(*) AS null_rows FROM rnn_setof() t WHERE t IS NULL;
SELECT count(*) AS total_rows FROM rnn_setof();

-- 2) RETURNS TABLE behaves the same.
CREATE FUNCTION rnn_table() RETURNS TABLE(a int, b text) LANGUAGE pljs AS $$
  pljs.return_next(null);
  pljs.return_next({a: 2, b: 'y'});
$$;
SELECT coalesce(a::text, 'NULL') AS a, coalesce(b, 'NULL') AS b FROM rnn_table();

-- 3) a wider row type is NULL in every column.
CREATE FUNCTION rnn_wide() RETURNS TABLE(a int, b text, c bigint, d jsonb) LANGUAGE pljs AS $$
  pljs.return_next(null);
$$;
SELECT a IS NULL AND b IS NULL AND c IS NULL AND d IS NULL AS all_null
  FROM rnn_wide();

-- 4) a non-null value of the wrong shape must still raise.
CREATE FUNCTION rnn_bad_num() RETURNS SETOF rnn_ct LANGUAGE pljs AS $$
  pljs.return_next(5);
$$;
CREATE FUNCTION rnn_bad_str() RETURNS SETOF rnn_ct LANGUAGE pljs AS $$
  pljs.return_next('nope');
$$;
SELECT * FROM rnn_bad_num();
SELECT * FROM rnn_bad_str();

-- 5) a set of only NULL rows is still a set of rows, not an empty result.
CREATE FUNCTION rnn_only_nulls() RETURNS SETOF rnn_ct LANGUAGE pljs AS $$
  for (var i = 0; i < 3; i++) { pljs.return_next(null); }
$$;
SELECT count(*) AS rows FROM rnn_only_nulls();

DROP FUNCTION rnn_setof, rnn_table, rnn_wide, rnn_bad_num, rnn_bad_str,
  rnn_only_nulls;
DROP TYPE rnn_ct;
