# pljs Import/Require Rewriting Rules

This document is the single source of truth for how the `@plv8/pljs-cli` tool transforms JavaScript import and require statements for use with the pljs PostgreSQL extension. Both the rewriter implementation and its tests reference this document.

## Context

pljs embeds QuickJS inside PostgreSQL as a trusted procedural language. Modules are stored in the `pljs.modules` table and loaded via `pljs.import()` (ES6) or `pljs.require()` (CommonJS). The CLI tool rewrites standard JavaScript module syntax into these calls before uploading to the database.

---

## ES6 Import Transforms

All ES6 `import` declarations are converted to `const` variable declarations using `pljs.import()`.

### Default Import

```js
// Input
import foo from 'foo';

// Output
const foo = pljs.import('foo');
```

### Named Imports

```js
// Input
import { bar, baz } from 'foo';

// Output
const { bar, baz } = pljs.import('foo');
```

### Aliased Named Imports

```js
// Input
import { bar as b, baz as z } from 'foo';

// Output
const { bar: b, baz: z } = pljs.import('foo');
```

### Namespace Import

```js
// Input
import * as foo from 'foo';

// Output
const foo = pljs.import('foo');
```

### Side-Effect Import

```js
// Input
import 'foo';

// Output
pljs.import('foo');
```

### Default + Named Combined

```js
// Input
import foo, { bar, baz } from 'foo';

// Output
const __imp0 = pljs.import('foo');
const foo = __imp0.default;
const { bar, baz } = __imp0;
```

### Multiple Imports from Same Module

Each import declaration is transformed independently. pljs caches modules internally, so repeated `pljs.import()` calls for the same module are not a performance concern.

```js
// Input
import { a } from 'foo';
import { b } from 'foo';

// Output
const { a } = pljs.import('foo');
const { b } = pljs.import('foo');
```

---

## ES6 Re-Export Transforms

### Named Re-Export

```js
// Input
export { foo, bar } from 'baz';

// Output — handled at module boundary, converted to import + re-export
const __re0 = pljs.import('baz');
export const foo = __re0.foo;
export const bar = __re0.bar;
```

### Re-Export with Alias

```js
// Input
export { foo as f } from 'baz';

// Output
const __re0 = pljs.import('baz');
export const f = __re0.foo;
```

### Re-Export All

```js
// Input
export * from 'foo';

// Output — not supported, error with clear message
// ERROR: "export * from" is not supported in pljs. Use named re-exports instead.
```

### Re-Export Default

```js
// Input
export { default } from 'foo';

// Output
const __re0 = pljs.import('foo');
export default __re0.default;
```

---

## CommonJS Require Transforms

All `require()` calls with string literal arguments are converted to `pljs.require()`.

### Basic Require

```js
// Input
const foo = require('foo');

// Output
const foo = pljs.require('foo');
```

### Destructured Require

```js
// Input
const { bar, baz } = require('foo');

// Output
const { bar, baz } = pljs.require('foo');
```

### Var/Let Require

```js
// Input
var foo = require('foo');
let bar = require('bar');

// Output
var foo = pljs.require('foo');
let bar = pljs.require('bar');
```

### Bare Require (No Assignment)

```js
// Input
require('foo');

// Output
pljs.require('foo');
```

### Chained Require

```js
// Input
const bar = require('foo').bar;

// Output
const bar = pljs.require('foo').bar;
```

### Require in Expression

```js
// Input
console.log(require('foo').version);

// Output
console.log(pljs.require('foo').version);
```

---

## Path Rewriting

### Relative Paths Within a Package

Relative paths (`./`, `../`) within a package are rewritten to package-qualified paths using `packageName/resolved/path` format. The `.js` extension is stripped.

```js
// Inside package "mylib", file: src/index.js

// Input
import { util } from './lib/util';
import { helper } from '../common/helper.js';
const data = require('./data');

// Output
const { util } = pljs.import('mylib/src/lib/util');
const { helper } = pljs.import('mylib/common/helper');
const data = pljs.require('mylib/src/data');
```

Resolution: the relative path is resolved against the current file's location within the package, then prefixed with the package name.

### Bare Specifiers (Package Names)

Non-relative specifiers are treated as package names and left as-is (or with sub-path):

