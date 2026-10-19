import path from 'node:path';

export function resolveModulePath(specifier, options = {}) {
  const { packageName, filePath } = options;

  // Bare specifiers (package names) — leave as-is.
  if (!specifier.startsWith('.') && !specifier.startsWith('/')) {
    return specifier;
  }

  // Relative path but no package context — leave as-is (standalone module).
  if (!packageName) {
    return specifier;
  }

  // Resolve relative to current file's directory within the package.
  const fileDir = path.dirname(filePath);
  let resolved = path.posix.normalize(path.posix.join(fileDir, specifier));

  // Strip JS extensions.
  resolved = resolved.replace(/\.(js|mjs|cjs)$/, '');

  return `${packageName}/${resolved}`;
}
