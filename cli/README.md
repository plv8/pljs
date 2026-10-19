# @plv8/pljs-cli

A command-line tool for installing JavaScript modules and packages into PostgreSQL for use with the [pljs](https://github.com/plv8/pljs) trusted procedural language extension.

pljs embeds QuickJS inside PostgreSQL, letting you write stored procedures in JavaScript. This CLI manages the module system — it takes standard JavaScript files and npm packages, rewrites their `import`/`require` statements to work with pljs, and uploads them to the database.

## Install

```bash
npm install -g @plv8/pljs-cli
```

Requires Node.js 18 or later.

## Quick Start

```bash
# Install a single JavaScript file as a standalone module
pljs install ./my-utils.js --dbname mydb

# Install an npm package
pljs install lodash --dbname mydb

# Use it in PostgreSQL
psql -d mydb -c "
  CREATE FUNCTION slugify(text) RETURNS text AS \$\$
    var _ = pljs.require('lodash');
    return _.kebabCase(text);
  \$\$ LANGUAGE pljs;
"
```

## Commands

### `pljs install <target>`

Installs a module or package into the database. The CLI detects what you're installing:

| Target | What happens |
|--------|-------------|
| `./file.js` | Installs as a standalone module |
| `./my-package/` (has `package.json` + `node_modules/`) | Processes and uploads the package |
| `./my-package/` (has `package.json`, no `node_modules/`) | Runs `npm install`, then processes and uploads |
| `lodash` | Installs from npm into a temp directory, processes, uploads |
| `lodash@4.17.21` | Same, with a pinned version |

**Options:**

- `--as <name>` — Override the module name for standalone file installs (default: filename without extension)
- `--force` — Overwrite if the module or package already exists
- `--sloppy` — Run the installed CommonJS modules in sloppy mode (see below)

**What it does to your code:**

The CLI rewrites `import` and `require` statements so they work inside pljs:

```js
// Your code                          // What gets stored in PostgreSQL
import { get } from 'lodash';     →  const { get } = pljs.import('lodash');
const fs = require('fs');          →  const fs = pljs.require('fs');
import { helper } from './lib';   →  const { helper } = pljs.import('mypackage/lib');
```

Relative paths within a package are rewritten to package-qualified paths. See `docs/IMPORT_RULES.md` for the complete set of rewriting rules.

**Strict and sloppy mode:**

pljs runs CommonJS modules in strict mode by default, as ES modules always are. Older
code that depends on sloppy mode -- assigning to a variable that was never declared,
`with`, octal literals like `010` -- then fails to load, with a `ReferenceError` or a
`SyntaxError`. Install it with `--sloppy` and it runs the way Node would run it:

```bash
pljs install ./legacy-utils.js --sloppy --dbname mydb
```

`--sloppy` applies to every module the command installs, including a package's
dependencies, and is stored with the module (the `sloppy` column of `pljs.modules`), so
it can be changed afterwards with an `UPDATE`. It has no effect on an ES module, which is
always strict. It needs a pljs version that has the `sloppy` column; installing without
the flag works with older ones.

**Node.js built-in handling:**

pljs runs inside PostgreSQL as a trusted extension — it cannot access the filesystem, network, or OS. When the CLI encounters an import of a Node.js built-in (`fs`, `path`, `crypto`, etc.), it attempts to find a pure-JS fallback:

1. Checks the package's `browser` field in `package.json`
2. Checks the `exports` field for browser/default conditions
3. Looks for a polyfill package in `node_modules` (e.g., `events`, `path-browserify`)

If no fallback is found, the install fails with a clear error message.

**Transitive dependencies:**

When installing a package, its dependencies are also installed (best-effort). Each dependency becomes a separate entry in `pljs.packages` with its own version, enabling semver-based resolution at runtime.

### `pljs remove <target>`

Removes a module or package from the database.

```bash
pljs remove lodash              # Remove all versions of the lodash package
pljs remove lodash@4.17.21     # Remove a specific version
pljs remove my-utils            # Remove a standalone module
```

### `pljs list`

Lists installed packages and standalone modules.

```bash
pljs list
```

```
Packages:
  Name                   Version      Entry Point
  -------------------------------------------------------
  lodash                 4.17.21      index

Standalone Modules:
  my-utils
  helpers
```

## Connecting to PostgreSQL

The CLI needs to connect to a PostgreSQL database that has the pljs extension installed. Connection details can be provided in three ways (highest priority first):

**1. Connection string:**
```bash
pljs list --connection-string "postgres://user:pass@localhost:5432/mydb"
```

**2. Individual flags:**
```bash
pljs list --host localhost --port 5432 --dbname mydb --username myuser --password mypass
```

**3. Environment variables:**
```bash
export PGHOST=localhost
export PGPORT=5432
export PGDATABASE=mydb
export PGUSER=myuser
export PGPASSWORD=mypass
pljs list
```

The standard `PG*` environment variables work out of the box. Flags override environment variables, and a connection string overrides everything.

## Global Options

| Flag | Description |
|------|-------------|
| `--connection-string` | PostgreSQL connection string |
| `--host` | Database host |
| `--port` | Database port |
| `--dbname` | Database name |
| `--username` | Database user |
| `--password` | Database password |
| `--json` | Output as JSON instead of human-readable text |
| `--force` | Overwrite existing modules/packages |
| `--sloppy` | Run installed CommonJS modules in sloppy mode |
| `--help` | Show help |

## Database Schema

The CLI writes to two tables created by the pljs extension:

- `pljs.packages` — Installed packages (name, version, entry point, dependency map)
- `pljs.modules` — Module source code (path, source, optional package reference)

All writes happen within a single transaction. If anything fails, the database is left unchanged.

## Testing

```bash
cd cli

# Unit tests (no database required)
npm test

# Integration tests (requires PostgreSQL with pljs installed)
node --test --experimental-test-module-mocks test/integration/e2e.test.js
```

## License

PostgreSQL License
