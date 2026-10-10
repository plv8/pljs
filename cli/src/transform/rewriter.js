import * as acorn from 'acorn';

/**
 * Recursively walk all AST nodes, calling visitor on each.
 */
function walk(node, visitor) {
  if (!node || typeof node !== 'object') return;
  visitor(node);
  for (const key of Object.keys(node)) {
    const child = node[key];
    if (Array.isArray(child)) {
      for (const item of child) {
        if (item && typeof item.type === 'string') walk(item, visitor);
      }
    } else if (child && typeof child.type === 'string') {
      walk(child, visitor);
    }
  }
}

/**
 * Rewrite ES6 import statements and CommonJS require() calls into
 * pljs.import() / pljs.require() calls.
 *
 * @param {string} source - The source code to transform
 * @param {object} [options] - Options
 * @param {Array} [options.warnings] - Array to collect warnings into
 * @param {Function} [options.resolveSpecifier] - Callback to resolve module specifiers
 * @returns {string} Transformed source code
 */
export function rewrite(source, options = {}) {
  let ast;
  try {
    ast = acorn.parse(source, {
      sourceType: 'module',
      ecmaVersion: 'latest',
    });
  } catch {
    ast = acorn.parse(source, {
      sourceType: 'script',
      ecmaVersion: 'latest',
    });
  }

  const replacements = [];
  let impCounter = 0;
  let reCounter = 0;

  for (const node of ast.body) {
    // Handle export * from '...' — throw error
    if (node.type === 'ExportAllDeclaration') {
      throw new Error(
        '"export * from" is not supported in pljs. Use named re-exports instead.'
      );
    }

    // Handle re-exports: export { foo, bar } from 'baz';
    if (node.type === 'ExportNamedDeclaration' && node.source) {
      let moduleName = node.source.value;
      if (options.resolveSpecifier) {
        const resolved = options.resolveSpecifier(moduleName);
        if (resolved === null) {
          // Disabled — replace each exported name with empty object export
          const lines = [];
          for (const spec of node.specifiers) {
            const localName = spec.exported.name || spec.exported.value;
            if (localName === 'default') {
              lines.push(`export default {};`);
            } else {
              lines.push(`export const ${localName} = {};`);
            }
          }
          replacements.push({ start: node.start, end: node.end, text: lines.join('\n') });
          continue;
        }
        moduleName = resolved;
      }
      const tmpVar = `__re${reCounter++}`;
      const lines = [`const ${tmpVar} = pljs.import('${moduleName}');`];
      for (const spec of node.specifiers) {
        const localName = spec.exported.name || spec.exported.value;
        const importedName = spec.local.name || spec.local.value;
        if (localName === 'default') {
          lines.push(`export default ${tmpVar}.${importedName};`);
        } else {
          lines.push(`export const ${localName} = ${tmpVar}.${importedName};`);
        }
      }
      replacements.push({ start: node.start, end: node.end, text: lines.join('\n') });
      continue;
    }

    if (node.type !== 'ImportDeclaration') continue;

    let moduleName = node.source.value;
    const specifiers = node.specifiers;

    if (options.resolveSpecifier) {
      const resolved = options.resolveSpecifier(moduleName);
      if (resolved === null) {
        // Disabled built-in — replace with empty object assignments
        if (specifiers.length === 0) {
          replacements.push({ start: node.start, end: node.end, text: `({});` });
        } else {
          const defaultSpec = specifiers.find(s => s.type === 'ImportDefaultSpecifier');
          const namespaceSpec = specifiers.find(s => s.type === 'ImportNamespaceSpecifier');
          const namedSpecs = specifiers.filter(s => s.type === 'ImportSpecifier');
          const lines = [];
          if (defaultSpec) lines.push(`const ${defaultSpec.local.name} = {};`);
          if (namespaceSpec) lines.push(`const ${namespaceSpec.local.name} = {};`);
          if (namedSpecs.length > 0) {
            for (const s of namedSpecs) {
              lines.push(`const ${s.local.name} = {};`);
            }
          }
          replacements.push({ start: node.start, end: node.end, text: lines.join('\n') });
        }
        continue;
      }
      moduleName = resolved;
    }

    const importCall = `pljs.import('${moduleName}')`;

    // Side-effect import: import 'foo';
    if (specifiers.length === 0) {
      replacements.push({ start: node.start, end: node.end, text: `${importCall};` });
      continue;
    }

    // Separate default/namespace specifiers from named specifiers
    const defaultSpec = specifiers.find(s => s.type === 'ImportDefaultSpecifier');
    const namespaceSpec = specifiers.find(s => s.type === 'ImportNamespaceSpecifier');
    const namedSpecs = specifiers.filter(s => s.type === 'ImportSpecifier');

    // Combined default + named: import foo, { bar, baz } from 'foo';
    if (defaultSpec && namedSpecs.length > 0) {
      const tmpVar = `__imp${impCounter++}`;
      const namedDestructure = namedSpecs
        .map(s => s.imported.name === s.local.name
          ? s.local.name
          : `${s.imported.name}: ${s.local.name}`)
        .join(', ');
      const lines = [
        `const ${tmpVar} = ${importCall};`,
        `const ${defaultSpec.local.name} = ${tmpVar}.default;`,
        `const { ${namedDestructure} } = ${tmpVar};`,
      ];
      replacements.push({ start: node.start, end: node.end, text: lines.join('\n') });
      continue;
    }

    // Default import: import foo from 'foo';
    if (defaultSpec) {
      replacements.push({
        start: node.start, end: node.end,
        text: `const ${defaultSpec.local.name} = ${importCall};`,
      });
      continue;
    }

    // Namespace import: import * as foo from 'foo';
    if (namespaceSpec) {
      replacements.push({
        start: node.start, end: node.end,
        text: `const ${namespaceSpec.local.name} = ${importCall};`,
      });
      continue;
    }

    // Named imports: import { bar, baz } from 'foo';
    if (namedSpecs.length > 0) {
      const destructure = namedSpecs
        .map(s => s.imported.name === s.local.name
          ? s.local.name
          : `${s.imported.name}: ${s.local.name}`)
        .join(', ');
      replacements.push({
        start: node.start, end: node.end,
        text: `const { ${destructure} } = ${importCall};`,
      });
    }
  }

  // Collect require/import nodes that should be disabled (resolveSpecifier returned null).
  // We need their positions to find enclosing VariableDeclarations.
  const disabledRequirePositions = new Set();

  // Walk the AST to find and rewrite require() calls and dynamic import()
  walk(ast, (node) => {
    // Handle dynamic import() expressions
    if (node.type === 'ImportExpression') {
      if (node.source.type === 'Literal' && typeof node.source.value === 'string') {
        const specifier = node.source.value;
        if (options.resolveSpecifier) {
          const resolved = options.resolveSpecifier(specifier);
          if (resolved === null) {
            // Replace the whole import() expression with ({})
            replacements.push({
              start: node.start,
              end: node.end,
              text: '({})',
            });
            return;
          }
          // Replace the whole import() with pljs.import('resolved')
          replacements.push({
            start: node.start,
            end: node.end,
            text: `pljs.import('${resolved}')`,
          });
        } else {
          // Replace `import(` with `pljs.import(`
          replacements.push({
            start: node.start,
            end: node.source.start,
            text: 'pljs.import(',
          });
        }
      } else {
        if (options.warnings) {
          options.warnings.push(
            `Dynamic import() at position ${node.start} cannot be statically rewritten`
          );
        }
      }
      return;
    }

    if (
      node.type === 'CallExpression' &&
      node.callee.type === 'Identifier' &&
      node.callee.name === 'require'
    ) {
      if (
        node.arguments.length === 1 &&
        node.arguments[0].type === 'Literal' &&
        typeof node.arguments[0].value === 'string'
      ) {
        const specifier = node.arguments[0].value;
        if (options.resolveSpecifier) {
          const resolved = options.resolveSpecifier(specifier);
          if (resolved === null) {
            // Mark this require call as disabled — handle below
            disabledRequirePositions.add(node.start);
            return;
          }
          // Replace the entire require call with the resolved specifier
          replacements.push({
            start: node.start,
            end: node.end,
            text: `pljs.require('${resolved}')`,
          });
        } else {
          // No resolveSpecifier — just replace `require` with `pljs.require`
          replacements.push({
            start: node.callee.start,
            end: node.callee.end,
            text: 'pljs.require',
          });
        }
      } else {
        // Non-string argument — push a warning if requested
        if (options.warnings) {
          options.warnings.push(
            `Dynamic require() at position ${node.start} cannot be statically rewritten`
          );
        }
      }
    }
  });

  // Handle disabled require() calls by finding their enclosing statements
  if (disabledRequirePositions.size > 0) {
    for (const bodyNode of ast.body) {
      if (
        bodyNode.type === 'VariableDeclaration' &&
        bodyNode.declarations.length === 1
      ) {
        const decl = bodyNode.declarations[0];
        if (
          decl.init &&
          decl.init.type === 'CallExpression' &&
          disabledRequirePositions.has(decl.init.start)
        ) {
          // Replace `const foo = require('bar');` with `const foo = {};`
          const varName = decl.id.type === 'Identifier'
            ? decl.id.name
            : source.slice(decl.id.start, decl.id.end);
          const keyword = source.slice(bodyNode.start, bodyNode.start + bodyNode.kind.length);
          replacements.push({
            start: bodyNode.start,
            end: bodyNode.end,
            text: `${keyword} ${varName} = {};`,
          });
          disabledRequirePositions.delete(decl.init.start);
        }
      }
    }
    // Any remaining disabled requires that weren't in a VariableDeclaration
    // (e.g., bare `require('fs');`) — replace with `({});`
    // We need to walk again to find them by position
    if (disabledRequirePositions.size > 0) {
      walk(ast, (node) => {
        if (
          node.type === 'CallExpression' &&
          disabledRequirePositions.has(node.start)
        ) {
          replacements.push({
            start: node.start,
            end: node.end,
            text: '({})',
          });
        }
      });
    }
  }

  // Apply replacements back-to-front to preserve positions
  replacements.sort((a, b) => b.start - a.start);

  let result = source;
  for (const r of replacements) {
    result = result.slice(0, r.start) + r.text + result.slice(r.end);
  }

  return result;
}
