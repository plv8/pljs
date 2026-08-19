-- Regression: a composite column that converts to NULL crashed the backend.
--
-- pljs_jsvalue_to_datum() signalled a SQL NULL with PG_RETURN_NULL(), which
-- expands to `fcinfo->isnull = true; return (Datum) 0`.  Every column of a
-- composite is converted through pljs_jsvalue_to_datums() or
-- pljs_jsvalue_to_record(), and both pass fcinfo == NULL -- they report the null
-- through the is_null argument instead.  Each of those sites therefore wrote
-- through a null pointer: a SIGSEGV on address 0x1c, which is the offset of
-- FunctionCallInfoBaseData.isnull.
--
-- Reaching it takes nothing exotic.  `case DATEOID` handles only a JavaScript
-- Date and breaks out of the switch for anything else, so a plain string for a
-- date column falls through to the trailing null return.  The scalar form of the
-- same conversion has a real fcinfo and quietly returns NULL, which is why this
-- stayed hidden: `RETURNS date` is fine, and only the composite form dies.
CREATE TYPE cnd_row AS (d date, ts timestamp, n int);

-- Through return_next() on a set.
CREATE FUNCTION cnd_set() RETURNS SETOF cnd_row AS $$
  pljs.return_next({ d: 'not-a-date', ts: 'not-a-timestamp', n: 1 });
$$ LANGUAGE pljs;

SELECT * FROM cnd_set();

-- Through a plain composite return, which takes pljs_jsvalue_to_record().
CREATE FUNCTION cnd_record() RETURNS cnd_row AS $$
  return { d: 'not-a-date', ts: 42, n: 2 };
$$ LANGUAGE pljs;

SELECT * FROM cnd_record();

-- An array column reaches the same path one level deeper: the element conversion
-- is handed the caller's fcinfo, which is NULL here too.
CREATE TYPE cnd_arr AS (a date[]);

CREATE FUNCTION cnd_array() RETURNS cnd_arr AS $$
  return { a: ['not-a-date'] };
$$ LANGUAGE pljs;

SELECT * FROM cnd_array();

-- A valid date string takes the same path, because the check is Is_Date() and not
-- a parse: it becomes NULL rather than 2020-01-01.  Recorded here as the current
-- behaviour, not endorsed -- routing these through the type's input function is a
-- change of semantics and belongs with the rest of the type I/O work.
CREATE FUNCTION cnd_valid_string() RETURNS cnd_row AS $$
  return { d: '2020-01-01', ts: '2020-01-01 12:00:00', n: 3 };
$$ LANGUAGE pljs;

SELECT * FROM cnd_valid_string();

-- A real Date still converts, so the fix did not disturb the working path.
CREATE FUNCTION cnd_real_date() RETURNS cnd_row AS $$
  return { d: new Date(Date.UTC(2020, 0, 1)), ts: new Date(Date.UTC(2020, 0, 1, 12)), n: 4 };
$$ LANGUAGE pljs;

-- Compared rather than printed: the rendering of a date depends on DateStyle.
SELECT (r).d = DATE '2020-01-01'                        AS date_converts,
       (r).ts = TIMESTAMP '2020-01-01 12:00:00'         AS timestamp_converts,
       (r).n                                            AS n
  FROM cnd_real_date() r;

-- The scalar path, which always had a real fcinfo, is unchanged.
CREATE FUNCTION cnd_scalar() RETURNS date AS $$ return 'not-a-date'; $$ LANGUAGE pljs;

SELECT cnd_scalar() IS NULL AS scalar_still_null;

SELECT 1 AS alive;

DROP FUNCTION cnd_set, cnd_record, cnd_array, cnd_valid_string, cnd_real_date, cnd_scalar;
DROP TYPE cnd_row, cnd_arr;
