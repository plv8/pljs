-- A NULL result has to reach fcinfo->isnull, whatever the type.
--
-- call_function() reports its result's nullness through fcinfo->isnull.  Most
-- conversions set it, but two reported NULL only through their is_null
-- out-parameter, which call_function() ignored:
--
--   - a composite, whose conversion is shared with composite columns that
--     have no fcinfo of their own, and
--   - the fallback's `{is_null: true}`, for a type without a case of its own.
--
-- Both returned a zero pointer as a non-null value.  A composite's was
-- dereferenced as a tuple header and crashed the backend; an enum's was
-- read as OID 0; a uuid's crashed in uuid_out().  Every conversion now
-- reports NULL through both.
CREATE EXTENSION IF NOT EXISTS pljs;

CREATE TYPE pnr_row AS (a int4, b text);

CREATE FUNCTION pnr_row_null() RETURNS pnr_row LANGUAGE pljs AS $$ return null; $$;
SELECT pnr_row_null() IS NULL AS is_null;
SELECT pnr_row_null();

CREATE FUNCTION pnr_row_undefined() RETURNS pnr_row LANGUAGE pljs AS $$ return; $$;
SELECT pnr_row_undefined() IS NULL AS is_null;

-- A domain over a composite took the same path.
CREATE DOMAIN pnr_row_d AS pnr_row;
CREATE FUNCTION pnr_row_d_null() RETURNS pnr_row_d LANGUAGE pljs AS $$ return null; $$;
SELECT pnr_row_d_null() IS NULL AS is_null;

-- The fallback's {is_null: true}.
CREATE FUNCTION pnr_uuid_sentinel() RETURNS uuid LANGUAGE pljs AS $$
  return {is_null: true};
$$;
SELECT pnr_uuid_sentinel() IS NULL AS is_null, pnr_uuid_sentinel();

CREATE TYPE pnr_mood AS ENUM ('sad', 'happy');
CREATE FUNCTION pnr_enum_sentinel() RETURNS pnr_mood LANGUAGE pljs AS $$
  return {is_null: true};
$$;
SELECT pnr_enum_sentinel() IS NULL AS is_null, pnr_enum_sentinel();

-- And from inside pljs, through SPI.
DO $$
  const r = pljs.execute(
    "SELECT pnr_row_null() AS r, pnr_uuid_sentinel() AS u, pnr_enum_sentinel() AS e")[0];
  pljs.elog(NOTICE, 'nulls: ' + (r.r === null) + ' ' + (r.u === null) + ' ' + (r.e === null));
$$ LANGUAGE pljs;

-- A void function returns void, not NULL, whatever it returned.  void is a
-- pseudotype, which was converted as a composite: `return;` came back as
-- NULL, and any other value raised "type void is not composite".
CREATE FUNCTION pnr_void() RETURNS void LANGUAGE pljs AS $$ return; $$;
CREATE FUNCTION pnr_void_value() RETURNS void LANGUAGE pljs AS $$ return 5; $$;
SELECT pnr_void() IS NULL AS is_null, pnr_void_value() IS NULL AS value_is_null;

-- A function returning record, or with OUT parameters, gave the record
-- conversion nowhere to report a NULL, and crashed the backend.
CREATE FUNCTION pnr_record_null() RETURNS record LANGUAGE pljs AS $$
  return null;
$$;
SELECT * FROM pnr_record_null() AS t(a int4, b text);
CREATE FUNCTION pnr_out_null(OUT a int4, OUT b text) LANGUAGE pljs AS $$
  return null;
$$;
SELECT * FROM pnr_out_null();
SELECT pnr_out_null() IS NULL AS is_null;

-- An array type raised "value is not an Array" for a NULL, returned or bound.
CREATE FUNCTION pnr_array_null() RETURNS int4[] LANGUAGE pljs AS $$
  return null;
$$;
SELECT pnr_array_null() IS NULL AS is_null;
DO $$
  const r = pljs.execute("SELECT $1::int4[] IS NULL AS n", [null])[0];
  pljs.elog(NOTICE, 'bound null array is null: ' + r.n);
$$ LANGUAGE pljs;

DROP FUNCTION pnr_row_null(), pnr_row_undefined(), pnr_row_d_null(),
              pnr_uuid_sentinel(), pnr_enum_sentinel(), pnr_void(),
              pnr_void_value(), pnr_record_null(), pnr_out_null(),
              pnr_array_null();
DROP DOMAIN pnr_row_d;
DROP TYPE pnr_row, pnr_mood;
