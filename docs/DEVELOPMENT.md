# PLJS Development

PLJS provides JavaScript functionality as a language extension for Postgres. It's implemented in `C` and built using `make` and a `C` compiler.

## Building

As long as pre-requisites are met, building can be as simple as running `make`:

```bash
make
```

and installation by running:

```bash
make install
```

Some specific packages and software need to be installed depending on the operating system in use:

### MacOS

- XCode (provides `clang`, `make`, and `git`)
- Postgres (such as with `brew`)

### Linux

- Postgres development packages
- `git`
- `gcc` (typically with `build-essential` or `development tools` depending on operating system)
- `make` (generally installed with the above packages)

## Testing

PLJS uses Postgres' testing framework.

### Tests

Tests use Postgres' [regression testing framework](https://www.postgresql.org/docs/current/regress.html), thus running `make installcheck` after running `make install` will run the test suite and provide results. Tests live in the `sql` directory and are named according to their tests.

## File Naming Conventions

Files are named by function.

### Source

All source for the PLJS extension can be found in the `src` directory:

- `pljs.h` - Header file that defines functions and `structs`
- `pljs.c` - Main entry point for PLJS, including setup and teardown
- `cache.c` - Context and function caching
- `functions.c` - Functions available from PLV8
- `types.c` - Type conversion to/from Postgres/JavaScript
- `params.c` - Parameter management for Postgres

### Tests

All tests live in the `sql` directory, and their expected results live in the `expected` directory.

Tests are named by the function that they test.

## Function Naming Conventions

Naming conventions for this project generally favor using `pljs_` as a prefix for functions that are not private (not marked `static`) in the project. `C` functions accessible from JavaScript are also prefixed with `pljs_` and marked as static, indicating their availability within the JavaScript engine.

A function scoped to `pljs.c`](../src/pljs.c)`:

```c
static Datum call_function(PG_FUNCTION_ARGS, pljs_context *context,
                           JSValueConst *argv);
```

A function available throughout the project:

```c
bool pljs_has_permission_to_execute(const char *signature);
```

A function available in JavaScript:

```c
static JSValue pljs_elog(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv);
```
