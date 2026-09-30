# PLJS Types

In PLJS, types are converted between Postgres native types and JavaScript types. This is not always a 1:1 mapping, but where possible there are direct conversions:

| PostgreSQL Type  | JavaScript Type      |
| ---------------- | -------------------- |
| `TEXT`           | `String`             |
| `VARCHAR`        | `String`             |
| `FLOAT4`         | `Number`             |
| `FLOAT8`         | `Number`             |
| `NUMERIC`        | `Number` or `BigInt` |
| `OID`            | `Number`             |
| `BOOL`           | `Bool`               |
| `INT2`           | `Number`             |
| `INT4`           | `Number` or `BigInt` |
| `INT8`           | `Number` or `BigInt` |
| `FLOAT4`         | `Number`             |
| `FLOAT8`         | `Number`             |
| `JSON`           | `JSON`               |
| `JSONB`          | `JSON`               |
| `TEXT`           | `String`             |
| `VARCHAR`        | `String`             |
| `BPCHAR`         | `String`             |
| `NAME`           | `String`             |
| `XML`            | `String`             |
| `BYTEA`          | `Uint8Array`         |
| `DATE`           | `Date`               |
| `TIMESTAMP`      | `Date`               |
| `TIMESTAMPTZOID` | `Date`               |
| default          | `String`             |

## Every other type

A type without a row above is converted through its own text input and output
functions, so it reaches JavaScript as the `String` that `psql` would print and
is parsed back by the type itself. That covers `uuid`, `inet`, `cidr`,
`macaddr`, `interval`, `time`, `timetz`, the geometric types, `bit` and `bit
varying`, `money`, `tsvector`, every enum, and extension types such as
`ltree`:

```sql
CREATE FUNCTION shout(m mood) RETURNS text LANGUAGE pljs AS $$
  return m.toUpperCase();
$$;
```

A `void` value, such as the column of `SELECT pg_sleep(0) AS v`, has nothing
to convert and arrives as `undefined`.

Because the type parses the value on the way back, returning something it does
not accept raises rather than storing a corrupted value:

```sql
CREATE FUNCTION bad() RETURNS uuid LANGUAGE pljs AS $$ return 'not-a-uuid'; $$;
SELECT bad();
-- ERROR:  invalid input syntax for type uuid: "not-a-uuid"
```

### Upgrading

Every pass-by-value type without a row above used to reach JavaScript as a
`Number` — usually a wrong one — and now arrives as a `String`:

- enum types, which arrived as the enum's internal OID rather than its label;
- `money`, `time`, `xid8` and `pg_lsn`, which were truncated to 32 bits;
- `"char"`, `xid` and `cid`;
- the OID alias types `regclass`, `regtype`, `regproc`, `regprocedure`,
  `regoper`, `regoperator`, `regnamespace`, `regrole`, `regconfig`,
  `regdictionary` and `regcollation`, which now arrive as the name
  (`'pg_class'`) rather than the OID. Cast to `oid` in SQL to keep the number.
  `regproc` and `regoper` arrive as a signature, as `regprocedure` and
  `regoperator` do — `'abs(integer)'`, `'+(integer,integer)'` — because
  the bare name of an overloaded function or operator cannot be read back.
  A bare name, or an OID written as text, is still accepted in return.

Arithmetic or comparisons written against the old values need updating.

In the other direction, a JavaScript `Number` returned for one of these types
is converted to text and parsed by the type, so it means what the same text
would mean in SQL. Two types need care:

- `"char"`: `65` is parsed as the text `'65'`, which is `'6'`. Return the
  character itself, `'A'` or `String.fromCharCode(65)`.
- `money`: `1234.5` is parsed according to `lc_monetary`, where `.` may be the
  thousands separator rather than the decimal point. Return a string in the
  locale's format, or return `numeric` and cast in SQL.

## `bytea`

A `bytea` arrives as a `Uint8Array` of its bytes. A `bytea` can be returned as
a typed array (`Uint8Array`, `Int8Array`, `Uint16Array`, `Int16Array`,
`Uint32Array` or `Int32Array`), whose bytes are stored in the machine's byte
order; as an `ArrayBuffer`; or as a `String`, which is stored as its UTF-8
bytes.

### Upgrading

