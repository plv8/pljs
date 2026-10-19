import { describe, it, mock } from 'node:test';
import assert from 'node:assert/strict';

function createMockClient(queryResults = {}) {
  const queries = [];
  return {
    queries,
    query: async (text, params) => {
      queries.push({ text, params });
      const key = Object.keys(queryResults).find(k => text.includes(k));
      return (key ? queryResults[key] : null) || { rows: [], rowCount: 0 };
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

const { parseRemoveTarget, remove } = await import('../../src/commands/remove.js');

describe('parseRemoveTarget', () => {
  it('parses bare package name', () => {
    const result = parseRemoveTarget('lodash');
    assert.deepStrictEqual(result, { name: 'lodash', version: null });
  });

  it('parses package name with version', () => {
    const result = parseRemoveTarget('lodash@4.17.21');
    assert.deepStrictEqual(result, { name: 'lodash', version: '4.17.21' });
  });

  it('parses scoped package', () => {
    const result = parseRemoveTarget('@scope/pkg');
    assert.deepStrictEqual(result, { name: '@scope/pkg', version: null });
  });

  it('parses scoped package with version', () => {
    const result = parseRemoveTarget('@scope/pkg@1.0.0');
    assert.deepStrictEqual(result, { name: '@scope/pkg', version: '1.0.0' });
  });
});

describe('remove', () => {
  it('removes a package with version', async () => {
    mockClientInstance = createMockClient({
      'DELETE FROM pljs.modules WHERE package_name': { rowCount: 3 },
      'DELETE FROM pljs.packages WHERE name': { rowCount: 1 },
    });
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await remove(['lodash@4.17.21'], { json: false });
    } finally {
      console.log = origLog;
    }
    assert.ok(logs.some(l => l.includes("Removed package 'lodash'") && l.includes('@4.17.21')));
  });

  it('removes a package without version', async () => {
    mockClientInstance = createMockClient({
      'DELETE FROM pljs.modules WHERE package_name': { rowCount: 5 },
      'DELETE FROM pljs.packages WHERE name': { rowCount: 2 },
    });
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await remove(['lodash'], { json: false });
    } finally {
      console.log = origLog;
    }
    assert.ok(logs.some(l => l.includes("Removed package 'lodash'")));
  });

  it('removes a standalone module', async () => {
    // When pkg delete returns 0, it tries standalone module delete
    let callCount = 0;
    mockClientInstance = createMockClient();
    const origQuery = mockClientInstance.query;
    mockClientInstance.query = async (text, params) => {
      callCount++;
      if (text.includes('DELETE FROM pljs.modules WHERE package_name = $1') && !text.includes('package_version')) {
        return { rowCount: 0 };
      }
      if (text.includes('DELETE FROM pljs.packages')) {
        return { rowCount: 0 };
      }
      if (text.includes('DELETE FROM pljs.modules WHERE path')) {
        return { rowCount: 1 };
      }
      return origQuery(text, params);
    };
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await remove(['my_util'], { json: false });
    } finally {
      console.log = origLog;
    }
    assert.ok(logs.some(l => l.includes("Removed module 'my_util'")));
  });

  it('reports when nothing is found', async () => {
    mockClientInstance = createMockClient();
    mockClientInstance.query = async (text, params) => {
      return { rows: [], rowCount: 0 };
    };
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await remove(['nonexistent'], { json: false });
    } finally {
      console.log = origLog;
    }
    assert.ok(logs.some(l => l.includes('No module or package found')));
  });

  it('outputs JSON when flag is set', async () => {
    mockClientInstance = createMockClient({
      'DELETE FROM pljs.modules WHERE package_name': { rowCount: 3 },
      'DELETE FROM pljs.packages WHERE name': { rowCount: 1 },
    });
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await remove(['lodash@1.0.0'], { json: true });
    } finally {
      console.log = origLog;
    }
    const parsed = JSON.parse(logs.join('\n'));
    assert.strictEqual(parsed.name, 'lodash');
    assert.strictEqual(parsed.version, '1.0.0');
  });

  it('calls client.end in finally block', async () => {
    let endCalled = false;
    mockClientInstance = createMockClient();
    mockClientInstance.query = async () => ({ rows: [], rowCount: 0 });
    mockClientInstance.end = async () => { endCalled = true; };
    const origLog = console.log;
    console.log = () => {};
    try {
      await remove(['test'], {});
    } finally {
      console.log = origLog;
    }
    assert.ok(endCalled);
  });
});
