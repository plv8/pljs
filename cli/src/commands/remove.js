import { connect, withTransaction } from '../db.js';

export function parseRemoveTarget(target) {
  if (target.startsWith('@')) {
    const withoutScope = target.slice(1);
    const parts = withoutScope.split('@');
    if (parts.length > 1) {
      return { name: `@${parts[0]}`, version: parts[1] };
    }
    return { name: target, version: null };
  }

  const atIndex = target.indexOf('@');
  if (atIndex > 0) {
    return { name: target.slice(0, atIndex), version: target.slice(atIndex + 1) };
  }
  return { name: target, version: null };
}

export async function remove(args, flags) {
  if (args.length === 0) {
    console.error('Usage: pljs remove <target>');
    process.exit(1);
  }

  const target = args[0];
  const { name, version } = parseRemoveTarget(target);
  const client = await connect(flags);

  try {
    let result;

    await withTransaction(client, async (c) => {
      if (version) {
        const modResult = await c.query(
          'DELETE FROM pljs.modules WHERE package_name = $1 AND package_version = $2', [name, version]
        );
        const pkgResult = await c.query(
          'DELETE FROM pljs.packages WHERE name = $1 AND version = $2', [name, version]
        );
        result = {
          type: pkgResult.rowCount > 0 ? 'package' : 'module',
          name, version,
          modulesRemoved: modResult.rowCount,
        };
      } else {
        const modResult = await c.query('DELETE FROM pljs.modules WHERE package_name = $1', [name]);
        const pkgResult = await c.query('DELETE FROM pljs.packages WHERE name = $1', [name]);

        if (pkgResult.rowCount > 0) {
          result = { type: 'package', name, modulesRemoved: modResult.rowCount, versionsRemoved: pkgResult.rowCount };
        } else {
          const standaloneResult = await c.query(
            'DELETE FROM pljs.modules WHERE path = $1 AND package_name IS NULL', [name]
          );
          result = { type: 'module', name, removed: standaloneResult.rowCount > 0 };
        }
      }
    });

    if (flags.json) {
      console.log(JSON.stringify(result, null, 2));
    } else {
      if (result.type === 'package') {
        console.log(`Removed package '${name}'${version ? `@${version}` : ''}`);
      } else if (result.removed) {
        console.log(`Removed module '${name}'`);
      } else {
        console.log(`No module or package found with name '${name}'`);
      }
    }
  } finally {
    await client.end();
  }
}
