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
| default          | `String` or `Number` |
