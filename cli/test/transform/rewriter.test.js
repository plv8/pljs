import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { rewrite } from '../../src/transform/rewriter.js';

describe('rewriter - ES6 imports', () => {
  it('rewrites default import', () => {
    const input = `import foo from 'foo';`;
    const output = rewrite(input);
    assert.strictEqual(output, `const foo = pljs.import('foo');`);
  });

  it('rewrites named imports', () => {
    const input = `import { bar, baz } from 'foo';`;
    const output = rewrite(input);
    assert.strictEqual(output, `const { bar, baz } = pljs.import('foo');`);
  });

  it('rewrites aliased named imports', () => {
    const input = `import { bar as b, baz as z } from 'foo';`;
    const output = rewrite(input);
    assert.strictEqual(output, `const { bar: b, baz: z } = pljs.import('foo');`);
  });

  it('rewrites namespace import', () => {
    const input = `import * as foo from 'foo';`;
    const output = rewrite(input);
    assert.strictEqual(output, `const foo = pljs.import('foo');`);
  });

  it('rewrites side-effect import', () => {
    const input = `import 'foo';`;
    const output = rewrite(input);
    assert.strictEqual(output, `pljs.import('foo');`);
  });

  it('rewrites default + named combined import', () => {
    const input = `import foo, { bar, baz } from 'foo';`;
    const output = rewrite(input);
    const expected = [
      `const __imp0 = pljs.import('foo');`,
      `const foo = __imp0.default;`,
      `const { bar, baz } = __imp0;`,
    ].join('\n');
    assert.strictEqual(output, expected);
  });

  it('rewrites multiple imports from same module', () => {
    const input = `import { a } from 'foo';\nimport { b } from 'foo';`;
    const output = rewrite(input);
    const expected = `const { a } = pljs.import('foo');\nconst { b } = pljs.import('foo');`;
    assert.strictEqual(output, expected);
  });

  it('preserves non-import code around imports', () => {
    const input = `const x = 1;\nimport foo from 'foo';\nconst y = 2;`;
    const output = rewrite(input);
    assert.ok(output.includes(`const foo = pljs.import('foo');`));
    assert.ok(output.includes('const x = 1;'));
    assert.ok(output.includes('const y = 2;'));
  });
});

describe('rewriter - CommonJS requires', () => {
  it('rewrites const require', () => {
    const input = `const foo = require('foo');`;
    const output = rewrite(input);
    assert.strictEqual(output, `const foo = pljs.require('foo');`);
  });

  it('rewrites var require', () => {
    const input = `var foo = require('foo');`;
    const output = rewrite(input);
    assert.strictEqual(output, `var foo = pljs.require('foo');`);
  });

  it('rewrites let require', () => {
    const input = `let foo = require('foo');`;
    const output = rewrite(input);
    assert.strictEqual(output, `let foo = pljs.require('foo');`);
  });

  it('rewrites destructured require', () => {
    const input = `const { bar, baz } = require('foo');`;
    const output = rewrite(input);
    assert.strictEqual(output, `const { bar, baz } = pljs.require('foo');`);
  });

  it('rewrites bare require', () => {
    const input = `require('foo');`;
    const output = rewrite(input);
    assert.strictEqual(output, `pljs.require('foo');`);
  });

  it('rewrites chained require', () => {
    const input = `const bar = require('foo').bar;`;
    const output = rewrite(input);
    assert.strictEqual(output, `const bar = pljs.require('foo').bar;`);
  });

  it('rewrites require in expression', () => {
    const input = `console.log(require('foo').version);`;
    const output = rewrite(input);
    assert.strictEqual(output, `console.log(pljs.require('foo').version);`);
  });

  it('does not rewrite require.resolve', () => {
    const input = `const p = require.resolve('foo');`;
    const output = rewrite(input);
    assert.strictEqual(output, `const p = require.resolve('foo');`);
  });

  it('does not rewrite require with variable argument', () => {
    const warnings = [];
    const input = `const mod = require(varName);`;
    const result = rewrite(input, { warnings });
    assert.strictEqual(result, `const mod = require(varName);`);
    assert.strictEqual(warnings.length, 1);
  });

  it('handles mixed imports and requires', () => {
    const input = `import foo from 'foo';\nconst bar = require('bar');`;
    const output = rewrite(input);
    assert.ok(output.includes(`const foo = pljs.import('foo');`));
    assert.ok(output.includes(`const bar = pljs.require('bar');`));
  });
});

describe('rewriter - re-exports', () => {
  it('rewrites named re-export', () => {
    const input = `export { foo, bar } from 'baz';`;
    const output = rewrite(input);
    const expected = [
      `const __re0 = pljs.import('baz');`,
      `export const foo = __re0.foo;`,
      `export const bar = __re0.bar;`,
    ].join('\n');
    assert.strictEqual(output, expected);
  });

  it('rewrites aliased re-export', () => {
    const input = `export { foo as f } from 'baz';`;
    const output = rewrite(input);
    const expected = [
      `const __re0 = pljs.import('baz');`,
      `export const f = __re0.foo;`,
    ].join('\n');
    assert.strictEqual(output, expected);
  });

  it('rewrites default re-export', () => {
    const input = `export { default } from 'foo';`;
    const output = rewrite(input);
    const expected = [
      `const __re0 = pljs.import('foo');`,
      `export default __re0.default;`,
    ].join('\n');
    assert.strictEqual(output, expected);
  });

  it('errors on export * from', () => {
    const input = `export * from 'foo';`;
    assert.throws(() => rewrite(input), {
      message: /export \* from.*not supported/,
    });
  });
});

