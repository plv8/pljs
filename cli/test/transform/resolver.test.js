import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { resolveModulePath } from '../../src/transform/resolver.js';

describe('resolveModulePath', () => {
  it('leaves bare specifier as-is', () => {
    const result = resolveModulePath('lodash', {});
    assert.strictEqual(result, 'lodash');
  });

  it('leaves bare specifier with subpath as-is', () => {
    const result = resolveModulePath('lodash/get', {});
    assert.strictEqual(result, 'lodash/get');
  });

  it('resolves relative path within package', () => {
    const result = resolveModulePath('./lib/util', {
      packageName: 'mylib',
      filePath: 'src/index.js',
    });
    assert.strictEqual(result, 'mylib/src/lib/util');
  });

  it('resolves parent-relative path within package', () => {
    const result = resolveModulePath('../common/helper.js', {
      packageName: 'mylib',
      filePath: 'src/index.js',
    });
    assert.strictEqual(result, 'mylib/common/helper');
  });

  it('strips .js extension', () => {
    const result = resolveModulePath('./util.js', {
      packageName: 'mylib',
      filePath: 'src/index.js',
    });
    assert.strictEqual(result, 'mylib/src/util');
  });

  it('strips .mjs extension', () => {
    const result = resolveModulePath('./util.mjs', {
      packageName: 'mylib',
      filePath: 'index.js',
    });
    assert.strictEqual(result, 'mylib/util');
  });

  it('strips .cjs extension', () => {
    const result = resolveModulePath('./util.cjs', {
      packageName: 'mylib',
      filePath: 'index.js',
    });
    assert.strictEqual(result, 'mylib/util');
  });

  it('returns as-is when no packageName (standalone module)', () => {
    const result = resolveModulePath('./lib/util', {});
    assert.strictEqual(result, './lib/util');
  });
});
