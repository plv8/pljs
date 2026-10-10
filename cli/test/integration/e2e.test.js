import { describe, it, before, after, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import pg from 'pg';

const DB_CONFIG = { database: 'contrib_regression' };

/** Helper: get a connected pg.Client */
async function getClient() {
  const client = new pg.Client(DB_CONFIG);
  await client.connect();
  return client;
}

/** Helper: clean all test data from the database */
async function cleanTestData(client) {
  // Drop any test functions we might have created
  const fnResult = await client.query(`
    SELECT p.proname
    FROM pg_proc p
    JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'public' AND p.proname LIKE 'e2e_test_%'
  `);
  for (const row of fnResult.rows) {
    await client.query(`DROP FUNCTION IF EXISTS ${row.proname}() CASCADE`);
  }

  // Clean modules first (FK constraint), then packages
  await client.query(`DELETE FROM pljs.modules WHERE path LIKE 'e2e_%' OR package_name LIKE 'e2etest%'`);
  await client.query(`DELETE FROM pljs.packages WHERE name LIKE 'e2etest%'`);
}

/** Helper: create a temp directory with files */
function createTempDir(files) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-e2e-'));
  for (const [relPath, content] of Object.entries(files)) {
    const fullPath = path.join(dir, relPath);
    fs.mkdirSync(path.dirname(fullPath), { recursive: true });
    fs.writeFileSync(fullPath, content);
  }
  return dir;
}

/** Helper: install a standalone file via the CLI's install logic */
async function installFile(client, moduleName, source, force = false) {
  const { processStandaloneSource } = await import('../../src/package/installer.js');
  const { withTransaction } = await import('../../src/db.js');
  const result = processStandaloneSource(source);
  await withTransaction(client, async (c) => {
    if (force) {
      await c.query('DELETE FROM pljs.modules WHERE path = $1 AND package_name IS NULL', [moduleName]);
    }
    await c.query('INSERT INTO pljs.modules (path, source) VALUES ($1, $2)', [moduleName, result.source]);
  });
  return result;
}

/** Helper: install a package directory via the CLI's install logic */
async function installPackageDir(client, packageDir, force = false) {
  const { processPackage } = await import('../../src/package/installer.js');
  const { withTransaction } = await import('../../src/db.js');

  const nodeModulesDir = path.join(packageDir, 'node_modules');
  if (!fs.existsSync(nodeModulesDir)) {
    fs.mkdirSync(nodeModulesDir);
  }

  const processed = processPackage(packageDir, nodeModulesDir);

  await withTransaction(client, async (c) => {
    const pkg = processed.package;
    if (force) {
      await c.query('DELETE FROM pljs.modules WHERE package_name = $1 AND package_version = $2', [pkg.name, pkg.version]);
      await c.query('DELETE FROM pljs.packages WHERE name = $1 AND version = $2', [pkg.name, pkg.version]);
    }
    await c.query(
      'INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES ($1, $2, $3, $4)',
      [pkg.name, pkg.version, pkg.entryPoint, JSON.stringify(pkg.dependencies)]
    );
    for (const mod of processed.modules) {
      await c.query(
        'INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES ($1, $2, $3, $4)',
        [mod.path, mod.source, pkg.name, pkg.version]
      );
    }
  });

  return processed;
}

/** Helper: remove a module or package */
async function removeTarget(client, name, version = null) {
  const { withTransaction } = await import('../../src/db.js');
  await withTransaction(client, async (c) => {
    if (version) {
      await c.query('DELETE FROM pljs.modules WHERE package_name = $1 AND package_version = $2', [name, version]);
      await c.query('DELETE FROM pljs.packages WHERE name = $1 AND version = $2', [name, version]);
    } else {
      await c.query('DELETE FROM pljs.modules WHERE package_name = $1', [name]);
      const pkgResult = await c.query('DELETE FROM pljs.packages WHERE name = $1', [name]);
      if (pkgResult.rowCount === 0) {
        await c.query('DELETE FROM pljs.modules WHERE path = $1 AND package_name IS NULL', [name]);
      }
    }
  });
}

/** Helper: create a pljs function and call it, returning the result */
async function callPljs(client, fnName, body, returnType = 'text') {
  await client.query(`CREATE OR REPLACE FUNCTION ${fnName}() RETURNS ${returnType} AS $pljs$ ${body} $pljs$ LANGUAGE pljs`);
  const result = await client.query(`SELECT ${fnName}()`);
  return result.rows[0][fnName];
}

