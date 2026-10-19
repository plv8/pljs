import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { connect, withTransaction } from '../db.js';
import { processStandaloneSource, processPackage, npmInstall, ensureNodeModules } from '../package/installer.js';

export function detectInstallMode(target) {
  if (/\.(js|mjs|cjs)$/.test(target) && fs.existsSync(target)) {
    return { type: 'file', path: target };
  }

  if (fs.existsSync(target) && fs.statSync(target).isDirectory()) {
    const hasPkgJson = fs.existsSync(path.join(target, 'package.json'));
    const hasNodeModules = fs.existsSync(path.join(target, 'node_modules'));

    if (hasPkgJson && hasNodeModules) {
      return { type: 'directory-ready', path: target };
    }
    if (hasPkgJson) {
      return { type: 'directory-needs-install', path: target };
    }
  }

  return { type: 'npm-package', spec: target };
}

// The sloppy column only exists in the INSERT when it is asked for, so that
// installing without --sloppy still works against a pljs that has no such column.
function moduleInsert(columns, params, flags) {
  if (flags.sloppy) {
    columns = [...columns, 'sloppy'];
    params = [...params, true];
  }

  const placeholders = params.map((_, i) => `$${i + 1}`).join(', ');

  return {
    text: `INSERT INTO pljs.modules (${columns.join(', ')}) VALUES (${placeholders})`,
    params,
  };
}

async function installFile(target, flags, client) {
  const source = fs.readFileSync(target, 'utf-8');
  const moduleName = flags.as || path.basename(target).replace(/\.(js|mjs|cjs)$/, '');
  const result = processStandaloneSource(source);

  for (const w of result.warnings) {
    console.warn(`Warning: ${w}`);
  }

  await withTransaction(client, async (c) => {
    if (flags.force) {
      await c.query('DELETE FROM pljs.modules WHERE path = $1 AND package_name IS NULL', [moduleName]);
    }
    const insert = moduleInsert(['path', 'source'], [moduleName, result.source], flags);
    await c.query(insert.text, insert.params);
  });

  return { type: 'module', name: moduleName };
}

async function installFromDir(packageDir, nodeModulesDir, flags, client) {
  const processed = processPackage(packageDir, nodeModulesDir);
  const results = [];

  for (const w of processed.warnings) {
    console.warn(`Warning: ${w}`);
  }

  await withTransaction(client, async (c) => {
    const pkg = processed.package;

    if (flags.force) {
      await c.query('DELETE FROM pljs.modules WHERE package_name = $1 AND package_version = $2', [pkg.name, pkg.version]);
      await c.query('DELETE FROM pljs.packages WHERE name = $1 AND version = $2', [pkg.name, pkg.version]);
    }

    await c.query(
      'INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES ($1, $2, $3, $4)',
      [pkg.name, pkg.version, pkg.entryPoint, JSON.stringify(pkg.dependencies)]
    );

    for (const mod of processed.modules) {
      const insert = moduleInsert(
        ['path', 'source', 'package_name', 'package_version'],
        [mod.path, mod.source, pkg.name, pkg.version],
        flags
      );
      await c.query(insert.text, insert.params);
    }

    results.push({ type: 'package', name: pkg.name, version: pkg.version, modules: processed.modules.length });
  });

  // Process transitive dependencies best-effort.
  if (processed.package.dependencies) {
    for (const depName of Object.keys(processed.package.dependencies)) {
      const depDir = path.join(nodeModulesDir, depName);
      if (fs.existsSync(path.join(depDir, 'package.json'))) {
        try {
          const depResults = await installFromDir(depDir, nodeModulesDir, { ...flags, force: flags.force }, client);
          results.push(...(Array.isArray(depResults) ? depResults : [depResults]));
        } catch (err) {
          console.warn(`Warning: Failed to install dependency '${depName}': ${err.message}`);
        }
      }
    }
  }

  return results;
}

async function installDirectory(dir, flags, client) {
  const nodeModulesDir = ensureNodeModules(dir);
  return installFromDir(dir, nodeModulesDir, flags, client);
}

async function installNpmPackage(spec, flags, client) {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pljs-install-'));
  try {
    fs.writeFileSync(path.join(tmpDir, 'package.json'), '{}');
    npmInstall(tmpDir, spec);
    const nodeModulesDir = path.join(tmpDir, 'node_modules');
    const pkgName = spec.replace(/@[^/]*$/, '').replace(/^@/, '') || spec;
    // Handle scoped packages
    const pkgDir = fs.existsSync(path.join(nodeModulesDir, spec.split('@')[0]))
      ? path.join(nodeModulesDir, spec.split('@')[0])
      : path.join(nodeModulesDir, spec.replace(/@.*$/, ''));
    return installFromDir(pkgDir, nodeModulesDir, flags, client);
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

export async function install(args, flags) {
  if (args.length === 0) {
    console.error('Usage: pljs install <target>');
    process.exit(1);
  }

  const target = args[0];
  const mode = detectInstallMode(target);
  const client = await connect(flags);

  try {
    let result;

    switch (mode.type) {
      case 'file':
        result = await installFile(mode.path, flags, client);
        break;
      case 'directory-ready':
      case 'directory-needs-install':
        result = await installDirectory(mode.path, flags, client);
        break;
      case 'npm-package':
        result = await installNpmPackage(mode.spec, flags, client);
        break;
    }

    if (flags.json) {
      console.log(JSON.stringify(result, null, 2));
    } else {
      if (Array.isArray(result)) {
        for (const r of result) {
          console.log(`Installed ${r.type} '${r.name}'${r.version ? `@${r.version}` : ''}`);
        }
      } else {
        console.log(`Installed ${result.type} '${result.name}'`);
      }
    }
  } finally {
    await client.end();
  }
}