A `bytea` used to arrive as a `String`, made by reading its bytes as UTF-8.
Every byte that was not part of a valid UTF-8 sequence was replaced, so a
value that went back to PostgreSQL — `NEW` from a trigger, even one that
changed some other column — was rewritten: `'\xdeadbeef'` was stored as
`'\xdeadefbfbd'`. Code that treated the value as a string needs updating: for
text stored as `bytea`, `String.fromCharCode(...value)` reads ASCII, and
`convert_from(value, 'UTF8')` in SQL reads UTF-8.

## `json` and `jsonb`

A value is written to `json` and `jsonb` as `JSON.stringify()` writes it:

- a property whose value is a function, `undefined` or a `Symbol` is left out,
  and such an element of an array is `null`;
- a value that has no JSON at all — a function or a `Symbol` — is SQL
  `NULL`, as `undefined` is;
- `NaN` and the infinities are `null`;
- a `Date` is written through its `toJSON()`, as an ISO 8601 string, or
  `null` for an invalid `Date`;
- a `BigInt`, or a `BigInt` object, raises, as `JSON.stringify()` throws for
  one, unless `BigInt.prototype.toJSON` is defined;
- only an object's own enumerable properties are written.

Every `int8` reaches JavaScript as a `BigInt`, so a row read with
`pljs.execute()` cannot be returned as `jsonb` as it is if it has an `int8`
column. Convert the value, with `Number()` or `String()`, or cast the column in
the query. `jsonb` used to store a `BigInt` as a string, `"10"`, and a `BigInt`
object as `{}`, where `json` raised. For `jsonb` the error is "cannot convert a
BigInt to jsonb", with SQLSTATE `22023` (`invalid_parameter_value`). `json` is
written by `JSON.stringify()` itself, and raises what it throws, "Do not know
how to serialize a BigInt", with SQLSTATE `XX000` as any other JavaScript
error.

An object that contains itself raises "cannot convert a circular structure to
jsonb", and a string or a key holding `"\u0000"` cannot be stored in `jsonb`,
which raises as `jsonb_in()` does.

An `xml` value is parsed by `xml`'s input function, so something that is not
XML raises.

## Domains

A domain whose base type has a row above is converted as its base type, so
`CREATE DOMAIN dint AS int4` arrives as a `Number` and not as the `String`
`"5"`, a domain over `jsonb` takes an object, and a domain over `timestamp`
takes a `Date`. Any other domain — over `uuid`, an enum, or an extension type —
is converted as plv8 converts every domain: as a `String`, parsed back by the
domain's own input function.

Either way the domain's `NOT NULL` and `CHECK` constraints are applied to
whatever JavaScript returns, including through a chain of domains, and
including a `null` or `undefined` returned as the value, as an array element,
or as a field of a composite or a trigger's `NEW`:

```sql
CREATE DOMAIN positive AS int4 CHECK (VALUE > 0);
CREATE FUNCTION negate(v positive) RETURNS positive LANGUAGE pljs AS $$ return -v; $$;
SELECT negate(1);
-- ERROR:  value for domain positive violates check constraint "positive_check"
```

The exception is a generated column in a `BEFORE` trigger's `NEW`, which is
`NULL` until the executor computes it after the trigger, and is not checked.

### Upgrading

A domain used to be converted from its internal representation, whatever its
base type. For most base types that was wrong — a domain over `timestamp`
arrived as a truncated integer, one over `jsonb` as its binary storage — but
two worked, and code written against them needs updating:

- A domain over `json` arrived as the JSON text, and JSON text returned for one
  was stored as that JSON. It is now converted as `json` is: an argument
  arrives as the parsed value, so `JSON.parse()` on it throws, and a returned
  string is stored as a JSON string, so `return JSON.stringify({a: 1})` stores
  `"{\"a\":1}"` rather than `{"a": 1}`. Use the value as it arrives, and return
  the object itself.
- A domain over `boolean` arrived as the `Number` `0` or `1`, and now arrives
  as `false` or `true`.

## Length and precision

A type modifier — the `(2)` in `char(2)`, the `(5,2)` in `numeric(5,2)` — is
applied to every value JavaScript returns into a domain or a column that has
one: a composite's field, a row passed to `return_next()`, or a column of a
trigger's `NEW`. The value is rounded, padded or rejected as an `INSERT` would
do it:

```sql
CREATE TABLE t (code char(2), amount numeric(5,2));
-- in a BEFORE INSERT trigger:
--   NEW.amount = 3.14159;   stored as 3.14
--   NEW.code = 'Texas';     ERROR:  value too long for type character(2)
```
