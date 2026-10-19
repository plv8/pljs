import fs from 'node:fs';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { rewrite } from '../transform/rewriter.js';
import { resolveModulePath } from '../transform/resolver.js';
import { isNodeBuiltin, resolveBuiltinFallback } from '../transform/scanner.js';
import { readPackageJson, collectPackageFiles } from './reader.js';

export function processStandaloneSource(source) {
  const warnings = [];
  const rewritten = rewrite(source, { warnings });
  return { source: rewritten, warnings };
}

export function processPackageSource(source, options) {
  const { packageName, filePath, browser, nodeModulesDir } = options;
  const warnings = [];

  const rewritten = rewrite(source, {
    warnings,
    resolveSpecifier: (specifier) => {
      if (isNodeBuiltin(specifier)) {
        const fallback = resolveBuiltinFallback(specifier, {
          browser,
          nodeModulesDir,
          existsSync: fs.existsSync,
        });
        if (!fallback) {
          throw new Error(
            `Unsupported Node.js built-in '${specifier}' imported in ${filePath}. ` +
            `pljs runs inside PostgreSQL as a trusted extension. ` +
            `No browser/polyfill fallback was found.`
          );
        }
        if (fallback.type === 'disabled') return null;
        return fallback.replacement;
      }
      return resolveModulePath(specifier, { packageName, filePath });
    },
  });

  return { source: rewritten, warnings };
}

export function processPackage(packageDir, nodeModulesDir) {
  const pkgInfo = readPackageJson(packageDir);
  const files = collectPackageFiles(packageDir);
  const modules = [];
  const warnings = [];

  for (const file of files) {
    const source = fs.readFileSync(file.fullPath, 'utf-8');
    const modulePath = file.relPath.replace(/\.(js|mjs|cjs)$/, '');

    const result = processPackageSource(source, {
      packageName: pkgInfo.name,
      filePath: file.relPath,
      browser: pkgInfo.browser,
      nodeModulesDir,
    });

    modules.push({ path: modulePath, source: result.source });
    warnings.push(...result.warnings);
  }

  const entryPoint = pkgInfo.entryPoint
    .replace(/^\.\//, '')
    .replace(/\.(js|mjs|cjs)$/, '');

  return {
    package: {
      name: pkgInfo.name,
      version: pkgInfo.version,
      entryPoint,
      dependencies: pkgInfo.dependencies,
    },
    modules,
    warnings,
  };
}

export function npmInstall(targetDir, packageSpec) {
  execSync(`npm install --production ${packageSpec || ''}`, {
    cwd: targetDir,
    stdio: 'pipe',
  });
}

export function ensureNodeModules(targetDir) {
  const nodeModulesDir = path.join(targetDir, 'node_modules');
  if (!fs.existsSync(nodeModulesDir)) {
    npmInstall(targetDir);
  }
  return nodeModulesDir;
}
