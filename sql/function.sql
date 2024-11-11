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

-- return type check
CREATE OR REPLACE FUNCTION bogus_return_type() RETURNS int[] AS
$$
    return 1;
$$ LANGUAGE pljs;
SELECT bogus_return_type();

-- INOUT and OUT parameters
CREATE FUNCTION one_inout(a integer, INOUT b text) AS
$$
return a + b;
$$
LANGUAGE pljs;
SELECT one_inout(5, 'ABC');

CREATE FUNCTION one_out(OUT o text, i integer) AS
$$
return "ABC" + i;
$$
LANGUAGE pljs;
SELECT one_out(123);

CREATE FUNCTION two_out(OUT o text, OUT o2 text, i integer) AS
$$
return { o: "ABC" + i, o2: i + "ABC" };
$$
LANGUAGE pljs;
SELECT two_out(123);

-- polymorphic types
CREATE FUNCTION polymorphic(poly anyarray) returns anyelement AS
$$
    return poly[0];
$$
LANGUAGE pljs;
SELECT polymorphic(ARRAY[10, 11]), polymorphic(ARRAY['foo', 'bar']);

-- RECORD TYPES
CREATE TYPE rec AS (i integer, t text);
CREATE FUNCTION scalar_to_record(i integer, t text) RETURNS rec AS
$$
	return { "i": i, "t": t };
$$
LANGUAGE pljs;
SELECT scalar_to_record(1, 'a');

CREATE FUNCTION record_to_text(x rec) RETURNS text AS
$$
	return JSON.stringify(x);
$$
LANGUAGE pljs;
SELECT record_to_text('(1,a)'::rec);

CREATE FUNCTION return_record(i integer, t text) RETURNS record AS
$$
	return { "i": i, "t": t };
$$
LANGUAGE pljs;
SELECT * FROM return_record(1, 'a');
SELECT * FROM return_record(1, 'a') AS t(j integer, s text);
SELECT * FROM return_record(1, 'a') AS t(x text, y text);
SELECT * FROM return_record(1, 'a') AS t(i integer, t text);