describe('pljs CLI e2e integration tests', () => {
  let client;
  const tempDirs = [];

  before(async () => {
    client = await getClient();
    await cleanTestData(client);
  });

  afterEach(async () => {
    await cleanTestData(client);
    for (const dir of tempDirs) {
      fs.rmSync(dir, { recursive: true, force: true });
    }
    tempDirs.length = 0;
    // Reconnect to get a fresh pljs JSContext (pljs caches modules per connection)
    await client.end();
    client = await getClient();
  });

  after(async () => {
    await client.end();
  });

  describe('1. Standalone CommonJS module', () => {
    it('installs and executes a CommonJS module via pljs.require()', async () => {
      const source = `
        module.exports = {
          greet: function(name) { return 'Hello, ' + name + '!'; }
        };
      `;
      await installFile(client, 'e2e_cjs_hello', source);

      const result = await callPljs(client, 'e2e_test_cjs',
        "var mod = pljs.require('e2e_cjs_hello'); return mod.greet('world');");

      assert.strictEqual(result, 'Hello, world!');
    });
  });

  describe('2. Standalone ES6 module', () => {
    it('installs and executes an ES6 module via pljs.import()', async () => {
      const source = `
        export function add(a, b) { return a + b; }
        export function multiply(a, b) { return a * b; }
      `;
      await installFile(client, 'e2e_es6_math', source);

      const result = await callPljs(client, 'e2e_test_es6',
        "var mod = pljs.import('e2e_es6_math'); return String(mod.add(10, 20));");

      assert.strictEqual(result, '30');
    });
  });

  describe('3. CommonJS module requiring another CommonJS module', () => {
    it('chains require calls between standalone modules', async () => {
      // Module A: base utility
      const sourceA = `
        module.exports = {
          double: function(n) { return n * 2; }
        };
      `;
      await installFile(client, 'e2e_cjs_a', sourceA);

      // Module B: requires A. The rewriter transforms require() -> pljs.require()
      const sourceB = `
        var a = require('e2e_cjs_a');
        module.exports = {
          quadruple: function(n) { return a.double(a.double(n)); }
        };
      `;
      await installFile(client, 'e2e_cjs_b', sourceB);

      // Verify the rewriter changed require -> pljs.require
      const modRow = await client.query(
        "SELECT source FROM pljs.modules WHERE path = 'e2e_cjs_b' AND package_name IS NULL"
      );
      assert.ok(modRow.rows[0].source.includes('pljs.require'));

      const result = await callPljs(client, 'e2e_test_chain_cjs',
        "var b = pljs.require('e2e_cjs_b'); return String(b.quadruple(5));");

      assert.strictEqual(result, '20');
    });
  });

  describe('4. ES6 module importing another ES6 module', () => {
    it('chains import calls between standalone modules', async () => {
      // Module C: base ES6 module
      const sourceC = `
        export function triple(n) { return n * 3; }
      `;
      await installFile(client, 'e2e_es6_c', sourceC);

      // Module D: imports from C. The rewriter transforms import to pljs.import()
      const sourceD = `
        import { triple } from 'e2e_es6_c';
        export function nineX(n) { return triple(triple(n)); }
      `;
      await installFile(client, 'e2e_es6_d', sourceD);

      // Verify the rewriter changed the import
      const modRow = await client.query(
        "SELECT source FROM pljs.modules WHERE path = 'e2e_es6_d' AND package_name IS NULL"
      );
      assert.ok(modRow.rows[0].source.includes('pljs.import'));
      assert.ok(!modRow.rows[0].source.includes('import {'));

      const result = await callPljs(client, 'e2e_test_chain_es6',
        "var d = pljs.import('e2e_es6_d'); return String(d.nineX(2));");

      assert.strictEqual(result, '18');
    });
  });

  describe('5. Package with entry point (CommonJS)', () => {
    it('installs a package directory and loads via pljs.require()', async () => {
      const dir = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestcjspkg',
          version: '1.0.0',
          main: 'index.js',
        }),
        'index.js': `
          module.exports = {
            name: function() { return 'e2etestcjspkg'; },
            version: function() { return '1.0.0'; }
          };
        `,
      });
      tempDirs.push(dir);

      await installPackageDir(client, dir);

      // Verify the package appears in pljs.packages
      const pkgRow = await client.query(
        "SELECT * FROM pljs.packages WHERE name = 'e2etestcjspkg'"
      );
      assert.strictEqual(pkgRow.rows.length, 1);
      assert.strictEqual(pkgRow.rows[0].name, 'e2etestcjspkg');
      assert.strictEqual(pkgRow.rows[0].version, '1.0.0');
      assert.strictEqual(pkgRow.rows[0].entry_point, 'index');

      const result = await callPljs(client, 'e2e_test_cjs_pkg',
        "var pkg = pljs.require('e2etestcjspkg'); return pkg.name();");

      assert.strictEqual(result, 'e2etestcjspkg');
    });
  });

  describe('6. Package with multiple modules (CommonJS)', () => {
    it('installs a package with internal requires rewritten', async () => {
      const dir = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestcjsmulti',
          version: '2.0.0',
          main: 'index.js',
        }),
        'index.js': `
          var helper = require('./lib/helper');
          module.exports = {
            greet: function(name) { return helper.format('Hello', name); }
          };
        `,
        'lib/helper.js': `
          module.exports = {
            format: function(greeting, name) { return greeting + ', ' + name + '!'; }
          };
        `,
      });
      tempDirs.push(dir);

      await installPackageDir(client, dir);

      // Verify the require('./lib/helper') was rewritten to pljs.require('e2etestcjsmulti/lib/helper')
      const modRow = await client.query(
        "SELECT source FROM pljs.modules WHERE path = 'index' AND package_name = 'e2etestcjsmulti'"
      );
      assert.ok(modRow.rows[0].source.includes("pljs.require('e2etestcjsmulti/lib/helper')"));

      // Verify both modules exist
      const allMods = await client.query(
        "SELECT path FROM pljs.modules WHERE package_name = 'e2etestcjsmulti' ORDER BY path"
      );
      assert.strictEqual(allMods.rows.length, 2);

      const result = await callPljs(client, 'e2e_test_cjs_multi',
        "var pkg = pljs.require('e2etestcjsmulti'); return pkg.greet('Alice');");

      assert.strictEqual(result, 'Hello, Alice!');
    });
  });

  describe('7. Package with entry point (ES6)', () => {
    it('installs an ES6 package and loads via pljs.import()', async () => {
      const dir = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestes6pkg',
          version: '1.0.0',
          main: 'index.mjs',
        }),
        'index.mjs': `
          export function sayHello() { return 'hello from es6 package'; }
          export const VERSION = '1.0.0';
        `,
      });
      tempDirs.push(dir);

      await installPackageDir(client, dir);

      const pkgRow = await client.query(
        "SELECT * FROM pljs.packages WHERE name = 'e2etestes6pkg'"
      );
      assert.strictEqual(pkgRow.rows.length, 1);
      assert.strictEqual(pkgRow.rows[0].entry_point, 'index');

      const result = await callPljs(client, 'e2e_test_es6_pkg',
        "var pkg = pljs.import('e2etestes6pkg'); return pkg.sayHello();");

      assert.strictEqual(result, 'hello from es6 package');
    });
  });

  describe('8. Package with multiple modules (ES6)', () => {
    it('installs an ES6 package with internal imports rewritten', async () => {
      const dir = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestes6multi',
          version: '1.0.0',
          main: 'index.mjs',
        }),
        'index.mjs': `
          import { compute } from './math.mjs';
          export function result() { return String(compute(7)); }
        `,
        'math.mjs': `
          export function compute(n) { return n * n; }
        `,
      });
      tempDirs.push(dir);

      await installPackageDir(client, dir);

      // Verify import rewriting
      const modRow = await client.query(
        "SELECT source FROM pljs.modules WHERE path = 'index' AND package_name = 'e2etestes6multi'"
      );
      assert.ok(modRow.rows[0].source.includes("pljs.import('e2etestes6multi/math')"));
      assert.ok(!modRow.rows[0].source.includes("from './math.mjs'"));

      const result = await callPljs(client, 'e2e_test_es6_multi',
        "var pkg = pljs.import('e2etestes6multi'); return pkg.result();");

      assert.strictEqual(result, '49');
    });
  });

  describe('9. Package that depends on another package', () => {
    it('resolves cross-package dependencies at runtime', async () => {
      // Install PackageA first
      const dirA = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestdepa',
          version: '1.0.0',
          main: 'index.js',
        }),
        'index.js': `
          module.exports = {
            baseValue: function() { return 42; }
          };
        `,
      });
      tempDirs.push(dirA);
      await installPackageDir(client, dirA);

      // Install PackageB that depends on PackageA
      const dirB = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestdepb',
          version: '1.0.0',
          main: 'index.js',
          dependencies: { 'e2etestdepa': '^1.0.0' },
        }),
        'index.js': `
          var a = require('e2etestdepa');
          module.exports = {
            derivedValue: function() { return a.baseValue() + 8; }
          };
        `,
      });
      tempDirs.push(dirB);
      await installPackageDir(client, dirB);

      // Verify dependency is recorded
      const pkgRow = await client.query(
        "SELECT dependencies FROM pljs.packages WHERE name = 'e2etestdepb'"
      );
      const deps = pkgRow.rows[0].dependencies;
      assert.ok(deps['e2etestdepa']);

      const result = await callPljs(client, 'e2e_test_dep_chain',
        "var b = pljs.require('e2etestdepb'); return String(b.derivedValue());");

      assert.strictEqual(result, '50');
    });
  });

  describe('10. --force overwrite', () => {
    it('overwrites an existing standalone module when force is used', async () => {
      // Install v1
      const sourceV1 = `
        module.exports = { ver: function() { return 'v1'; } };
      `;
      await installFile(client, 'e2e_force_mod', sourceV1);

      const resultV1 = await callPljs(client, 'e2e_test_force',
        "var m = pljs.require('e2e_force_mod'); return m.ver();");
      assert.strictEqual(resultV1, 'v1');

      // Drop the function so pljs picks up the updated module
      await client.query('DROP FUNCTION IF EXISTS e2e_test_force()');

      // Install v2 with force
      const sourceV2 = `
        module.exports = { ver: function() { return 'v2'; } };
      `;
      await installFile(client, 'e2e_force_mod', sourceV2, true);

      // Reconnect to clear the pljs module cache
      await client.end();
      client = await getClient();

      const resultV2 = await callPljs(client, 'e2e_test_force',
        "var m = pljs.require('e2e_force_mod'); return m.ver();");
      assert.strictEqual(resultV2, 'v2');
    });

    it('overwrites an existing package when force is used', async () => {
      // Install v1
      const dirV1 = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestforcepkg',
          version: '1.0.0',
          main: 'index.js',
        }),
        'index.js': `module.exports = { v: function() { return 'v1'; } };`,
      });
      tempDirs.push(dirV1);
      await installPackageDir(client, dirV1);

      const r1 = await callPljs(client, 'e2e_test_force_pkg',
        "return pljs.require('e2etestforcepkg').v();");
      assert.strictEqual(r1, 'v1');

      await client.query('DROP FUNCTION IF EXISTS e2e_test_force_pkg()');

      // Install v1 again with updated source and force
      const dirV2 = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestforcepkg',
          version: '1.0.0',
          main: 'index.js',
        }),
        'index.js': `module.exports = { v: function() { return 'v2'; } };`,
      });
      tempDirs.push(dirV2);
      await installPackageDir(client, dirV2, true);

      // Reconnect to clear the pljs module cache
      await client.end();
      client = await getClient();

      const r2 = await callPljs(client, 'e2e_test_force_pkg',
        "return pljs.require('e2etestforcepkg').v();");
      assert.strictEqual(r2, 'v2');
    });
  });

  describe('11. Remove', () => {
    it('removes a standalone module and it becomes unavailable', async () => {
      const source = `module.exports = { x: function() { return 1; } };`;
      await installFile(client, 'e2e_remove_mod', source);

      // Verify it exists
      const result = await callPljs(client, 'e2e_test_remove',
        "return String(pljs.require('e2e_remove_mod').x());");
      assert.strictEqual(result, '1');

      await client.query('DROP FUNCTION IF EXISTS e2e_test_remove()');

      // Remove it
      await removeTarget(client, 'e2e_remove_mod');

      // Verify it is gone from the modules table
      const modRow = await client.query(
        "SELECT * FROM pljs.modules WHERE path = 'e2e_remove_mod' AND package_name IS NULL"
      );
      assert.strictEqual(modRow.rows.length, 0);

      // Reconnect to clear pljs cache, then verify the module is gone
      await client.end();
      client = await getClient();

      await assert.rejects(
        callPljs(client, 'e2e_test_remove',
          "return String(pljs.require('e2e_remove_mod').x());"),
        (err) => {
          assert.ok(
            err.message.includes('could not load module') || err.detail?.includes('could not load module'),
            `Expected "could not load module" in error, got: ${err.message} / ${err.detail}`
          );
          return true;
        }
      );
    });

    it('removes a package and all its modules', async () => {
      const dir = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestremovepkg',
          version: '1.0.0',
          main: 'index.js',
        }),
        'index.js': `module.exports = { y: function() { return 2; } };`,
      });
      tempDirs.push(dir);
      await installPackageDir(client, dir);

      // Verify it exists
      const r = await callPljs(client, 'e2e_test_remove_pkg',
        "return String(pljs.require('e2etestremovepkg').y());");
      assert.strictEqual(r, '2');

      await client.query('DROP FUNCTION IF EXISTS e2e_test_remove_pkg()');

      // Remove it
      await removeTarget(client, 'e2etestremovepkg');

      // Verify gone from DB
      const pkgRow = await client.query("SELECT * FROM pljs.packages WHERE name = 'e2etestremovepkg'");
      assert.strictEqual(pkgRow.rows.length, 0);

      const modRow = await client.query("SELECT * FROM pljs.modules WHERE package_name = 'e2etestremovepkg'");
      assert.strictEqual(modRow.rows.length, 0);

      // Reconnect to clear pljs cache, then verify the package is gone
      await client.end();
      client = await getClient();

      await assert.rejects(
        callPljs(client, 'e2e_test_remove_pkg',
          "return String(pljs.require('e2etestremovepkg').y());"),
        (err) => {
          assert.ok(
            err.message.includes('could not load module') || err.detail?.includes('could not load module'),
            `Expected "could not load module" in error, got: ${err.message} / ${err.detail}`
          );
          return true;
        }
      );
    });
  });

  describe('12. List', () => {
    it('lists installed standalone modules and packages', async () => {
      // Install a standalone module
      await installFile(client, 'e2e_list_mod', `module.exports = {};`);

      // Install a package
      const dir = createTempDir({
        'package.json': JSON.stringify({
          name: 'e2etestlistpkg',
          version: '3.0.0',
          main: 'index.js',
        }),
        'index.js': `module.exports = {};`,
      });
      tempDirs.push(dir);
      await installPackageDir(client, dir);

      // Query the tables directly (same as what the list command does)
      const pkgResult = await client.query(
        "SELECT name, version, entry_point FROM pljs.packages WHERE name LIKE 'e2etest%' ORDER BY name"
      );
      const modResult = await client.query(
        "SELECT path FROM pljs.modules WHERE path LIKE 'e2e_%' AND package_name IS NULL ORDER BY path"
      );

      // Verify the standalone module appears
      const modPaths = modResult.rows.map(r => r.path);
      assert.ok(modPaths.includes('e2e_list_mod'), `Expected 'e2e_list_mod' in ${JSON.stringify(modPaths)}`);

      // Verify the package appears
      const pkgNames = pkgResult.rows.map(r => r.name);
      assert.ok(pkgNames.includes('e2etestlistpkg'), `Expected 'e2etestlistpkg' in ${JSON.stringify(pkgNames)}`);

      // Also test formatListOutput
      const { formatListOutput } = await import('../../src/commands/list.js');
      const data = {
        packages: pkgResult.rows,
        modules: modResult.rows,
      };
      const output = formatListOutput(data);
      assert.ok(output.includes('e2etestlistpkg'));
      assert.ok(output.includes('3.0.0'));
      assert.ok(output.includes('e2e_list_mod'));

      // Test JSON format
      const jsonOutput = formatListOutput(data, true);
      const parsed = JSON.parse(jsonOutput);
      assert.ok(parsed.packages.some(p => p.name === 'e2etestlistpkg'));
      assert.ok(parsed.modules.some(m => m.path === 'e2e_list_mod'));
    });
  });
});
