-- ============================================================
-- Package system tests
-- ============================================================

-- Basic package with single entry-point module (CommonJS)
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('DateUtils', '1.0.0', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'function formatDate(d) { return d.toISOString().split("T")[0]; } module.exports = { formatDate };', 'DateUtils', '1.0.0');

DO $$
  var du = pljs.require('DateUtils');
  pljs.elog(INFO, typeof du.formatDate);
$$ LANGUAGE pljs;

-- Package with multiple versions — latest should be picked
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('DateUtils', '1.1.0', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'function formatDate(d) { return d.toISOString().split("T")[0]; } function formatTime(d) { return d.toISOString().split("T")[1]; } module.exports = { formatDate, formatTime };', 'DateUtils', '1.1.0');

DO $$
  var du = pljs.require('DateUtils');
  pljs.elog(INFO, typeof du.formatDate);
  pljs.elog(INFO, typeof du.formatTime);
$$ LANGUAGE pljs;

-- Package with sub-module access
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('helpers', 'module.exports = { pad: function(n) { return n < 10 ? "0" + n : "" + n; } };', 'DateUtils', '1.1.0');

DO $$
  var helpers = pljs.require('DateUtils/helpers');
  pljs.elog(INFO, helpers.pad(5));
  pljs.elog(INFO, helpers.pad(12));
$$ LANGUAGE pljs;

-- ES6 module package
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('MathLib_ES', '2.0.0', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'export function add(a, b) { return a + b; } export function multiply(a, b) { return a * b; }', 'MathLib_ES', '2.0.0');

DO $$
  const math = pljs.import('MathLib_ES');
  pljs.elog(INFO, math.add(10, 20));
  pljs.elog(INFO, math.multiply(3, 7));
$$ LANGUAGE pljs;

-- CommonJS module package for nested requires
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('MathLib', '2.0.0', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'function add(a, b) { return a + b; } function multiply(a, b) { return a * b; } module.exports = { add, multiply };', 'MathLib', '2.0.0');

-- Nested dependency resolution
-- Calculator@1.0.0 depends on MathLib@^2.0.0
INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES
  ('Calculator', '1.0.0', 'index', '{"MathLib": "^2.0.0"}');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'var math = pljs.require("MathLib"); module.exports = { doubleAdd: function(a, b) { return math.add(a, b) * 2; } };', 'Calculator', '1.0.0');

DO $$
  var calc = pljs.require('Calculator');
  pljs.elog(INFO, 'doubleAdd(3, 4) = ' + calc.doubleAdd(3, 4));
$$ LANGUAGE pljs;

-- Multiple versions with dependency pinning
-- Add MathLib 1.0.0 with different behavior
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('MathLib', '1.0.0', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'function add(a, b) { return a + b + 100; } module.exports = { add };', 'MathLib', '1.0.0');

-- OldCalc depends on MathLib@~1.0.0 (should get 1.0.0, not 2.0.0)
INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES
  ('OldCalc', '1.0.0', 'index', '{"MathLib": "~1.0.0"}');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'var math = pljs.require("MathLib"); module.exports = { compute: function(a, b) { return math.add(a, b); } };', 'OldCalc', '1.0.0');

DO $$
  var oldCalc = pljs.require('OldCalc');
  pljs.elog(INFO, 'OldCalc compute(3, 4) = ' + oldCalc.compute(3, 4));
$$ LANGUAGE pljs;

-- Top-level MathLib require should still get latest (2.0.0)
DO $$
  var math = pljs.require('MathLib');
  pljs.elog(INFO, 'top-level add(3, 4) = ' + math.add(3, 4));
$$ LANGUAGE pljs;

-- Standalone modules still work (backward compat)
INSERT INTO pljs.modules (path, source) VALUES
  ('standalone_util', 'module.exports = { hello: function() { return "world"; } };');

DO $$
  var util = pljs.require('standalone_util');
  pljs.elog(INFO, 'standalone: ' + util.hello());
$$ LANGUAGE pljs;

-- Standalone modules take priority over packages with same name
INSERT INTO pljs.modules (path, source) VALUES
  ('priority_test', 'module.exports = "standalone";');
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('priority_test', '1.0.0', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'module.exports = "package";', 'priority_test', '1.0.0');

DO $$
  var result = pljs.require('priority_test');
  pljs.elog(INFO, 'priority: ' + result);
$$ LANGUAGE pljs;

-- Error: nonexistent package
DO $$
  try {
    pljs.require('NonexistentPackage');
    pljs.elog(ERROR, 'should not reach here');
  } catch (e) {
    pljs.elog(INFO, e.message);
  }
$$ LANGUAGE pljs;

-- Error: nonexistent sub-module in existing package
DO $$
  try {
    pljs.require('DateUtils/nonexistent');
    pljs.elog(ERROR, 'should not reach here');
  } catch (e) {
    pljs.elog(INFO, e.message);
  }
$$ LANGUAGE pljs;

