import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { buildConnectionConfig, withTransaction } from '../src/db.js';

function createMockClient(queryResults = {}) {
  const queries = [];
  return {
    queries,
    query: async (text, params) => {
      queries.push({ text, params });
      return queryResults[text] || { rows: [], rowCount: 0 };
    },
    end: async () => {},
  };
}

describe('withTransaction', () => {
  it('wraps function in BEGIN/COMMIT', async () => {
    const client = createMockClient();
    const result = await withTransaction(client, async (c) => {
      await c.query('SELECT 1');
      return 'ok';
    });
    assert.strictEqual(result, 'ok');
    assert.strictEqual(client.queries[0].text, 'BEGIN');
    assert.strictEqual(client.queries[1].text, 'SELECT 1');
    assert.strictEqual(client.queries[2].text, 'COMMIT');
  });

  it('rolls back on error', async () => {
    const client = createMockClient();
    await assert.rejects(
      () => withTransaction(client, async () => { throw new Error('fail'); }),
      { message: 'fail' },
    );
    assert.strictEqual(client.queries[0].text, 'BEGIN');
    assert.strictEqual(client.queries[1].text, 'ROLLBACK');
  });

  it('returns the value from the callback', async () => {
    const client = createMockClient();
    const result = await withTransaction(client, async () => ({ data: 42 }));
    assert.deepStrictEqual(result, { data: 42 });
  });
});

describe('buildConnectionConfig', () => {
  it('returns empty config when no flags or env vars', () => {
    const config = buildConnectionConfig({}, {});
    assert.deepStrictEqual(config, {});
  });

  it('uses env vars when no flags provided', () => {
    const env = {
      PGHOST: 'envhost',
      PGPORT: '5433',
      PGDATABASE: 'envdb',
      PGUSER: 'envuser',
      PGPASSWORD: 'envpass',
    };
    const config = buildConnectionConfig({}, env);
    assert.strictEqual(config.host, 'envhost');
    assert.strictEqual(config.port, 5433);
    assert.strictEqual(config.database, 'envdb');
    assert.strictEqual(config.user, 'envuser');
    assert.strictEqual(config.password, 'envpass');
  });

  it('flags override env vars', () => {
    const env = { PGHOST: 'envhost', PGDATABASE: 'envdb' };
    const flags = { host: 'flaghost', dbname: 'flagdb' };
    const config = buildConnectionConfig(flags, env);
    assert.strictEqual(config.host, 'flaghost');
    assert.strictEqual(config.database, 'flagdb');
  });

  it('connection string overrides everything', () => {
    const env = { PGHOST: 'envhost' };
    const flags = {
      host: 'flaghost',
      'connection-string': 'postgres://u:p@cshost:5434/csdb',
    };
    const config = buildConnectionConfig(flags, env);
    assert.strictEqual(config.connectionString, 'postgres://u:p@cshost:5434/csdb');
    assert.strictEqual(config.host, undefined);
  });

  it('PGCONNECTION_STRING env var works', () => {
    const env = { PGCONNECTION_STRING: 'postgres://envcs:5435/db' };
    const config = buildConnectionConfig({}, env);
    assert.strictEqual(config.connectionString, 'postgres://envcs:5435/db');
  });

  it('flag connection-string overrides PGCONNECTION_STRING env', () => {
    const env = { PGCONNECTION_STRING: 'postgres://envcs/db' };
    const flags = { 'connection-string': 'postgres://flagcs/db' };
    const config = buildConnectionConfig(flags, env);
    assert.strictEqual(config.connectionString, 'postgres://flagcs/db');
  });

  it('port is parsed as integer', () => {
    const config = buildConnectionConfig({ port: '5433' }, {});
    assert.strictEqual(config.port, 5433);
  });
});
