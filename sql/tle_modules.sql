-- Test PL/JS module extensions: inline @version/@range, QuickJS Bytecode, requires allowlist, and is_trusted
DELETE FROM pljs.modules;
DELETE FROM pljs.packages;

-- 1. Install versioned packages in pljs.packages and pljs.modules (v1.0.0 and v1.2.0) and a nested dependency
INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES
  ('math-core', '1.0.0', 'index.js', '{}'),
  ('math-utils', '1.0.0', 'index.js', '{"math-core": "^1.0.0"}'),
  ('math-utils', '1.2.0', 'index.js', '{"math-core": "^1.0.0"}'),
  ('string-helpers', '2.1.0', 'index.js', '{}'),
  ('fast-mod', '1.0.0', 'index.js', '{}');

INSERT INTO pljs.modules (path, package_name, package_version, source) VALUES
  ('index.js', 'math-core', '1.0.0', 'export function square(x) { return x * x; }'),
  ('index.js', 'math-utils', '1.0.0', 'import { square } from "math-core@1.0.0"; export function sumSquares(a, b) { return square(a) + square(b); } export const VERSION = "1.0.0";'),
  ('index.js', 'math-utils', '1.2.0', 'import { square } from "math-core@1.0.0"; export function sumSquares(a, b) { return square(a) + square(b); } export function cube(x) { return x * x * x; } export const VERSION = "1.2.0";');

-- 2. Install a CommonJS package module
INSERT INTO pljs.modules (path, package_name, package_version, source) VALUES
  ('index.js', 'string-helpers', '2.1.0', 'exports.shout = function(s) { return s.toUpperCase() + "!"; };');

-- 3. Test static ES Module import with explicit inline version and latest version resolution
CREATE OR REPLACE FUNCTION test_esm_pinned(a int, b int) RETURNS text LANGUAGE pljs AS $$
  import { sumSquares, VERSION } from 'tle:math-utils@1.0.0';
  return VERSION + ':' + sumSquares(a, b);
$$;

CREATE OR REPLACE FUNCTION test_esm_latest(a int, b int) RETURNS text LANGUAGE pljs AS $$
  import { sumSquares, cube, VERSION } from 'math-utils';
  return VERSION + ':' + sumSquares(a, b) + ':' + cube(a);
$$;

SELECT test_esm_pinned(3, 4);
SELECT test_esm_latest(3, 4);

-- 4. Test CommonJS require() / pljs.require()
CREATE OR REPLACE FUNCTION test_cjs_require(msg text) RETURNS text LANGUAGE pljs AS $$
  const helpers = require('string-helpers@2.1.0');
  const math = pljs.import('math-utils@1.0.0');
  return helpers.shout(msg) + ' (' + math.sumSquares(5, 12) + ')';
$$;

SELECT test_cjs_require('hello tle');

-- 5. Test QuickJS precompiled bytecode module (pljs.compile_bytecode + bytecode BYTEA column)
DO LANGUAGE pljs $$
  const bc = pljs.compile_bytecode(
    'export function fastAdd(x, y) { return x + y + 100; }',
    'fast-mod@1.0.0'
  );
  pljs.execute(
    'INSERT INTO pljs.modules (path, package_name, package_version, bytecode) VALUES ($1, $2, $3, $4)',
    ['index.js', 'fast-mod', '1.0.0', bc]
  );
$$;

CREATE OR REPLACE FUNCTION test_bytecode_import(x int, y int) RETURNS int LANGUAGE pljs AS $$
  import { fastAdd } from 'fast-mod@1.0.0';
  return fastAdd(x, y);
$$;

SELECT test_bytecode_import(10, 20);

-- 6. Test automatic invalidation when a module in pljs.modules is updated
UPDATE pljs.modules
SET source = 'export function sumSquares(a, b) { return (a + b) * 1000; } export const VERSION = "1.0.0-patched";'
WHERE package_name = 'math-utils' AND package_version = '1.0.0' AND path = 'index.js';

SELECT test_esm_pinned(3, 4);

-- 7. Test DO block with static ES module import
DO LANGUAGE pljs $$
  import { VERSION } from 'math-utils@1.2.0';
  pljs.elog(NOTICE, 'DO block imported math-utils version: ' + VERSION);
$$;

DROP FUNCTION test_esm_pinned(int, int);
DROP FUNCTION test_esm_latest(int, int);
DROP FUNCTION test_cjs_require(text);
DROP FUNCTION test_bytecode_import(int, int);
DELETE FROM pljs.modules;
DELETE FROM pljs.packages;

-- 8. Test requires allowlist, is_trusted flag, relative imports (./ and ../),
--    and semicolon-in-string/comment import parsing
GRANT USAGE ON SCHEMA pljs TO PUBLIC;
GRANT SELECT ON pljs.modules TO PUBLIC;
GRANT SELECT ON pljs.packages TO PUBLIC;

INSERT INTO pljs.modules (path, source, requires, is_trusted) VALUES
  ('pkg/sub/leaf.js', 'export function leafVal() { return 11; }', NULL, true),
  ('pkg/helper.js', 'import { leafVal } from "./sub/leaf.js"; export function doubleLeaf(x) { return x * 2 + leafVal(); }', ARRAY['pkg/sub/leaf.js'], true),
  ('pkg/sub/back.js', 'import { doubleLeaf } from "../helper.js"; export function fromSub(x) { return doubleLeaf(x) + 1; }', ARRAY['pkg/helper.js'], true),
  ('pkg/blocked.js', 'import { leafVal } from "./sub/leaf.js"; export function bad() { return leafVal(); }', ARRAY['other_mod'], true),
  ('pkg/escape.js', 'import { val } from "../../semi;mod"; export function esc() { return val(); }', NULL, true),
  ('semi;mod', 'export function val() { return 42; }', NULL, true),
  ('admin_secret', 'export function secret() { return "top-secret"; }', NULL, false);

