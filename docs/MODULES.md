# Modules and packages

PL/JS loads JavaScript from two tables, `pljs.modules` and `pljs.packages`, so
a function can share code with other functions without copying it. See
[CONFIGURATION.md](CONFIGURATION.md) for who may write to them.

```sql
INSERT INTO pljs.modules (path, source) VALUES
  ('greeter', 'export function hello(name) { return "hello " + name; }');

CREATE FUNCTION greet(name text) RETURNS text AS $$
  return pljs.import('greeter').hello(name);
$$ LANGUAGE pljs;
```

## Loading code

| | |
|---|---|
| `pljs.import(name)` | Returns the module's namespace (`{ hello }` above). |
| `pljs.require(name)` | Returns a CommonJS module's `module.exports`; for an ES module, the same namespace as `pljs.import()`. |
| `import ... from 'name'` | Inside a stored module, or at the start of a function. |

A module is CommonJS if its source can be the body of a function that is given
`exports`, `module` and `require`; if it uses `import` or `export` it is an ES
module. The two kinds import each other: importing a CommonJS module gives its
`module.exports` as the default export.

### Strict and sloppy mode

An ES module is strict, and so is a CommonJS module, which pljs wraps in one. Code
that relies on sloppy mode -- assigning to a variable that was never declared, `with`,
octal literals like `010` -- fails to load, with a `ReferenceError` or a `SyntaxError`.

A CommonJS module can ask for sloppy mode, and then runs the way it would in Node:

```sql
INSERT INTO pljs.modules (path, source, sloppy) VALUES
  ('legacy', 'total = 41; with ({ n: 1 }) { total += n; } module.exports = total;', true);
```

or `pljs install --sloppy` from the command line. The flag can be changed with an
`UPDATE`, and takes effect like any other change to a module. It has no effect on an
ES module, which is always strict, or on a function's own code.

### `import` in a function

A function or `DO` block can start with `import` statements:

```sql
CREATE FUNCTION circumference(r float8) RETURNS float8 AS $$
  import { PI } from "constants";
  return 2 * PI * r;
$$ LANGUAGE pljs;
```

Only the imports at the very start count, after any white space and comments
and before any other code. An `import` anywhere else is a syntax error, as it is
in any module. `import(...)` and `import.meta` are not static imports.

- The imports are hoisted into a module that the function is declared in, so
  **a function that imports runs in strict mode**. It is still a global of its
  user's context, as any function is, and other functions can call it by name.
- Errors report the function's own lines, as they do without imports.
- Only the syntax is checked when the function is created, so a function can be
  created before the modules it imports are.
- A relative path (`./x`) has no module to be relative to in a function; use
  the module's whole name.
- The modules are bound when the function is compiled. When `pljs.modules` or
  `pljs.packages` change, the function is compiled again on its next call, in
  every session, so it always runs the current modules. Each compile of such a
  function leaves a little behind until the session ends or `pljs_reset()`
  (see below), so a function does not need to be rewritten to be kept current,
  but a session that changes modules constantly will grow.

## What a name means

1. A **relative** name (`./x`, `../x`) is looked up beside the module that
   wrote it, in the same package if that module is in one. Going above the
   root is an error. A function has no module to be relative to.
2. Otherwise, a **standalone module** with exactly that path, if there is one.
3. Otherwise a **package**: `Name` loads the package's entry point, and
   `Name/sub/path` loads one of its modules. A scoped name (`@scope/pkg`) is
   one package name.

For a package, the version is:

- the importing package's own version, when it imports its own name;
- otherwise the highest version allowed by the importing package's
  `dependencies` entry for it (`^`, `~`, `>=`, `<=`, `>`, `<` or an exact
  version; below 1.0.0 a `^` range stays within the left-most non-zero part,
  so `^0.2.3` is `>=0.2.3 <0.3.0`);
- otherwise, for a name the importer does not list, or code that is not in a
  package, the highest version there is.

Versions that are not plain `major.minor.patch` (`2.0.0-beta`) are never
selected.

## Each module runs once

A module is evaluated the first time it is loaded in a session, and every
importer shares that instance: module-level state is shared, and
`pljs.import('x') === pljs.import('x')`. Each user has their own JavaScript
context, so the instance is per user per session.

What a name resolved to is remembered too, so importing in a function that is
called many times costs a property lookup, not catalog reads.

Importing a module that is already loading (a cycle through `pljs.import()` or
`pljs.require()`) is an error. Cycles between static `import` statements are
fine, as they are in any ES module system. Nested loads are also limited by
`pljs.max_module_depth`.

## Changing a module

Changing `pljs.modules` or `pljs.packages` takes effect in every session,
without a reload and without calling anything:

```sql
UPDATE pljs.modules SET source = '...' WHERE path = 'greeter';
-- the next call of greet() in any session uses the new source
```

This covers `INSERT`, `UPDATE`, `DELETE` and `TRUNCATE`, new versions of a
package, a package's `dependencies` or `entry_point`, a change made in the
current transaction (visible to it at once), and a change that is rolled back
(undone). It is the table's statement-level trigger, `pljs_modules_changed`,
that does this; it is created with the extension and is `ENABLE ALWAYS`, so it
also runs during replication and restores. **Do not disable it**: sessions
would keep running the modules they loaded before.

Any change throws away every loaded module, not only the one that changed, so
the next import of each evaluates it again.

Two things follow from how QuickJS works:

- A context cannot unload a module. The old instances stay in memory, no
  longer reachable by name, until `pljs_reset()` or the end of the session. A
  session that rewrites modules constantly grows; call `pljs_reset()` from time
  to time if that matters to you.
- A module that throws while it is evaluated is evaluated again the next time
  it is imported, rather than throwing the same error forever.

## Backups

`pg_dump` includes the contents of `pljs.modules` and `pljs.packages`, and a restore puts
them back, so modules installed in a database go with it. Nothing needs to be done to
turn this on. A database made from an earlier build of 1.1.0-dev has the tables but not
the setting; run this once in it:

```sql
SELECT pg_extension_config_dump('pljs.packages', '');
SELECT pg_extension_config_dump('pljs.modules', '');
```

## Security

Loading reads the tables directly and does not check privileges on them: any
role that can run a PL/JS function can import any stored module. Control who
can *write* the tables, since what is written there runs as whoever imports it.

## Encoding

Source is converted from the database encoding to UTF-8 when it is loaded, the
way a function's source is.
