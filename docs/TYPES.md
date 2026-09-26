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
| `BYTEA`          | `Array`              |
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

Arithmetic or comparisons written against the old values need updating.

In the other direction, a JavaScript `Number` returned for one of these types
is converted to text and parsed by the type, so it means what the same text
would mean in SQL. Two types need care:

- `"char"`: `65` is parsed as the text `'65'`, which is `'6'`. Return the
  character itself, `'A'` or `String.fromCharCode(65)`.
- `money`: `1234.5` is parsed according to `lc_monetary`, where `.` may be the
  thousands separator rather than the decimal point. Return a string in the
  locale's format, or return `numeric` and cast in SQL.

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
