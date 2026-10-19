import { describe, it, mock, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

function createMockClient() {
  const queries = [];
  return {
    queries,
    query: async (text, params) => {
      queries.push({ text, params });
      return { rows: [], rowCount: 0 };
    },
    end: async () => {},
  };
}

let mockClientInstance;

mock.module('../../src/db.js', {
  exports: {
    connect: async () => mockClientInstance,
    buildConnectionConfig: () => ({}),
    withTransaction: async (client, fn) => {
      await client.query('BEGIN');
      try {
        const result = await fn(client);
        await client.query('COMMIT');
        return result;
      } catch (err) {
        await client.query('ROLLBACK');
        throw err;
      }
    },
  },
});

const { detectInstallMode, install } = await import('../../src/commands/install.js');

describe('detectInstallMode', () => {
  let tmpDir;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-test-'));
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('detects single JS file', () => {
    const file = path.join(tmpDir, 'mod.js');
    fs.writeFileSync(file, 'module.exports = 1;');
    const mode = detectInstallMode(file);
    assert.strictEqual(mode.type, 'file');
  });

  it('detects .mjs file', () => {
    const file = path.join(tmpDir, 'mod.mjs');
    fs.writeFileSync(file, 'export default 1;');
    const mode = detectInstallMode(file);
    assert.strictEqual(mode.type, 'file');
  });

  it('detects directory with package.json and node_modules', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), '{"name":"test"}');
    fs.mkdirSync(path.join(tmpDir, 'node_modules'));
    const mode = detectInstallMode(tmpDir);
    assert.strictEqual(mode.type, 'directory-ready');
  });

  it('detects directory with package.json but no node_modules', () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), '{"name":"test"}');
    const mode = detectInstallMode(tmpDir);
    assert.strictEqual(mode.type, 'directory-needs-install');
  });

  it('detects bare package name', () => {
    const mode = detectInstallMode('lodash');
    assert.strictEqual(mode.type, 'npm-package');
    assert.strictEqual(mode.spec, 'lodash');
  });

  it('detects package name with version', () => {
    const mode = detectInstallMode('lodash@4.17.21');
    assert.strictEqual(mode.type, 'npm-package');
    assert.strictEqual(mode.spec, 'lodash@4.17.21');
  });
});

describe('install - file mode', () => {
  let tmpDir;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-test-'));
    mockClientInstance = createMockClient();
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('installs a single JS file', async () => {
    const file = path.join(tmpDir, 'mymod.js');
    fs.writeFileSync(file, `const x = require('foo');\nmodule.exports = x;`);
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await install([file], {});
    } finally {
      console.log = origLog;
    }
    assert.ok(logs.some(l => l.includes("Installed module 'mymod'")));
    // Check that the INSERT was called
    const insertQuery = mockClientInstance.queries.find(q => q.text.includes('INSERT INTO pljs.modules'));
    assert.ok(insertQuery);
    assert.strictEqual(insertQuery.params[0], 'mymod');
  });

  it('stores modules as sloppy only when asked', async () => {
    const file = path.join(tmpDir, 'mymod.js');
    fs.writeFileSync(file, `implicit = 1;\nmodule.exports = implicit;`);
    const origLog = console.log;
    console.log = () => {};
    try {
      await install([file], {});
      const plain = mockClientInstance.queries.find(q => q.text.includes('INSERT INTO pljs.modules'));
      assert.ok(!plain.text.includes('sloppy'));
      assert.strictEqual(plain.params.length, 2);

      mockClientInstance.queries.length = 0;
      await install([file], { sloppy: true });
      const sloppy = mockClientInstance.queries.find(q => q.text.includes('INSERT INTO pljs.modules'));
      assert.strictEqual(sloppy.text, 'INSERT INTO pljs.modules (path, source, sloppy) VALUES ($1, $2, $3)');
      assert.strictEqual(sloppy.params[0], 'mymod');
      assert.strictEqual(sloppy.params[2], true);
    } finally {
      console.log = origLog;
    }
  });

  it('stores every module of a package as sloppy with --sloppy', async () => {
    const pkgDir = path.join(tmpDir, 'pkg');
    fs.mkdirSync(path.join(pkgDir, 'node_modules'), { recursive: true });
    fs.writeFileSync(path.join(pkgDir, 'package.json'), JSON.stringify({ name: 'sloppypkg', version: '1.0.0', main: 'index.js' }));
    fs.writeFileSync(path.join(pkgDir, 'index.js'), `module.exports = require('./other');`);
    fs.writeFileSync(path.join(pkgDir, 'other.js'), `implicit = 2;\nmodule.exports = implicit;`);
    const origLog = console.log;
    console.log = () => {};
    try {
      await install([pkgDir], { sloppy: true });
    } finally {
      console.log = origLog;
    }
    const inserts = mockClientInstance.queries.filter(q => q.text.includes('INSERT INTO pljs.modules'));
    assert.ok(inserts.length >= 2);
    for (const q of inserts) {
      assert.ok(q.text.includes('sloppy'));
      assert.strictEqual(q.params[q.params.length - 1], true);
    }
  });

  it('uses --as flag to override module name', async () => {
    const file = path.join(tmpDir, 'mymod.js');
    fs.writeFileSync(file, `export default 1;`);
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await install([file], { as: 'custom_name' });
    } finally {
      console.log = origLog;
    }
    assert.ok(logs.some(l => l.includes("Installed module 'custom_name'")));
  });

  it('uses --force to delete before inserting', async () => {
    const file = path.join(tmpDir, 'mymod.js');
    fs.writeFileSync(file, `export default 1;`);
    const origLog = console.log;
    console.log = () => {};
    try {
      await install([file], { force: true });
    } finally {
      console.log = origLog;
    }
    const deleteQuery = mockClientInstance.queries.find(q => q.text.includes('DELETE'));
    assert.ok(deleteQuery);
  });

  it('outputs JSON when flag is set', async () => {
    const file = path.join(tmpDir, 'mymod.js');
    fs.writeFileSync(file, `export default 1;`);
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await install([file], { json: true });
    } finally {
      console.log = origLog;
    }
    const parsed = JSON.parse(logs.join(''));
    assert.strictEqual(parsed.type, 'module');
    assert.strictEqual(parsed.name, 'mymod');
  });

  it('prints warnings from processing', async () => {
    const file = path.join(tmpDir, 'mymod.js');
    fs.writeFileSync(file, `const x = require(varName);`);
    const warns = [];
    const origWarn = console.warn;
    const origLog = console.log;
    console.warn = (...args) => warns.push(args.join(' '));
    console.log = () => {};
    try {
      await install([file], {});
    } finally {
      console.warn = origWarn;
      console.log = origLog;
    }
    assert.ok(warns.some(w => w.includes('Warning:')));
  });

  it('calls client.end in finally block', async () => {
    const file = path.join(tmpDir, 'mymod.js');
    fs.writeFileSync(file, `export default 1;`);
    let endCalled = false;
    mockClientInstance.end = async () => { endCalled = true; };
    const origLog = console.log;
    console.log = () => {};
    try {
      await install([file], {});
    } finally {
      console.log = origLog;
    }
    assert.ok(endCalled);
  });
});

