CREATE FUNCTION return_void() RETURNS void AS $$ $$ LANGUAGE pljs;
SELECT return_void();

CREATE FUNCTION return_null() RETURNS text AS $$ return null; $$ LANGUAGE pljs;
SELECT r, r IS NULL AS isnull FROM return_null() AS r;

-- TYPE CONVERTIONS
CREATE FUNCTION int2_to_int4(x int2) RETURNS int4 AS $$ return x; $$ LANGUAGE pljs;
SELECT int2_to_int4(24::int2);
CREATE FUNCTION int4_to_int2(x int4) RETURNS int2 AS $$ return x; $$ LANGUAGE pljs;
SELECT int4_to_int2(42);
CREATE FUNCTION int4_to_int8(x int4) RETURNS int8 AS $$ return x; $$ LANGUAGE pljs;
SELECT int4_to_int8(48);
CREATE FUNCTION int8_to_int4(x int8) RETURNS int4 AS $$ return x; $$ LANGUAGE pljs;
SELECT int8_to_int4(84);
CREATE FUNCTION float8_to_numeric(x float8) RETURNS numeric AS $$ return x; $$ LANGUAGE pljs;
SELECT float8_to_numeric(1.5);
CREATE FUNCTION numeric_to_int8(x numeric) RETURNS int8 AS $$ return x; $$ LANGUAGE pljs;
SELECT numeric_to_int8(1234.56);
CREATE FUNCTION int4_to_text(x int4) RETURNS text AS $$ return x; $$ LANGUAGE pljs;
SELECT int4_to_text(123);
CREATE FUNCTION text_to_int4(x text) RETURNS int4 AS $$ return x; $$ LANGUAGE pljs;
SELECT text_to_int4('123');
SELECT text_to_int4('abc');

-- ARRAYS
CREATE FUNCTION return_array() RETURNS TEXT[] AS $$ return ["foo", "bar"]; $$LANGUAGE pljs;
SELECT return_array();

-- BigInt
-- a BigInt that will work on any value
CREATE OR REPLACE FUNCTION bigint_working(val BIGINT)
   RETURNS BIGINT AS $$
    return val - 1n;
   $$ LANGUAGE pljs STABLE STRICT;
SELECT bigint_working(9223372036854775807);

SELECT bigint_working(32);

-- a BigInt that will fail on any value
CREATE OR REPLACE FUNCTION bigint_failing(val BIGINT)
   RETURNS BIGINT AS $$
    return val - 1;
   $$ LANGUAGE pljs STABLE STRICT;
SELECT bigint_failing(9223372036854775807);

-- BigInt as Numeric
CREATE OR REPLACE FUNCTION bigint_numeric(a INT8, b INT8)
   RETURNS NUMERIC AS $$
    return a ** b;
   $$ LANGUAGE pljs STABLE STRICT;
SELECT bigint_numeric(20, 200);
