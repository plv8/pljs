# PLJS

PLJS is a trusted JavaScript Language Extension for PostgreSQL.

It is compact, lightweight, and fast.

A [Discord](https://discord.gg/XYGSCfVNBC) is available for general questions, discussions, and support. Please check there before opening an issue.

## Technology

JavaScript: [QuickJS](https://bellard.org/quickjs/quickjs.html)

PostgreSQL: 14+

### Current Status

1.0.1 released.

## Using PLJS

Once the extension has been installed (see [Building](docs/DEVELOPMENT.md)), you can run:

```sql
CREATE EXTENSION pljs;
```

from your SQL REPL.

You can test that it works by executing:

```
DO $$ pljs.elog(NOTICE, "Hello, World!") $$ LANGUAGE pljs;
```

## Documentation

- [Integrations](docs/INTEGRATION.md) - How PLJS integrates with Postgres
- [Types](docs/TYPES.md) - Type conversion between Postgres and JavaScript
- [Functions](docs/FUNCTIONS.md) - Functions and functionality provided by PLJS
- [Configuration](docs/CONFIGURATION.md) - Configuration options
- [Development](docs/DEVELOPMENT.md) - How to build and develop PLJS
- [Versioning](docs/VERSIONING.md) - PLJS's versioning policies
- [Change Log](docs/CHANGELOG.md) - Release change log
- [Roadmap](docs/ROADMAP.md) - PLJS's development roadmap
- [Benchmarks](docs/BENCHMARKS.md) - Benchmarks and comparisons with [PLV8](https://github.com/plv8/pljs)
