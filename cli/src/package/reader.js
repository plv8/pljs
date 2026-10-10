import fs from 'node:fs';
import path from 'node:path';

export function resolveEntryPoint(pkg) {
  // 1. Check exports field (highest priority).
  if (pkg.exports) {
    if (typeof pkg.exports === 'string') {
      return pkg.exports;
    }
    const dot = pkg.exports['.'];
    if (dot) {
      if (typeof dot === 'string') return dot;
      if (dot.browser) return dot.browser;
      if (dot.default) return dot.default;
    }
  }

  // 2. Check browser field.
  if (pkg.browser) {
    if (typeof pkg.browser === 'string') {
      return pkg.browser;
    }
    // Object form: check if it remaps the main entry.
    if (pkg.main) {
      const mainWithPrefix = pkg.main.startsWith('./') ? pkg.main : `./${pkg.main}`;
      const mainWithout = pkg.main.startsWith('./') ? pkg.main.slice(2) : pkg.main;
      if (pkg.browser[mainWithPrefix]) return pkg.browser[mainWithPrefix];
      if (pkg.browser[mainWithout]) return pkg.browser[mainWithout];
    }
  }

  // 3. Main field.
  if (pkg.main) return pkg.main;

  // 4. Default.
  return 'index.js';
}

export function readPackageInfo(pkg) {
  return {
    name: pkg.name,
    version: pkg.version || '0.0.0',
    entryPoint: resolveEntryPoint(pkg),
    dependencies: pkg.dependencies || {},
    browser: pkg.browser || null,
    exports: pkg.exports || null,
  };
}

export function readPackageJson(dir) {
  const filePath = path.join(dir, 'package.json');
  const content = fs.readFileSync(filePath, 'utf-8');
  return readPackageInfo(JSON.parse(content));
}

export function collectPackageFiles(packageDir) {
  const files = [];
  function traverse(dir, prefix) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (entry.name === 'node_modules' || entry.name.startsWith('.')) continue;
      const fullPath = path.join(dir, entry.name);
      const relPath = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isDirectory()) {
        traverse(fullPath, relPath);
      } else if (/\.(js|mjs|cjs)$/.test(entry.name)) {
        files.push({ fullPath, relPath });
      }
    }
  }
  traverse(packageDir, '');
  return files;
}
