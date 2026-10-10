import pg from 'pg';

export function buildConnectionConfig(flags, env = process.env) {
  const connectionString = flags['connection-string'] || env.PGCONNECTION_STRING;
  if (connectionString) {
    return { connectionString };
  }

  const config = {};

  const host = flags.host || env.PGHOST;
  if (host) config.host = host;

  const port = flags.port || env.PGPORT;
  if (port) config.port = parseInt(port, 10);

  const database = flags.dbname || env.PGDATABASE;
  if (database) config.database = database;

  const user = flags.username || env.PGUSER;
  if (user) config.user = user;

  const password = flags.password || env.PGPASSWORD;
  if (password) config.password = password;

  return config;
}

export async function connect(flags) {
  const config = buildConnectionConfig(flags);
  const client = new pg.Client(config);
  await client.connect();
  return client;
}

export async function withTransaction(client, fn) {
  await client.query('BEGIN');
  try {
    const result = await fn(client);
    await client.query('COMMIT');
    return result;
  } catch (err) {
    await client.query('ROLLBACK');
    throw err;
  }
}