-- ============================================================
-- ^ ranges below 1.0.0 stay inside the left-most non-zero part
-- ============================================================
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('ZeroX', '0.2.3', 'index'),
  ('ZeroX', '0.2.9', 'index'),
  ('ZeroX', '0.3.0', 'index'),
  ('ZeroX', '0.0.3', 'index'),
  ('ZeroX', '0.0.4', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'module.exports = "0.2.3";', 'ZeroX', '0.2.3'),
  ('index', 'module.exports = "0.2.9";', 'ZeroX', '0.2.9'),
  ('index', 'module.exports = "0.3.0";', 'ZeroX', '0.3.0'),
  ('index', 'module.exports = "0.0.3";', 'ZeroX', '0.0.3'),
  ('index', 'module.exports = "0.0.4";', 'ZeroX', '0.0.4');

INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES
  ('NeedsMinor', '1.0.0', 'index', '{"ZeroX": "^0.2.3"}'),
  ('NeedsPatch', '1.0.0', 'index', '{"ZeroX": "^0.0.3"}'),
  ('NeedsTilde', '1.0.0', 'index', '{"ZeroX": "~0.2.3"}'),
  ('NeedsNone', '1.0.0', 'index', '{}');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'module.exports = pljs.require("ZeroX");', 'NeedsMinor', '1.0.0'),
  ('index', 'module.exports = pljs.require("ZeroX");', 'NeedsPatch', '1.0.0'),
  ('index', 'module.exports = pljs.require("ZeroX");', 'NeedsTilde', '1.0.0'),
  ('index', 'module.exports = pljs.require("ZeroX");', 'NeedsNone', '1.0.0');

DO $$
  pljs.elog(INFO, '^0.2.3 -> ' + pljs.require('NeedsMinor'));
  pljs.elog(INFO, '^0.0.3 -> ' + pljs.require('NeedsPatch'));
  pljs.elog(INFO, '~0.2.3 -> ' + pljs.require('NeedsTilde'));
  pljs.elog(INFO, 'unlisted -> ' + pljs.require('NeedsNone'));
$$ LANGUAGE pljs;

-- ============================================================
-- Versions that are not plain major.minor.patch are never selected
-- ============================================================
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('Pre', '1.0.0', 'index'),
  ('Pre', '2.0.0-beta', 'index'),
  ('Pre', '1.5', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'module.exports = "1.0.0";', 'Pre', '1.0.0'),
  ('index', 'module.exports = "2.0.0-beta";', 'Pre', '2.0.0-beta'),
  ('index', 'module.exports = "1.5";', 'Pre', '1.5');

DO $$
  pljs.elog(INFO, pljs.require('Pre'));
$$ LANGUAGE pljs;

-- ============================================================
-- Scoped package names
-- ============================================================
INSERT INTO pljs.packages (name, version, entry_point) VALUES
  ('@acme/util', '1.0.0', 'index');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'module.exports = { name: "acme util" };', '@acme/util', '1.0.0'),
  ('strings', 'module.exports = { up: function(s) { return s.toUpperCase(); } };', '@acme/util', '1.0.0');

DO $$
  pljs.elog(INFO, pljs.require('@acme/util').name);
  pljs.elog(INFO, pljs.require('@acme/util/strings').up('scoped'));
$$ LANGUAGE pljs;

-- A malformed range in a dependency fails to load rather than guessing.
INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES
  ('BadRange', '1.0.0', 'index', '{"ZeroX": "not-a-range"}');
INSERT INTO pljs.modules (path, source, package_name, package_version) VALUES
  ('index', 'module.exports = pljs.require("ZeroX");', 'BadRange', '1.0.0');

DO $$
  try {
    pljs.require('BadRange');
    pljs.elog(ERROR, 'should not reach here');
  } catch (e) {
    pljs.elog(INFO, e.message);
  }
$$ LANGUAGE pljs;

-- ============================================================
-- Lookups agree with the index's collation (names that sort differently
-- under C and a linguistic collation, with several rows in the index)
-- ============================================================
INSERT INTO pljs.packages (name, version, entry_point)
  SELECT n, '1.0.0', 'index' FROM unnest(ARRAY['alpha', 'Alpha', 'ALPHA', '_alpha', 'alpha-2', 'Zulu', 'zulu', 'beta', 'Beta', '9lives']) AS n;
INSERT INTO pljs.modules (path, source, package_name, package_version)
  SELECT 'index', 'module.exports = ' || quote_literal(n) || ';', n, '1.0.0'
  FROM unnest(ARRAY['alpha', 'Alpha', 'ALPHA', '_alpha', 'alpha-2', 'Zulu', 'zulu', 'beta', 'Beta', '9lives']) AS n;

DO $$
  var names = ['alpha', 'Alpha', 'ALPHA', '_alpha', 'alpha-2', 'Zulu', 'zulu', 'beta', 'Beta', '9lives'];
  var bad = names.filter(function(n) { return pljs.require(n) !== n; });
  pljs.elog(INFO, 'mismatches: ' + bad.length);
$$ LANGUAGE pljs;

-- Cleanup
DELETE FROM pljs.modules;
DELETE FROM pljs.packages;
