import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { isNodeBuiltin, resolveBuiltinFallback } from '../../src/transform/scanner.js';

describe('isNodeBuiltin', () => {
  it('detects fs', () => assert.ok(isNodeBuiltin('fs')));
  it('detects path', () => assert.ok(isNodeBuiltin('path')));
  it('detects crypto', () => assert.ok(isNodeBuiltin('crypto')));
  it('detects http', () => assert.ok(isNodeBuiltin('http')));
  it('detects child_process', () => assert.ok(isNodeBuiltin('child_process')));
  it('detects node: prefix', () => assert.ok(isNodeBuiltin('node:fs')));
  it('detects node:path', () => assert.ok(isNodeBuiltin('node:path')));
  it('does not detect lodash', () => assert.ok(!isNodeBuiltin('lodash')));
  it('does not detect acorn', () => assert.ok(!isNodeBuiltin('acorn')));
  it('does not detect relative path', () => assert.ok(!isNodeBuiltin('./foo')));
});

describe('resolveBuiltinFallback', () => {
  it('resolves via browser field string mapping', () => {
    const result = resolveBuiltinFallback('fs', {
      browser: { fs: './lib/fs-polyfill.js' },
    });
    assert.strictEqual(result.type, 'browser-field');
    assert.strictEqual(result.replacement, './lib/fs-polyfill.js');
  });

  it('resolves via browser field false (disabled)', () => {
    const result = resolveBuiltinFallback('fs', {
      browser: { fs: false },
    });
    assert.strictEqual(result.type, 'disabled');
  });

  it('resolves via polyfill mapping', () => {
    const result = resolveBuiltinFallback('events', {
      nodeModulesDir: '/fake/node_modules',
      existsSync: (p) => p.includes('events'),
    });
    assert.strictEqual(result.type, 'polyfill');
    assert.strictEqual(result.replacement, 'events');
  });

  it('resolves crypto to crypto-browserify', () => {
    const result = resolveBuiltinFallback('crypto', {
      nodeModulesDir: '/fake/node_modules',
      existsSync: (p) => p.includes('crypto-browserify'),
    });
    assert.strictEqual(result.type, 'polyfill');
    assert.strictEqual(result.replacement, 'crypto-browserify');
  });

  it('returns null when no fallback exists', () => {
    const result = resolveBuiltinFallback('fs', {
      existsSync: () => false,
    });
    assert.strictEqual(result, null);
  });

  it('returns null when polyfill map entries exist but none are found on disk', () => {
    // 'events' has a polyfill map entry ['events'], but existsSync returns false
    const result = resolveBuiltinFallback('events', {
      nodeModulesDir: '/fake/node_modules',
      existsSync: () => false,
    });
    assert.strictEqual(result, null);
  });

  it('strips node: prefix before resolving', () => {
    const result = resolveBuiltinFallback('node:events', {
      nodeModulesDir: '/fake/node_modules',
      existsSync: (p) => p.includes('events'),
    });
    assert.strictEqual(result.type, 'polyfill');
    assert.strictEqual(result.replacement, 'events');
  });
});
