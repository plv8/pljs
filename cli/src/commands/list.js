import { connect } from '../db.js';

export function formatListOutput(data, json = false) {
  if (json) {
    return JSON.stringify(data, null, 2);
  }

  const lines = [];

  if (data.packages.length > 0) {
    lines.push('Packages:');
    lines.push('  Name                   Version      Entry Point');
    lines.push('  ' + '-'.repeat(55));
    for (const pkg of data.packages) {
      lines.push(`  ${pkg.name.padEnd(22)} ${pkg.version.padEnd(12)} ${pkg.entry_point}`);
    }
  } else {
    lines.push('No packages installed.');
  }

  lines.push('');

  if (data.modules.length > 0) {
    lines.push('Standalone Modules:');
    for (const mod of data.modules) {
      lines.push(`  ${mod.path}`);
    }
  } else {
    lines.push('No standalone modules installed.');
  }

  return lines.join('\n');
}

export async function list(args, flags) {
  const client = await connect(flags);

  try {
    const pkgResult = await client.query(
      'SELECT name, version, entry_point FROM pljs.packages ORDER BY name, version'
    );
    const modResult = await client.query(
      'SELECT path FROM pljs.modules WHERE package_name IS NULL ORDER BY path'
    );

    const data = {
      packages: pkgResult.rows,
      modules: modResult.rows,
    };

    console.log(formatListOutput(data, flags.json));
  } finally {
    await client.end();
  }
}