```js
// Input
import lodash from 'lodash';
import get from 'lodash/get';

// Output
const lodash = pljs.import('lodash');
const get = pljs.import('lodash/get');
```

### Index File Resolution

If a relative import points to a directory, resolve to the directory's index file:

```js
// ./lib/ contains index.js

// Input
import lib from './lib';

// Output (resolved to ./lib/index.js, then package-qualified)
const lib = pljs.import('mylib/src/lib/index');
```

---

## Dynamic Import/Require

### String Literal Arguments — Rewrite

```js
// Input
const foo = await import('foo');
const bar = require('foo');

// Output
const foo = await pljs.import('foo');
const bar = pljs.require('foo');
```

### Non-String Arguments — Warn and Pass Through

```js
// Input
const mod = require(varName);
const mod2 = await import(getModuleName());

// Output (unchanged)
const mod = require(varName);
const mod2 = await import(getModuleName());

// CLI Warning: "Dynamic require/import with non-string argument at file.js:3 — cannot rewrite, leaving as-is"
```

---

## Node.js Built-in Resolution

When an import references a Node.js built-in module, the CLI attempts to resolve a pure-JS fallback before erroring. pljs runs inside PostgreSQL as a trusted extension and cannot access the filesystem, network, or OS.

### Built-in Module List

`assert`, `async_hooks`, `buffer`, `child_process`, `cluster`, `crypto`, `dgram`, `dns`, `events`, `fs`, `http`, `http2`, `https`, `inspector`, `net`, `os`, `path`, `perf_hooks`, `querystring`, `readline`, `stream`, `string_decoder`, `tls`, `timers`, `trace_events`, `tty`, `url`, `util`, `v8`, `vm`, `wasi`, `worker_threads`, `zlib`

Any `node:` prefixed import (e.g. `node:fs`, `node:path`) is also a built-in.

### Fallback Resolution Order

1. **`browser` field in package.json** — if the package maps the built-in to a local file path, use that file instead. If mapped to `false`, replace with an empty module (`module.exports = {}`).

2. **`exports` field in package.json** — check for condition keys `browser` or `default` that provide an alternative entry point for the package.

3. **Polyfill in node_modules** — check if `node_modules` contains a pure-JS npm package with the same name as the built-in (e.g. `buffer`, `events`, `util`, `path-browserify`, `stream-browserify`, `crypto-browserify`). These are common browser polyfills.

4. **Error** — if no fallback resolves:
   ```
   ERROR: Unsupported Node.js built-in 'fs' imported in src/file-reader.js:3
   pljs runs inside PostgreSQL as a trusted extension and cannot access the filesystem.
   No browser/polyfill fallback was found for this import.
   ```

### Common Polyfill Mappings

These npm packages are recognized as polyfills for their corresponding built-ins:

| Built-in | Polyfill Package(s) |
|----------|---------------------|
| buffer | `buffer` |
| crypto | `crypto-browserify` |
| events | `events` |
| http | `stream-http` |
| https | `https-browserify` |
| os | `os-browserify` |
| path | `path-browserify`, `path` |
| stream | `stream-browserify`, `readable-stream` |
| url | `url` |
| util | `util` |
| zlib | `browserify-zlib` |
| assert | `assert` |
| querystring | `querystring-es3` |
| string_decoder | `string_decoder` |
| timers | `timers-browserify` |
| tty | `tty-browserify` |
| vm | `vm-browserify` |

---

## What Must NOT Be Rewritten

The rewriter must leave these untouched:

### Imports in Comments

```js
// import foo from 'foo'; ← must not rewrite
/* require('foo'); */      ← must not rewrite
```

### Imports in String Literals

```js
const s = "import foo from 'foo'";  // ← must not rewrite
const t = `require('bar')`;         // ← must not rewrite
```

### Non-Module require() Calls

```js
// require.resolve, require.cache, etc. — leave as-is
const p = require.resolve('foo');
delete require.cache[require.resolve('foo')];
```

---

## Error Conditions

The CLI must error (not warn) on these:

1. **Unresolvable Node.js built-in** — no browser/polyfill fallback found
2. **`export * from`** — not supported in pljs module system
3. **Syntax errors** — acorn can't parse the file

The CLI must warn (not error) on these:

1. **Dynamic require/import with non-string argument** — can't rewrite, left as-is
2. **Failed transitive dependency** — warn and continue with other deps
