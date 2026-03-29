-- Basic module import
INSERT INTO pljs.modules (path, source) VALUES
  ('math_utils', 'export function add(a, b) { return a + b; } export function multiply(a, b) { return a * b; }');

DO $$
  const math = pljs.import('math_utils');
  pljs.elog(INFO, math.add(2, 3));
  pljs.elog(INFO, math.multiply(4, 5));
$$ LANGUAGE pljs;

-- Module with default-style export (named)
INSERT INTO pljs.modules (path, source) VALUES
  ('greeter', 'export function greet(name) { return "Hello, " + name + "!"; }');

CREATE FUNCTION test_module_import() RETURNS text AS $$
  const mod = pljs.import('greeter');
  return mod.greet('world');
$$ LANGUAGE pljs;
SELECT test_module_import();

-- Module with path-like names
INSERT INTO pljs.modules (path, source) VALUES
  ('lib/strings', 'export function upper(s) { return s.toUpperCase(); } export function lower(s) { return s.toLowerCase(); }');

DO $$
  const strings = pljs.import('lib/strings');
  pljs.elog(INFO, strings.upper('hello'));
  pljs.elog(INFO, strings.lower('WORLD'));
$$ LANGUAGE pljs;

-- Deeper path-like name
INSERT INTO pljs.modules (path, source) VALUES
  ('core/utils/format', 'export function wrap(s) { return "[" + s + "]"; }');

DO $$
  const fmt = pljs.import('core/utils/format');
  pljs.elog(INFO, fmt.wrap('test'));
$$ LANGUAGE pljs;

-- Module with multiple exports and constants
INSERT INTO pljs.modules (path, source) VALUES
  ('constants', 'export const PI = 3.14159; export const E = 2.71828; export function circumference(r) { return 2 * PI * r; }');

CREATE FUNCTION circle_info(r float8) RETURNS text AS $$
  const c = pljs.import('constants');
  return 'r=' + r + ' C=' + c.circumference(r).toFixed(2) + ' PI=' + c.PI;
$$ LANGUAGE pljs;
SELECT circle_info(10);

-- Module that does not exist
DO $$
  try {
    pljs.import('nonexistent_module');
    pljs.elog(ERROR, 'should not reach here');
  } catch (e) {
    pljs.elog(INFO, e.message);
  }
$$ LANGUAGE pljs;

-- Using a module from a named function
INSERT INTO pljs.modules (path, source) VALUES
  ('validators', 'export function isPositive(n) { return n > 0; } export function isEven(n) { return n % 2 === 0; }');

CREATE FUNCTION check_number(n integer) RETURNS text AS $$
  const v = pljs.import('validators');
  const parts = [];
  if (v.isPositive(n)) parts.push('positive');
  if (v.isEven(n)) parts.push('even');
  return parts.length > 0 ? parts.join(', ') : 'negative and odd';
$$ LANGUAGE pljs;
SELECT check_number(4);
SELECT check_number(-3);

-- Multiple imports in one block
DO $$
  const math = pljs.import('math_utils');
  const v = pljs.import('validators');
  const result = math.add(10, 20);
  pljs.elog(INFO, 'sum=' + result + ' positive=' + v.isPositive(result));
$$ LANGUAGE pljs;

-- Module that imports another module via pljs.import()
INSERT INTO pljs.modules (path, source) VALUES
  ('base', 'export const BASE = 100; export function offset(n) { return BASE + n; }');

INSERT INTO pljs.modules (path, source) VALUES
  ('derived', 'const base = pljs.import("base"); export function compute(n) { return base.offset(n) * 2; } export const BASE = base.BASE;');

DO $$
  const derived = pljs.import('derived');
  pljs.elog(INFO, 'BASE = ' + derived.BASE);
  pljs.elog(INFO, 'compute(5) = ' + derived.compute(5));
$$ LANGUAGE pljs;

-- Module importing a module with a path-like name via pljs.import()
INSERT INTO pljs.modules (path, source) VALUES
  ('lib/format', 'const strings = pljs.import("lib/strings"); export function shout(s) { return strings.upper(s) + "!!!"; }');

DO $$
  const format = pljs.import('lib/format');
  pljs.elog(INFO, format.shout('wow'));
$$ LANGUAGE pljs;

-- Three levels of chained pljs.import()
INSERT INTO pljs.modules (path, source) VALUES
  ('core/calc', 'const c = pljs.import("constants"); export function circumference(r) { return 2 * c.PI * r; }');

INSERT INTO pljs.modules (path, source) VALUES
  ('app/geometry', 'const calc = pljs.import("core/calc"); const c = pljs.import("constants"); export function circleInfo(r) { return "r=" + r + " C=" + calc.circumference(r).toFixed(2) + " PI=" + c.PI; }');

CREATE FUNCTION circle_info_chained(r float8) RETURNS text AS $$
  const geo = pljs.import('app/geometry');
  return geo.circleInfo(r);
$$ LANGUAGE pljs;
SELECT circle_info_chained(10);

-- Module that tries to import a nonexistent module
INSERT INTO pljs.modules (path, source) VALUES
  ('bad_import', 'const nope = pljs.import("does_not_exist"); export function go() { return "ok"; }');

DO $$
  try {
    pljs.import('bad_import');
    pljs.elog(ERROR, 'should not reach here');
  } catch (e) {
    pljs.elog(INFO, e.message);
  }
$$ LANGUAGE pljs;

-- Cleanup
DROP FUNCTION circle_info_chained(float8);
DROP FUNCTION test_module_import();
DROP FUNCTION circle_info(float8);
DROP FUNCTION check_number(integer);
DELETE FROM pljs.modules;
