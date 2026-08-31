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

-- ENUM type
CREATE TYPE status AS ENUM ('active', 'inactive', 'pending');

CREATE FUNCTION enum_echo(s status) returns status AS $$
  return s;
$$ LANGUAGE pljs;

SELECT enum_echo('active');

-- Custom type
CREATE EXTENSION ltree;

CREATE FUNCTION ltree_echo(l ltree) RETURNS ltree AS $$
  return l;
$$ LANGUAGE pljs;

SELECT ltree_echo('1.2.3'::ltree);

-- Custom and domain types reach the catch-all varlena conversion in
-- pljs_datum_to_jsvalue_fallback().  A literal is built with a 4-byte header,
-- but the same value read back out of a column carries a 1-byte short header,
-- and a large one is stored compressed.  Reading either of those with VARDATA()
-- instead of VARDATA_ANY(), and without detoasting, returned a value shifted
-- three bytes into its own payload -- or the raw compressed bytes.
CREATE TABLE ltree_tbl (l ltree);
INSERT INTO ltree_tbl VALUES ('1.2.3');
SELECT ltree_echo(l) FROM ltree_tbl;

CREATE DOMAIN packed_text AS text;

CREATE FUNCTION packed_echo(v packed_text) RETURNS text AS $$
  return v;
$$ LANGUAGE pljs;

CREATE TABLE packed_tbl (v packed_text);
INSERT INTO packed_tbl VALUES ('ABCDEFGHIJ'), (repeat('Z', 5000));

SELECT packed_echo('ABCDEFGHIJ'::packed_text) AS from_literal;
SELECT packed_echo(v) AS from_column FROM packed_tbl WHERE length(v) = 10;
SELECT length(packed_echo(v)) AS toasted_len FROM packed_tbl WHERE length(v) > 100;