CREATE OR REPLACE FUNCTION test_rel_and_semi(x int) RETURNS int LANGUAGE pljs AS $$
  /* comment with ; before import */
  import {
    "fromSub" as fromSub
  } from 'tle:pkg/sub/back.js'
  import { val } from "tle:semi;mod";
  return fromSub(x) + val();
$$;

SELECT test_rel_and_semi(5);

-- Relative specifier traversal above root rejection
DO $$
BEGIN
  BEGIN
    CREATE OR REPLACE FUNCTION test_escape_root() RETURNS int LANGUAGE pljs AS $fn$
      import { esc } from 'tle:pkg/escape.js';
      return esc();
    $fn$;
    PERFORM test_escape_root();
    RAISE EXCEPTION 'Expected root traversal rejection';
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'Caught expected root traversal rejection';
  END;
END;
$$;

-- Requires allowlist enforcement
DO $$
BEGIN
  BEGIN
    CREATE OR REPLACE FUNCTION test_blocked_req() RETURNS int LANGUAGE pljs AS $fn$
      import { bad } from 'tle:pkg/blocked.js';
      return bad();
    $fn$;
    PERFORM test_blocked_req();
    RAISE EXCEPTION 'Expected requires allowlist violation';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected requires violation';
  END;
END;
$$;

-- Untrusted module enforcement for unprivileged role
CREATE OR REPLACE FUNCTION test_untrusted_tle() RETURNS text LANGUAGE pljs AS $$
  import { secret } from 'tle:admin_secret';
  return secret();
$$;

SELECT test_untrusted_tle();

CREATE ROLE pljs_unpriv_tle_user;
GRANT EXECUTE ON FUNCTION test_untrusted_tle() TO pljs_unpriv_tle_user;
SET ROLE pljs_unpriv_tle_user;
DO $$
BEGIN
  BEGIN
    PERFORM test_untrusted_tle();
    RAISE EXCEPTION 'Expected untrusted module rejection for non-superuser';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected untrusted module rejection';
  END;
END;
$$;
RESET ROLE;

-- 9. ES module singleton semantics across multiple importers + 1-arg bytecode requires + pljs_reset()
INSERT INTO pljs.modules (path, source, requires, is_trusted) VALUES
  ('singleton_c', 'let count = 0; const p = pljs; const g = globalThis; export function inc() { return ++count; }', NULL, true),
  ('importer_a', 'import { inc } from "tle:singleton_c"; export function a() { return inc(); }', ARRAY['singleton_c'], true),
  ('importer_b', 'import { inc } from "singleton_c"; export function b() { return inc(); }', ARRAY['singleton_c'], true);

CREATE OR REPLACE FUNCTION test_esm_singleton() RETURNS int[] LANGUAGE pljs AS $$
  import { a } from 'tle:importer_a';
  import { b } from 'tle:importer_b';
  import { inc } from 'tle:singleton_c';
  return [inc(), a(), b()];
$$;

SELECT test_esm_singleton();
SELECT test_esm_singleton();

-- 1-arg pljs.compile_bytecode module must also obey its requires allowlist
DO LANGUAGE pljs $$
  const bc = pljs.compile_bytecode('import { inc } from "tle:singleton_c"; export function bcu() { return inc(); }');
  pljs.execute(
    'INSERT INTO pljs.modules (path, source, bytecode, requires, is_trusted) VALUES ($1, $2, $3, $4, true)',
    ['bc_restricted', '', bc, ['other_mod']]
  );
$$;

DO $$
BEGIN
  BEGIN
    CREATE OR REPLACE FUNCTION test_bc_restricted() RETURNS int LANGUAGE pljs AS $fn$
      import { bcu } from 'tle:bc_restricted';
      return bcu();
    $fn$;
    PERFORM test_bc_restricted();
    RAISE EXCEPTION 'Expected requires violation for 1-arg bytecode module';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected 1-arg bytecode requires violation';
  END;
END;
$$;

-- pljs_reset() must cleanly tear down contexts with loaded ES modules referencing pljs/globalThis
SELECT pljs_reset();
SELECT test_esm_singleton();
SELECT pljs_reset();

-- Static import from pljs.modules
INSERT INTO pljs.modules (path, source) VALUES
  ('pljs_native_esm', 'export function nativeMul(x, y) { return x * y; }');

CREATE OR REPLACE FUNCTION test_pljs_native_static_import(x int, y int) RETURNS int LANGUAGE pljs AS $$
  import { nativeMul } from 'pljs_native_esm';
  return nativeMul(x, y);
$$;

SELECT test_pljs_native_static_import(6, 7);

DROP FUNCTION test_pljs_native_static_import(int, int);
DROP FUNCTION test_esm_singleton();
DROP FUNCTION test_untrusted_tle();
DROP FUNCTION test_rel_and_semi(int);
DROP ROLE pljs_unpriv_tle_user;
DELETE FROM pljs.modules;
DELETE FROM pljs.packages;
