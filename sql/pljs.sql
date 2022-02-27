-- CREATE FUNCTION
CREATE FUNCTION pljs_test(keys text[], vals text[]) RETURNS text AS
$$
	var o = {};
	for (var i = 0; i < keys.length; i++)
		o[keys[i]] = vals[i];
	return JSON.stringify(o);
$$
LANGUAGE pljs IMMUTABLE STRICT;
SELECT pljs_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);

CREATE FUNCTION unnamed_args(text[], text[]) RETURNS text[] AS
$$
	var array1 = arguments[0];
	var array2 = $2;
	return array1.concat(array2);
$$
LANGUAGE pljs IMMUTABLE STRICT;
SELECT unnamed_args(ARRAY['A', 'B'], ARRAY['C', 'D']);

CREATE FUNCTION concat_strings(VARIADIC args text[]) RETURNS text AS
$$
	var result = "";
	for (var i = 0; i < args.length; i++)
		if (args[i] != null)
			result += args[i];
	return result;
$$
LANGUAGE pljs IMMUTABLE STRICT;
SELECT concat_strings('A', 'B', NULL, 'C');

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
SELECT text_to_int4('abc'); -- error
CREATE FUNCTION int4array_to_textarray(x int4[]) RETURNS text[] AS $$ return x; $$ LANGUAGE pljs;
SELECT int4array_to_textarray(ARRAY[123, 456]::int4[]);
CREATE FUNCTION textarray_to_int4array(x text[]) RETURNS int4[] AS $$ return x; $$ LANGUAGE pljs;
SELECT textarray_to_int4array(ARRAY['123', '456']::text[]);
