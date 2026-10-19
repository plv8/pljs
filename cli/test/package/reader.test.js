import { describe, it, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { resolveEntryPoint, readPackageInfo, readPackageJson, collectPackageFiles } from '../../src/package/reader.js';

describe('resolveEntryPoint', () => {
  it('uses main field', () => {
    const ep = resolveEntryPoint({ main: 'lib/index.js' });
    assert.strictEqual(ep, 'lib/index.js');
  });

  it('defaults to index.js when no main', () => {
    const ep = resolveEntryPoint({});
    assert.strictEqual(ep, 'index.js');
  });

  it('prefers browser string field over main', () => {
    const ep = resolveEntryPoint({ main: 'lib/node.js', browser: 'lib/browser.js' });
    assert.strictEqual(ep, 'lib/browser.js');
  });

  it('uses exports.browser over main', () => {
    const ep = resolveEntryPoint({
      main: 'lib/node.js',
      exports: { '.': { browser: './lib/browser.js', default: './lib/node.js' } },
    });
    assert.strictEqual(ep, './lib/browser.js');
  });

  it('uses exports.default when no browser condition', () => {
    const ep = resolveEntryPoint({
      main: 'lib/node.js',
      exports: { '.': { default: './lib/default.js' } },
    });
    assert.strictEqual(ep, './lib/default.js');
  });

  it('handles exports as string shorthand', () => {
    const ep = resolveEntryPoint({ exports: './dist/index.js' });
    assert.strictEqual(ep, './dist/index.js');
  });

  it('handles browser field as object with main replacement', () => {
    const ep = resolveEntryPoint({
      main: 'lib/node.js',
      browser: { './lib/node.js': './lib/browser.js' },
    });
    assert.strictEqual(ep, './lib/browser.js');
  });
});

describe('readPackageInfo', () => {
  it('extracts name, version, dependencies', () => {
    const info = readPackageInfo({
      name: 'foo',
      version: '1.0.0',
      main: 'index.js',
      dependencies: { bar: '^1.0.0' },
    });
    assert.strictEqual(info.name, 'foo');
    assert.strictEqual(info.version, '1.0.0');
    assert.deepStrictEqual(info.dependencies, { bar: '^1.0.0' });
    assert.strictEqual(info.entryPoint, 'index.js');
  });

  it('defaults version to 0.0.0', () => {
    const info = readPackageInfo({ name: 'foo' });
    assert.strictEqual(info.version, '0.0.0');
  });

  it('defaults dependencies to empty object', () => {
    const info = readPackageInfo({ name: 'foo' });
    assert.deepStrictEqual(info.dependencies, {});
  });
});

describe('readPackageJson', () => {
  let tmpDir;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-reader-test-'));
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('reads and parses package.json from directory', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'test-pkg',
      version: '2.0.0',
      main: 'lib/index.js',
      dependencies: { lodash: '^4.0.0' },
    }));
    const info = readPackageJson(tmpDir);
    assert.strictEqual(info.name, 'test-pkg');
    assert.strictEqual(info.version, '2.0.0');
    assert.strictEqual(info.entryPoint, 'lib/index.js');
    assert.deepStrictEqual(info.dependencies, { lodash: '^4.0.0' });
  });

  it('throws when package.json does not exist', () => {
    assert.throws(() => readPackageJson(tmpDir), { code: 'ENOENT' });
  });
});

describe('collectPackageFiles', () => {
  let tmpDir;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-reader-test-'));
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('collects .js, .mjs, .cjs files recursively', () => {
    fs.writeFileSync(path.join(tmpDir, 'index.js'), 'export default 1;');
    fs.writeFileSync(path.join(tmpDir, 'util.mjs'), 'export const a = 1;');
    fs.writeFileSync(path.join(tmpDir, 'legacy.cjs'), 'module.exports = 1;');
    fs.mkdirSync(path.join(tmpDir, 'lib'));
    fs.writeFileSync(path.join(tmpDir, 'lib', 'helper.js'), 'export const h = 1;');

    const files = collectPackageFiles(tmpDir);
    const relPaths = files.map(f => f.relPath).sort();
    assert.deepStrictEqual(relPaths, ['index.js', 'legacy.cjs', 'lib/helper.js', 'util.mjs']);
  });

  it('skips node_modules directory', () => {
    fs.writeFileSync(path.join(tmpDir, 'index.js'), 'export default 1;');
    fs.mkdirSync(path.join(tmpDir, 'node_modules'));
    fs.mkdirSync(path.join(tmpDir, 'node_modules', 'dep'));
    fs.writeFileSync(path.join(tmpDir, 'node_modules', 'dep', 'index.js'), 'x');

    const files = collectPackageFiles(tmpDir);
    assert.strictEqual(files.length, 1);
    assert.strictEqual(files[0].relPath, 'index.js');
  });

  it('skips dotfiles and dotdirs', () => {
    fs.writeFileSync(path.join(tmpDir, 'index.js'), 'export default 1;');
    fs.writeFileSync(path.join(tmpDir, '.hidden.js'), 'secret');
    fs.mkdirSync(path.join(tmpDir, '.git'));
    fs.writeFileSync(path.join(tmpDir, '.git', 'config.js'), 'x');

    const files = collectPackageFiles(tmpDir);
    assert.strictEqual(files.length, 1);
    assert.strictEqual(files[0].relPath, 'index.js');
  });

  it('skips non-JS files', () => {
    fs.writeFileSync(path.join(tmpDir, 'index.js'), 'export default 1;');
    fs.writeFileSync(path.join(tmpDir, 'README.md'), '# readme');
    fs.writeFileSync(path.join(tmpDir, 'data.json'), '{}');

    const files = collectPackageFiles(tmpDir);
    assert.strictEqual(files.length, 1);
  });

  it('returns empty array for empty directory', () => {
    const files = collectPackageFiles(tmpDir);
    assert.deepStrictEqual(files, []);
  });
});
