import { describe, it, mock, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';

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

const mockClient = createMockClient({
  'SELECT name, version, entry_point FROM pljs.packages ORDER BY name, version': {
    rows: [{ name: 'lodash', version: '4.17.21', entry_point: 'index' }],
  },
  'SELECT path FROM pljs.modules WHERE package_name IS NULL ORDER BY path': {
    rows: [{ path: 'my_util' }],
  },
});

mock.module('../../src/db.js', {
  exports: {
    connect: async () => mockClient,
    buildConnectionConfig: () => ({}),
    withTransaction: async (client, fn) => fn(client),
  },
});

const { formatListOutput, list } = await import('../../src/commands/list.js');

describe('formatListOutput', () => {
  it('formats packages and modules as text', () => {
    const output = formatListOutput({
      packages: [
        { name: 'lodash', version: '4.17.21', entry_point: 'index' },
        { name: 'moment', version: '2.29.4', entry_point: 'moment' },
      ],
      modules: [
        { path: 'my_util' },
        { path: 'helpers' },
      ],
    });
    assert.ok(output.includes('lodash'));
    assert.ok(output.includes('4.17.21'));
    assert.ok(output.includes('my_util'));
  });

  it('formats as JSON', () => {
    const data = {
      packages: [{ name: 'lodash', version: '4.17.21', entry_point: 'index' }],
      modules: [{ path: 'my_util' }],
    };
    const output = formatListOutput(data, true);
    const parsed = JSON.parse(output);
    assert.strictEqual(parsed.packages.length, 1);
    assert.strictEqual(parsed.modules.length, 1);
  });

  it('handles empty results', () => {
    const output = formatListOutput({ packages: [], modules: [] });
    assert.ok(output.includes('No packages'));
    assert.ok(output.includes('No standalone modules'));
  });
});

describe('list', () => {
  it('queries packages and modules and outputs text', async () => {
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await list([], { json: false });
    } finally {
      console.log = origLog;
    }
    const output = logs.join('\n');
    assert.ok(output.includes('lodash'));
    assert.ok(output.includes('my_util'));
  });

  it('queries packages and modules and outputs JSON', async () => {
    const logs = [];
    const origLog = console.log;
    console.log = (...args) => logs.push(args.join(' '));
    try {
      await list([], { json: true });
    } finally {
      console.log = origLog;
    }
    const parsed = JSON.parse(logs.join('\n'));
    assert.strictEqual(parsed.packages.length, 1);
    assert.strictEqual(parsed.modules.length, 1);
  });

  it('calls client.end after listing', async () => {
    let endCalled = false;
    const origEnd = mockClient.end;
    mockClient.end = async () => { endCalled = true; };
    const origLog = console.log;
    console.log = () => {};
    try {
      await list([], {});
    } finally {
      console.log = origLog;
      mockClient.end = origEnd;
    }
    assert.ok(endCalled);
  });
});
