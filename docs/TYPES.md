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

Note that `xid`, `"char"`, `money` and enum types reach JavaScript as a
`String`. They previously arrived as a `Number` — the enum's internal OID
rather than its label, and a truncated 32-bit value for `money` — so
arithmetic written against the old behaviour needs updating.

## Domains

A domain is converted as its base type, so `CREATE DOMAIN dint AS int4` arrives
as a `Number` and not as the `String` `"5"`. The domain's `NOT NULL` and
`CHECK` constraints are applied to whatever JavaScript returns, including
through a chain of domains:

```sql
CREATE DOMAIN positive AS int4 CHECK (VALUE > 0);
CREATE FUNCTION negate(v positive) RETURNS positive LANGUAGE pljs AS $$ return -v; $$;
SELECT negate(1);
-- ERROR:  value for domain positive violates check constraint "positive_check"
```
