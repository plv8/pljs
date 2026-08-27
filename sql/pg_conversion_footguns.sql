-- Regression: JS -> SQL conversions must never silently corrupt or drop data.
--
--   * An embedded NUL in a JavaScript string truncated the text value at the
--     first \u0000 (CStringGetTextDatum -> strlen), so "a\u0000b" was stored as
--     "a" with no warning.  A value that passed an application's validation
--     becoming a shorter, different value in the table is a silent corruption
--     and now raises.
--   * A valid date/timestamp *string* bound to a date/timestamp parameter became
--     SQL NULL, because only JavaScript Date objects were handled.  It is now
--     parsed through the type's input function, and invalid input raises instead
--     of vanishing.
--   * A value with no byte representation (a number, a plain object, an
--     unhandled typed array) bound to bytea became SQL NULL.  It now raises.
--
-- The NUL is built with String.fromCharCode(0) rather than written literally, so
-- this file stays free of embedded NUL bytes.
CREATE EXTENSION IF NOT EXISTS pljs;

-- 1) Embedded NUL in text: a clear error, not silent truncation, on return ...
CREATE FUNCTION ret_nul() RETURNS text LANGUAGE pljs AS $$
  return "a" + String.fromCharCode(0) + "b";
$$;

SELECT ret_nul();

-- ... and on a text bind (raised in SQL, surfacing to JS as a catchable error).
CREATE FUNCTION bind_nul() RETURNS text LANGUAGE pljs AS $$
  const nul = "a" + String.fromCharCode(0) + "b";
  try { return pljs.execute("SELECT $1::text AS t", [nul])[0].t; }
  catch (e) { return "err:" + e.message; }
$$;

SELECT bind_nul();

-- A string with no NUL is unaffected, including multi-byte characters whose
-- encoded form must not be measured with strlen() either.
CREATE FUNCTION ret_plain() RETURNS text LANGUAGE pljs AS $$ return "aéb"; $$;

SELECT ret_plain(), length(ret_plain()) AS chars, octet_length(ret_plain()) AS bytes;

-- 2) date/timestamp *strings* are parsed, not dropped to NULL.  Compared inside
-- SQL so the result does not depend on DateStyle or TimeZone.
CREATE FUNCTION bind_ts_ok() RETURNS boolean LANGUAGE pljs AS $$
  return pljs.execute(
    "SELECT ($1::timestamp = '2020-01-02 03:04:05'::timestamp) AS ok",
    ["2020-01-02 03:04:05"])[0].ok;
$$;

SELECT bind_ts_ok();

CREATE FUNCTION bind_date_ok() RETURNS boolean LANGUAGE pljs AS $$
  return pljs.execute("SELECT ($1::date = '2020-01-02'::date) AS ok",
                      ["2020-01-02"])[0].ok;
$$;

SELECT bind_date_ok();

-- invalid date/timestamp input raises a clear error (caught in JS).
CREATE FUNCTION bind_ts_bad() RETURNS text LANGUAGE pljs AS $$
  try { pljs.execute("SELECT $1::timestamp AS t", ["not-a-date"]); return "no error"; }
  catch (e) { return "err:" + e.message; }
$$;

SELECT bind_ts_bad();

-- A real Date still binds, so the working path is undisturbed.
CREATE FUNCTION bind_date_obj() RETURNS boolean LANGUAGE pljs AS $$
  return pljs.execute("SELECT ($1::date = '2020-01-02'::date) AS ok",
                      [new Date(Date.UTC(2020, 0, 2))])[0].ok;
$$;

SELECT bind_date_obj();

-- 3) a value with no byte representation bound to bytea is a clear error ...
CREATE FUNCTION bind_bytea_bad() RETURNS text LANGUAGE pljs AS $$
  try { pljs.execute("SELECT $1::bytea AS b", [12345]); return "no error"; }
  catch (e) { return "err:" + e.message; }
$$;

SELECT bind_bytea_bad();

-- ... but a string still binds to bytea just fine.
CREATE FUNCTION bind_bytea_ok() RETURNS bytea LANGUAGE pljs AS $$
  return pljs.execute("SELECT $1::bytea AS b", ["hello"])[0].b;
$$;

SELECT bind_bytea_ok();

DROP FUNCTION ret_nul();
DROP FUNCTION bind_nul();
DROP FUNCTION ret_plain();
DROP FUNCTION bind_ts_ok();
DROP FUNCTION bind_date_ok();
DROP FUNCTION bind_ts_bad();
DROP FUNCTION bind_date_obj();
DROP FUNCTION bind_bytea_bad();
DROP FUNCTION bind_bytea_ok();
