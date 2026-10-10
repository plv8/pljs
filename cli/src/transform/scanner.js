import path from 'node:path';

export const NODE_BUILTINS = new Set([
  'assert', 'async_hooks', 'buffer', 'child_process', 'cluster', 'crypto',
  'dgram', 'dns', 'events', 'fs', 'http', 'http2', 'https', 'inspector',
  'net', 'os', 'path', 'perf_hooks', 'querystring', 'readline', 'stream',
  'string_decoder', 'tls', 'timers', 'trace_events', 'tty', 'url', 'util',
  'v8', 'vm', 'wasi', 'worker_threads', 'zlib',
]);

const POLYFILL_MAP = {
  buffer: ['buffer'],
  crypto: ['crypto-browserify'],
  events: ['events'],
  http: ['stream-http'],
  https: ['https-browserify'],
  os: ['os-browserify'],
  path: ['path-browserify', 'path'],
  stream: ['stream-browserify', 'readable-stream'],
  url: ['url'],
  util: ['util'],
  zlib: ['browserify-zlib'],
  assert: ['assert'],
  querystring: ['querystring-es3'],
  string_decoder: ['string_decoder'],
  timers: ['timers-browserify'],
  tty: ['tty-browserify'],
  vm: ['vm-browserify'],
};

export function isNodeBuiltin(specifier) {
  if (specifier.startsWith('node:')) return true;
  return NODE_BUILTINS.has(specifier);
}

function stripNodePrefix(specifier) {
  return specifier.startsWith('node:') ? specifier.slice(5) : specifier;
}

export function resolveBuiltinFallback(specifier, options = {}) {
  const { browser, nodeModulesDir, existsSync: checkExists } = options;
  const bare = stripNodePrefix(specifier);

  // 1. Check browser field.
  if (browser && bare in browser) {
    const mapping = browser[bare];
    if (mapping === false) {
      return { type: 'disabled' };
    }
    if (typeof mapping === 'string') {
      return { type: 'browser-field', replacement: mapping };
    }
  }

  // 2. Check for polyfill in node_modules.
  const polyfills = POLYFILL_MAP[bare];
  if (polyfills && nodeModulesDir && checkExists) {
    for (const polyfill of polyfills) {
      if (checkExists(path.join(nodeModulesDir, polyfill))) {
        return { type: 'polyfill', replacement: polyfill };
      }
    }
  }

  // 3. No fallback found.
  return null;
}
