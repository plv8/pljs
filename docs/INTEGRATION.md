# PLJS/Postgres Integration

PLJS has the ability to execute function calls inside of Postgres.

## Scalar Function Calls

In PLJS, you can write your invoked function call in JavaScript, using the usual `CREATE FUNCTION` statement. Here is an example of a `scalar` function call:

```
CREATE FUNCTION pljs_test(keys TEXT[], vals TEXT[]) RETURNS JSONB AS $$
    var o = {};
    for(var i=0; i<keys.length; i++){
        o[keys[i]] = vals[i];
    }
    return o;
$$ LANGUAGE pljs IMMUTABLE STRICT;

=# SELECT pljs_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);

pljs_test
---------------------------
{"name":"Tom","age":"29"}
(1 row)
```

Internally, the function will defined such as:

```
(function(keys, vals) {
  var o = {};
  for(var i=0; i<keys.length; i++){
      o[keys[i]] = vals[i];
  }
  return o;
})
```

Where `keys` and `vals` undergo type checking and validation within PostgreSQL, serving as arguments to the function. The object `o` becomes the result returned as `JSONB` to Postgres. If argument names are omitted during function creation, they will be available in the function as `$1`, `$2`, etc.

## Set-returning Function Calls

PLJS supports returning `SETOF` from function calls:

```
CREATE TYPE rec AS (i integer, t text);
CREATE FUNCTION set_of_records() RETURNS SETOF rec AS
$$
    // pljs.return_next() stores records in an internal tuplestore,
    // and return all of them at the end of function.
    pljs.return_next( { "i": 1, "t": "a" } );
    pljs.return_next( { "i": 2, "t": "b" } );

    // You can also return records with an array of JSON.
    return [ { "i": 3, "t": "c" }, { "i": 4, "t": "d" } ];
$$
LANGUAGE pljs;
```

Running this gives you a `SETOF` result:

```
=# SELECT * FROM set_of_records();

i | t
---+---
1 | a
2 | b
3 | c
4 | d
(4 rows)
```

Internally, when the function is declared as `RETURNS SETOF`, PLJS prepares a `tuplestore` every time it is called. You can call the `pljs.return_next()` function as many times as you need to return a row. In addition, you can also return an `array` to add a set of records.

If the argument object to `return_next()` has extra properties that are not defined by the argument, `return_next()` raises an error.

## Trigger Function Calls

PLJS supports trigger function calls:

```
CREATE FUNCTION test_trigger() RETURNS TRIGGER AS
$$
    pljs.elog(NOTICE, "NEW = ", JSON.stringify(NEW));
    pljs.elog(NOTICE, "OLD = ", JSON.stringify(OLD));
    pljs.elog(NOTICE, "TG_OP = ", TG_OP);
    pljs.elog(NOTICE, "TG_ARGV = ", TG_ARGV);
    if (TG_OP == "UPDATE") {
        NEW.i = 102;
        return NEW;
    }
$$
LANGUAGE "pljs";

CREATE TRIGGER test_trigger
    BEFORE INSERT OR UPDATE OR DELETE
    ON test_tbl FOR EACH ROW
    EXECUTE PROCEDURE test_trigger('foo', 'bar');
```

If the trigger type is an `INSERT` or `UPDATE`, you can assign properties of `NEW` variable to change the actual tuple stored by this operation. A PLJS trigger function will have the following special arguments that contain the trigger state:

- `NEW`
- `OLD`
- `TG_NAME`
- `TG_WHEN`
- `TG_LEVEL`
- `TG_OP`
- `TG_RELID`
- `TG_TABLE_NAME`
- `TG_TABLE_SCHEMA`
- `TG_ARGV`

For more information see the [trigger section in PostgreSQL manual](https://www.postgresql.org/docs/current/static/plpgsql-trigger.html).

## Inline Statement Calls

PLJS supports the `DO` block:

```
DO $$ pljs.elog(NOTICE, 'this', 'is', 'inline', 'code'); $$ LANGUAGE pljs;
```

## IN/OUT/INOUT Handling

There are some specific function declarations that PLJS handles differently than some other procedural languages.

```
CREATE FUNCTION inout_test(IN t1 TEXT, INOUT i1 INTEGER, OUT o1 TEXT) AS
$$
  return { i1: 23, o1: t1 + i1, foo: 'bar' };
$$
LANGUAGE pljs;
```

When we execute this we call it with only parameters that are inputs to the function, in this case `t1` and `i1`. Note that only named parameters in the function definition get returned.

```
SELECT * FROM inout_test('hello', 5);
 i1 |   o1
----+--------
 23 | hello5
(1 row)
```

When only one variable occurs in the function definition that as an output, then the return type must be a scalar value.

```
CREATE FUNCTION scalar_test(INOUT i1 INTEGER) AS
$$
  return i1 + 5;
$$
LANGUAGE pljs;

SELECT * FROM scalar_test(23);
i1
----
28
(1 row)
```

## Procedures

Procedures work similarly to functions.

```
CREATE PROCEDURE procedure_inout_test(IN t1 TEXT, INOUT i1 INTEGER, OUT o1 TEXT) AS
$$
  return { i1: 23, o1: t1 + i1, foo: 'bar' };
$$
LANGUAGE pljs;
```

The main difference is that `OUT` arguments _must_ be explicitly used as part of the `CALL`.

```
CALL procedure_inout_test('hello', 5, 'foo');
 i1 |   o1
----+--------
 23 | hello5
(1 row)
```

Again, extraneous output is ignored.