describe('install - directory mode', () => {
  let tmpDir;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-test-'));
    mockClientInstance = createMockClient();
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('installs from a directory with package.json and node_modules', async () => {
    // Create a minimal package
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'test-pkg',
      version: '1.0.0',
      main: 'index.js',
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `export default function() { return 1; }`);
    fs.mkdirSync(path.join(tmpDir, 'node_modules'));

    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await install([tmpDir], {});
    } finally {
      console.log = origLog;
    }
    assert.ok(logs.some(l => l.includes("Installed package 'test-pkg'")));
  });

  it('installs from directory with --force flag', async () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'test-pkg',
      version: '1.0.0',
      main: 'index.js',
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `export default 1;`);
    fs.mkdirSync(path.join(tmpDir, 'node_modules'));

    const origLog = console.log;
    console.log = () => {};
    try {
      await install([tmpDir], { force: true });
    } finally {
      console.log = origLog;
    }
    const deleteQueries = mockClientInstance.queries.filter(q => q.text.includes('DELETE'));
    assert.ok(deleteQueries.length >= 2); // modules + packages
  });

  it('outputs JSON for directory install', async () => {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'test-pkg',
      version: '1.0.0',
      main: 'index.js',
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `export default 1;`);
    fs.mkdirSync(path.join(tmpDir, 'node_modules'));

    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await install([tmpDir], { json: true });
    } finally {
      console.log = origLog;
    }
    const parsed = JSON.parse(logs.join(''));
    assert.ok(Array.isArray(parsed));
    assert.strictEqual(parsed[0].name, 'test-pkg');
  });

  it('processes transitive dependencies', async () => {
    // Create main package
    fs.writeFileSync(path.join(tmpDir, 'package.json'), JSON.stringify({
      name: 'main-pkg',
      version: '1.0.0',
      main: 'index.js',
      dependencies: { 'dep-pkg': '^1.0.0' },
    }));
    fs.writeFileSync(path.join(tmpDir, 'index.js'), `export default 1;`);

    // Create node_modules with dependency
    const nmDir = path.join(tmpDir, 'node_modules');
    fs.mkdirSync(nmDir);
    const depDir = path.join(nmDir, 'dep-pkg');
    fs.mkdirSync(depDir);
    fs.writeFileSync(path.join(depDir, 'package.json'), JSON.stringify({
      name: 'dep-pkg',
      version: '1.0.0',
      main: 'index.js',
    }));
    fs.writeFileSync(path.join(depDir, 'index.js'), `export default 2;`);

    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await install([tmpDir], {});
    } finally {
      console.log = origLog;
    }
    // Should install both main-pkg and dep-pkg
    assert.ok(logs.some(l => l.includes("main-pkg")));
    assert.ok(logs.some(l => l.includes("dep-pkg")));
  });
});
