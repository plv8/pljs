import { describe, it, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { processStandaloneSource, processPackageSource, processPackage, ensureNodeModules } from '../../src/package/installer.js';

describe('processStandaloneSource', () => {
  it('rewrites requires in standalone file', () => {
    const input = `const foo = require('foo');\nmodule.exports = foo;`;
    const result = processStandaloneSource(input);
    assert.ok(result.source.includes("pljs.require('foo')"));
  });

  it('rewrites imports in standalone file', () => {
    const input = `import foo from 'foo';\nexport default foo;`;
    const result = processStandaloneSource(input);
    assert.ok(result.source.includes("pljs.import('foo')"));
  });
});

describe('processPackageSource', () => {
  it('rewrites and resolves relative paths', () => {
    const input = `import { util } from './lib/util';`;
    const result = processPackageSource(input, {
      packageName: 'mylib',
      filePath: 'src/index.js',
    });
    assert.ok(result.source.includes("pljs.import('mylib/src/lib/util')"));
  });

  it('rewrites bare specifier without modification', () => {
    const input = `const lodash = require('lodash');`;
    const result = processPackageSource(input, {
      packageName: 'mylib',
      filePath: 'index.js',
    });
    assert.ok(result.source.includes("pljs.require('lodash')"));
  });

  it('collects warnings for dynamic requires', () => {
    const input = `const mod = require(varName);`;
    const result = processPackageSource(input, {
      packageName: 'mylib',
      filePath: 'index.js',
    });
    assert.strictEqual(result.warnings.length, 1);
  });

  it('throws for unsupported node builtin with no fallback', () => {
    const input = `import fs from 'fs';`;
    assert.throws(
      () => processPackageSource(input, {
        packageName: 'mylib',
        filePath: 'index.js',
        browser: null,
        nodeModulesDir: '/nonexistent',
      }),
      { message: /Unsupported Node\.js built-in 'fs'/ },
    );
  });

  it('disables node builtin when browser field maps it to false', () => {
    const input = `import fs from 'fs';`;
    const result = processPackageSource(input, {
      packageName: 'mylib',
      filePath: 'index.js',
      browser: { fs: false },
      nodeModulesDir: '/fake',
    });
    assert.ok(result.source.includes('const fs = {};'));
  });

  it('replaces node builtin with browser field string mapping', () => {
    const input = `import fs from 'fs';`;
    const result = processPackageSource(input, {
      packageName: 'mylib',
      filePath: 'index.js',
      browser: { fs: './lib/fs-shim.js' },
      nodeModulesDir: '/fake',
    });
    assert.ok(result.source.includes("pljs.import('./lib/fs-shim.js')"));
  });
});

describe('processPackage', () => {
  let tmpDir;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-installer-test-'));
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('processes a simple package with one file', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'test-pkg',
      version: '1.0.0',
      main: 'index.js',
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `export default function hello() { return 'hello'; }`);

    const nodeModulesDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nodeModulesDir);
    const result = processPackage(tmpDir, nodeModulesDir);

    assert.strictEqual(result.package.name, 'test-pkg');
    assert.strictEqual(result.package.version, '1.0.0');
    assert.strictEqual(result.package.entryPoint, 'index');
    assert.strictEqual(result.modules.length, 1);
    assert.strictEqual(result.modules[0].path, 'index');
  });

  it('processes a package with multiple files in subdirectories', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'multi-pkg',
      version: '2.0.0',
      main: './src/index.js',
    }));
    fs.mkdirSync(path.join(tmpDir, 'src'));
    fs.writeFileSync(path.join(tmpDir, 'src', 'index.js'), `import { helper } from './helper';\nexport default helper;`);
    fs.writeFileSync(path.join(tmpDir, 'src', 'helper.js'), `export function helper() { return 1; }`);

    const nodeModulesDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nodeModulesDir);
    const result = processPackage(tmpDir, nodeModulesDir);

    assert.strictEqual(result.package.name, 'multi-pkg');
    assert.strictEqual(result.package.entryPoint, 'src/index');
    assert.strictEqual(result.modules.length, 2);
    const paths = result.modules.map(m => m.path).sort();
    assert.deepStrictEqual(paths, ['src/helper', 'src/index']);
  });

  it('strips ./ prefix from entry point', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'prefix-pkg',
      version: '1.0.0',
      main: './lib/main.js',
    }));
    fs.mkdirSync(path.join(tmpDir, 'lib'));
    fs.writeFileSync(path.join(tmpDir, 'lib', 'main.js'), `export default 1;`);

    const nodeModulesDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nodeModulesDir);
    const result = processPackage(tmpDir, nodeModulesDir);

    assert.strictEqual(result.package.entryPoint, 'lib/main');
  });

  it('collects warnings from file processing', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'warn-pkg',
      version: '1.0.0',
      main: 'index.js',
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `const mod = require(dynamicVar);`);

    const nodeModulesDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nodeModulesDir);
    const result = processPackage(tmpDir, nodeModulesDir);

    assert.ok(result.warnings.length > 0);
  });

  it('includes dependencies from package.json', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'deps-pkg',
      version: '1.0.0',
      main: 'index.js',
      dependencies: { lodash: '^4.0.0' },
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `export default 1;`);

    const nodeModulesDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nodeModulesDir);
    const result = processPackage(tmpDir, nodeModulesDir);

    assert.deepStrictEqual(result.package.dependencies, { lodash: '^4.0.0' });
  });

  it('rewrites imports using package name for resolution', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'mypkg',
      version: '1.0.0',
      main: 'index.js',
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `import { foo } from './lib/foo';`);
    fs.mkdirSync(path.join(tmpDir, 'lib'));
    fs.writeFileSync(path.join(tmpDir, 'lib', 'foo.js'), `export const foo = 1;`);

    const nodeModulesDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nodeModulesDir);
    const result = processPackage(tmpDir, nodeModulesDir);

    const indexMod = result.modules.find(m => m.path === 'index');
    assert.ok(indexMod.source.includes("pljs.import('mypkg/lib/foo')"));
  });
});

describe('ensureNodeModules', () => {
  let tmpDir;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-installer-test-'));
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('returns node_modules path when it already exists', () => {
    const nmDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nmDir);
    const result = ensureNodeModules(tmpDir);
    assert.strictEqual(result, nmDir);
  });
});