describe('rewriter - dynamic import', () => {
  it('rewrites dynamic import with string literal', () => {
    const input = `const foo = await import('foo');`;
    const output = rewrite(input);
    assert.strictEqual(output, `const foo = await pljs.import('foo');`);
  });

  it('leaves dynamic import with variable alone and warns', () => {
    const warnings = [];
    const input = `const mod = await import(varName);`;
    const output = rewrite(input, { warnings });
    assert.strictEqual(output, input);
    assert.strictEqual(warnings.length, 1);
  });
});

describe('rewriter - script fallback', () => {
  it('falls back to script parsing for code that fails module parse', () => {
    // "with" statement is not valid in module mode (strict mode), but valid in script mode
    const input = `with (obj) { x = 1; }`;
    const output = rewrite(input);
    // Should succeed by falling back to script parse
    assert.strictEqual(output, input);
  });
});

describe('rewriter - must not rewrite', () => {
  it('does not rewrite imports in line comments', () => {
    const input = `// import foo from 'foo';\nconst x = 1;`;
    const output = rewrite(input);
    assert.strictEqual(output, input);
  });

  it('does not rewrite imports in block comments', () => {
    const input = `/* import foo from 'foo'; */\nconst x = 1;`;
    const output = rewrite(input);
    assert.strictEqual(output, input);
  });

  it('does not rewrite require in string literals', () => {
    const input = `const s = "require('foo')";`;
    const output = rewrite(input);
    assert.strictEqual(output, input);
  });

  it('does not rewrite require in template literals', () => {
    const input = "const s = `require('foo')`;";
    const output = rewrite(input);
    assert.strictEqual(output, input);
  });

  it('does not rewrite require.cache', () => {
    const input = `delete require.cache[require.resolve('foo')];`;
    const output = rewrite(input);
    assert.strictEqual(output, input);
  });
});

describe('rewriter - resolveSpecifier callback', () => {
  it('applies resolveSpecifier to import specifier', () => {
    const input = `import foo from './lib/foo';`;
    const output = rewrite(input, {
      resolveSpecifier: (s) => s === './lib/foo' ? 'mylib/lib/foo' : s,
    });
    assert.strictEqual(output, `const foo = pljs.import('mylib/lib/foo');`);
  });

  it('applies resolveSpecifier to require specifier', () => {
    const input = `const foo = require('./lib/foo');`;
    const output = rewrite(input, {
      resolveSpecifier: (s) => s === './lib/foo' ? 'mylib/lib/foo' : s,
    });
    assert.strictEqual(output, `const foo = pljs.require('mylib/lib/foo');`);
  });

  it('replaces disabled built-in with empty object', () => {
    const input = `const fs = require('fs');`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.strictEqual(output, `const fs = {};`);
  });

  it('replaces disabled import with empty objects for default specifier', () => {
    const input = `import fs from 'fs';`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.strictEqual(output, `const fs = {};`);
  });

  it('replaces disabled import with empty objects for namespace specifier', () => {
    const input = `import * as fs from 'fs';`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.strictEqual(output, `const fs = {};`);
  });

  it('replaces disabled import with empty objects for named specifiers', () => {
    const input = `import { readFile, writeFile } from 'fs';`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.ok(output.includes('const readFile = {};'));
    assert.ok(output.includes('const writeFile = {};'));
  });

  it('replaces disabled side-effect import with empty expression', () => {
    const input = `import 'fs';`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.strictEqual(output, `({});`);
  });

  it('replaces disabled re-export with empty object exports', () => {
    const input = `export { foo, bar } from 'disabled-mod';`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.ok(output.includes('export const foo = {};'));
    assert.ok(output.includes('export const bar = {};'));
  });

  it('replaces disabled re-export of default with export default {}', () => {
    const input = `export { default } from 'disabled-mod';`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.ok(output.includes('export default {};'));
  });

  it('replaces disabled dynamic import with empty object', () => {
    const input = `const mod = await import('disabled');`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.ok(output.includes('({})'));
  });

  it('resolves dynamic import with resolveSpecifier', () => {
    const input = `const mod = await import('foo');`;
    const output = rewrite(input, {
      resolveSpecifier: (s) => `resolved/${s}`,
    });
    assert.ok(output.includes("pljs.import('resolved/foo')"));
  });

  it('replaces bare disabled require with empty object', () => {
    const input = `require('fs');`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.strictEqual(output, `({});`);
  });

  it('replaces disabled destructured require with empty object', () => {
    const input = `const { readFile } = require('fs');`;
    const output = rewrite(input, {
      resolveSpecifier: () => null,
    });
    assert.strictEqual(output, `const { readFile } = {};`);
  });
});
