# PLJS Change Log

## 1.0.0

Released _June 25, 2025_.

- Support of PLV8 functionality
- Documentation created
- First initial release

### 1.0.1

Released _July 1, 2025_.

- Remove extraneous include from `types.c` that stopped PG14/15 compilation
- Add PG14/15 compilation to the CI

### 1.0.2

Released _August 20, 2025_.

- Fix function name collision with QuickJS
- Add missing `subtransaction` support
- Fix memory context switching before `CopyErrorData()`

### 1.0.3

Released _August 20, 2025_.

- PGXN release went out with an incorrect control file

### 1.0.4

Released _January 11, 2026_.

- Up memory default limit to 512MB
- Remove unnecessary include from modules.c
- Remove extra running of GC after each execution
- Better handling of SRF's, including freeing resources
- Increased test timeouts to accommodate slower CI machines
- Alter memory limit minimum to 64MB to facilitate testing
- Alter some tests for s390x testing
- Clean up parameter orders for function calls
- Fixed potential memory leak in `pljs_jsvalue_to_datums`

### 1.0.5

Released _January 27, 2026_.

- Fix key name issue in JSONB_DIRECT_CONVERSION path

### 1.1.0

- Added `pljs.import`
- Added initial hook support
- Added `pljs.require` for commonjs modules
- Added packages (`pljs.packages`) with semver dependency resolution
- Fixed `^` ranges below 1.0.0, scoped package names (`@scope/pkg`), and
  versions with a pre-release suffix, which were taken for releases
- Errors raised while loading a module reach the caller with their message
- CommonJS modules get their own `module` and `exports` instead of globals
- Module source is converted from the database encoding to UTF-8
- Fixed module and package lookups missing rows when the database collation is
  not `C`
- Stored modules load through the QuickJS module system: `import` statements
  and relative paths work inside modules, and each module is evaluated once
  per session and shared
- Changing `pljs.modules` or `pljs.packages` takes effect in every session
  without a reload (see [MODULES.md](MODULES.md))
- CommonJS modules run in strict mode, as ES modules do, unless their row in
  `pljs.modules` has `sloppy` set
- `import` statements at the start of a function or `DO` block (the function
  then runs as a module, so in strict mode)
- Upgrade scripts from every 1.0.x release (`ALTER EXTENSION pljs UPDATE`)
- `pg_dump` includes the contents of `pljs.modules` and `pljs.packages`
