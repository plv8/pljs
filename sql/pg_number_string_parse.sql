-- A JavaScript string bound to an integer or numeric column is parsed by that
-- type's input function, not coerced through a double.
--
-- QuickJS's numeric coercion (JS_ToInt32/JS_ToInt64/JS_ToFloat64) routes the text
-- through an IEEE-754 double, which holds 53 bits of mantissa. Anything wider is
-- rounded before PostgreSQL ever sees it, and the failure is silent: the value is
-- not rejected, it is quietly a different number. WAL LSNs are uint64 and a row
-- id passes 2^53 long before it reaches INT64_MAX, so this is reachable with
-- ordinary data.
--
-- plv8 parses such a bind with the type's input function, so this also makes the
-- two engines agree.
CREATE EXTENSION IF NOT EXISTS pljs;

-- int8 at the limits, exactly.
CREATE FUNCTION nsp_int8(s text) RETURNS int8 AS $$
  return pljs.execute('SELECT $1::int8 AS v', [s])[0].v;
$$ LANGUAGE pljs;

SELECT nsp_int8('9223372036854775807') = 9223372036854775807::int8 AS int64_max_exact;
SELECT nsp_int8('-9223372036854775808') = (-9223372036854775808)::int8 AS int64_min_exact;

-- Above 2^53 but nowhere near the limit: the case that looks harmless.
SELECT nsp_int8('123456789012345678') = 123456789012345678::int8 AS past_2_53_exact;
SELECT nsp_int8('9007199254740993') = 9007199254740993::int8 AS just_past_2_53_exact;

-- Out of range raises instead of wrapping.
\set VERBOSITY terse
SELECT nsp_int8('9223372036854775808');
\set VERBOSITY default

-- int4 and int2 take the same path.
CREATE FUNCTION nsp_int4(s text) RETURNS int4 AS $$
  return pljs.execute('SELECT $1::int4 AS v', [s])[0].v;
$$ LANGUAGE pljs;

SELECT nsp_int4('2147483647') AS int4_max;
\set VERBOSITY terse
SELECT nsp_int4('2147483648');
SELECT nsp_int4('abc');
\set VERBOSITY default

-- numeric keeps a scale no double can represent.
CREATE FUNCTION nsp_numeric(s text) RETURNS numeric AS $$
  return pljs.execute('SELECT $1::numeric AS v', [s])[0].v;
$$ LANGUAGE pljs;

SELECT nsp_numeric('12345678901234567890.123456789') AS numeric_exact;

-- The non-string paths are unchanged: a BigInt is still marshalled directly, and
-- an ordinary Number still works where it is exact.
CREATE FUNCTION nsp_bigint() RETURNS int8 AS $$
  return pljs.execute('SELECT $1::int8 AS v', [9223372036854775807n])[0].v;
$$ LANGUAGE pljs;

SELECT nsp_bigint() = 9223372036854775807::int8 AS bigint_still_exact;

CREATE FUNCTION nsp_number() RETURNS int4 AS $$
  return pljs.execute('SELECT $1::int4 AS v', [42])[0].v;
$$ LANGUAGE pljs;

SELECT nsp_number() AS number_unchanged;

-- A returned value takes the same conversion, so a string returned for an int8
-- column is exact too.
CREATE FUNCTION nsp_return_string() RETURNS int8 AS $$ return '9223372036854775807'; $$ LANGUAGE pljs;

SELECT nsp_return_string() = 9223372036854775807::int8 AS returned_string_exact;

DROP FUNCTION nsp_int8, nsp_int4, nsp_numeric, nsp_bigint, nsp_number, nsp_return_string;
