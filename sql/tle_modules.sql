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

-- 5. Test QuickJS precompiled bytecode module (pljs.compile_bytecode + bytecode BYTEA column, ESM and CommonJS)
DO LANGUAGE pljs $$
  const bc = pljs.compile_bytecode(
    'export function fastAdd(x, y) { return x + y + 100; }',
    'fast-mod@1.0.0'
  );
  pljs.execute(
    'INSERT INTO pljs.modules (path, package_name, package_version, bytecode) VALUES ($1, $2, $3, $4)',
    ['index.js', 'fast-mod', '1.0.0', bc]
  );
  const cjsBc = pljs.compile_bytecode(
    'exports.fastMul = function(x, y) { return x * y; };',
    'tle:fast-cjs'
  );
  pljs.execute(
    'INSERT INTO pljs.modules (path, bytecode) VALUES ($1, $2)',
    ['fast-cjs', cjsBc]
  );
$$;

CREATE OR REPLACE FUNCTION test_bytecode_import(x int, y int) RETURNS int LANGUAGE pljs AS $$
  import { fastAdd } from 'fast-mod@1.0.0';
  const cjs = require('fast-cjs');
  return fastAdd(x, y) + cjs.fastMul(x, 2);
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
    fromSub as fromSub
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

-- 10. Table ACLs, Row-Level Security (RLS), and non-superuser bytecode/untrusted restrictions
GRANT SELECT, INSERT, UPDATE, DELETE ON pljs.modules TO pljs_unpriv_tle_user;

-- Pre-populate restricted modules as superuser to test restricted module and delete protection
INSERT INTO pljs.modules (path, bytecode) VALUES ('restricted_bc', E'\\x0500');
INSERT INTO pljs.modules (path, source, is_trusted) VALUES ('restricted_untrusted', 'export const x = 1;', false);

SET ROLE pljs_unpriv_tle_user;
DO $$
BEGIN
  -- Happy paths (Positive DML tests)
  BEGIN
    -- Unprivileged user can insert a standard text module
    INSERT INTO pljs.modules (path, source, is_trusted) VALUES ('unpriv_standard', 'export const a = 2;', true);
    -- Unprivileged user can update a standard text module
    UPDATE pljs.modules SET source = 'export const a = 3;' WHERE path = 'unpriv_standard';
    -- Unprivileged user can delete a standard text module
    DELETE FROM pljs.modules WHERE path = 'unpriv_standard';
  EXCEPTION WHEN OTHERS THEN
    RAISE EXCEPTION 'Failed to execute allowed DML operations on standard module: %', SQLERRM;
  END;

  -- Verify packages table ACLs correctly prevent unauthorized inserts
  BEGIN
    INSERT INTO pljs.packages (name, version, entry_point) VALUES ('unpriv_pkg', '1.0.0', 'index.js');
    RAISE EXCEPTION 'Expected packages INSERT privilege rejection';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected packages INSERT privilege rejection';
  END;

  -- Negative tests (Negative DML tests)
  BEGIN
    EXECUTE $q$DO LANGUAGE pljs $js$ pljs.compile_bytecode('export const x = 1;'); $js$ $q$;
    RAISE EXCEPTION 'Expected compile_bytecode privilege rejection';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected compile_bytecode privilege rejection';
  END;
  BEGIN
    INSERT INTO pljs.modules (path, bytecode) VALUES ('unpriv_bc', E'\\x0500');
    RAISE EXCEPTION 'Expected bytecode INSERT privilege rejection';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected bytecode INSERT privilege rejection';
  END;
  BEGIN
    INSERT INTO pljs.modules (path, source, is_trusted) VALUES ('unpriv_untrusted', 'export const x = 1;', false);
    RAISE EXCEPTION 'Expected is_trusted=false INSERT privilege rejection';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected is_trusted=false INSERT privilege rejection';
  END;

  -- Test updating the new row to restricted
  BEGIN
    UPDATE pljs.modules SET is_trusted = false WHERE path = 'pljs_native_esm';
    RAISE EXCEPTION 'Expected is_trusted=false UPDATE privilege rejection';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected is_trusted=false UPDATE privilege rejection';
  END;

  -- Test Privilege Restrictions: Modifying an existing restricted module
  BEGIN
    UPDATE pljs.modules SET bytecode = NULL, source = 'export const x = "benign";' WHERE path = 'restricted_bc';
    RAISE EXCEPTION 'Expected restricted UPDATE privilege rejection for bytecode';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected restricted UPDATE privilege rejection for bytecode';
  END;

  BEGIN
    UPDATE pljs.modules SET is_trusted = true, source = 'export const x = "benign";' WHERE path = 'restricted_untrusted';
    RAISE EXCEPTION 'Expected restricted UPDATE privilege rejection for untrusted';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected restricted UPDATE privilege rejection for untrusted';
  END;

  -- Test Privilege Restrictions: Deleting an existing restricted module
  BEGIN
    DELETE FROM pljs.modules WHERE path = 'restricted_bc';
    RAISE EXCEPTION 'Expected DELETE privilege rejection for bytecode';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected DELETE privilege rejection for bytecode';
  END;

  BEGIN
    DELETE FROM pljs.modules WHERE path = 'restricted_untrusted';
    RAISE EXCEPTION 'Expected DELETE privilege rejection for untrusted';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected DELETE privilege rejection for untrusted';
  END;

  -- Test Multi-row UPDATE to ensure the transaction aborts when modifying a restricted row
  BEGIN
    UPDATE pljs.modules SET source = 'export const a = 4;' WHERE path IN ('unpriv_standard', 'restricted_untrusted');
    RAISE EXCEPTION 'Expected multi-row UPDATE to fail because it hits a restricted module';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected multi-row UPDATE privilege rejection';
  END;

  -- Test Multi-row DELETE
  BEGIN
    DELETE FROM pljs.modules WHERE path IN ('unpriv_standard', 'restricted_bc');
    RAISE EXCEPTION 'Expected multi-row DELETE to fail because it hits a restricted module';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected multi-row DELETE privilege rejection';
  END;
END;
$$;
RESET ROLE;
REVOKE INSERT, UPDATE, DELETE ON pljs.modules FROM pljs_unpriv_tle_user;

-- Revoking SELECT on pljs.modules from PUBLIC and unprivileged role blocks module loading
REVOKE SELECT ON pljs.modules FROM PUBLIC, pljs_unpriv_tle_user;
GRANT EXECUTE ON FUNCTION test_pljs_native_static_import(int, int) TO pljs_unpriv_tle_user;
SET ROLE pljs_unpriv_tle_user;
DO $$
BEGIN
  BEGIN
    PERFORM test_pljs_native_static_import(2, 3);
    RAISE EXCEPTION 'Expected table ACL rejection';
  EXCEPTION WHEN insufficient_privilege THEN
    RAISE NOTICE 'Caught expected table ACL rejection on pljs.modules';
  END;
END;
$$;
RESET ROLE;
GRANT SELECT ON pljs.modules TO PUBLIC;

-- Row-Level Security (RLS) policy on pljs.modules filters visible modules for non-superusers
INSERT INTO pljs.modules (path, source) VALUES
  ('rls_hidden_mod', 'export function hiddenVal() { return 99; }');
ALTER TABLE pljs.modules ENABLE ROW LEVEL SECURITY;
CREATE POLICY pljs_modules_rls_pol ON pljs.modules
  FOR SELECT USING (path <> 'rls_hidden_mod');

CREATE OR REPLACE FUNCTION test_rls_hidden() RETURNS int LANGUAGE pljs AS $$
  import { hiddenVal } from 'rls_hidden_mod';
  return hiddenVal();
$$;
GRANT EXECUTE ON FUNCTION test_rls_hidden() TO pljs_unpriv_tle_user;

-- Superuser bypasses RLS and sees rls_hidden_mod
SELECT test_rls_hidden();

-- Non-superuser is subject to RLS: rls_hidden_mod is filtered out, while pljs_native_esm is allowed
SET ROLE pljs_unpriv_tle_user;
SELECT test_pljs_native_static_import(6, 7);
DO $$
BEGIN
  BEGIN
    PERFORM test_rls_hidden();
    RAISE EXCEPTION 'Expected RLS to hide rls_hidden_mod';
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'Caught expected RLS hidden module error';
  END;
END;
$$;
RESET ROLE;

DROP POLICY pljs_modules_rls_pol ON pljs.modules;
ALTER TABLE pljs.modules DISABLE ROW LEVEL SECURITY;
DROP FUNCTION test_rls_hidden();

-- 11. Bytecode fallback to source on invalid/incompatible bytecode, automatic
--     clearing of stale bytecode when source is updated, and relative import
--     resolution when bytecode was compiled with a different module name
INSERT INTO pljs.modules (path, source, bytecode) VALUES
  ('bc_fallback_hdr', 'export function fbHdr() { return 101; }', E'\\x00'),
  ('bc_fallback_ver', 'export function fbVer() { return 202; }', E'\\xff0100'),
  ('bc_no_source_bad', NULL, E'\\xff0100');

CREATE OR REPLACE FUNCTION test_bc_fallback() RETURNS int[] LANGUAGE pljs AS $$
  import { fbHdr } from 'bc_fallback_hdr';
  import { fbVer } from 'bc_fallback_ver';
  return [fbHdr(), fbVer()];
$$;

SELECT test_bc_fallback();

-- When source IS NULL, invalid bytecode still fails
DO $$
BEGIN
  BEGIN
    CREATE OR REPLACE FUNCTION test_bc_no_src() RETURNS int LANGUAGE pljs AS $fn$
      import { x } from 'bc_no_source_bad';
      return x;
    $fn$;
    PERFORM test_bc_no_src();
    RAISE EXCEPTION 'Expected invalid bytecode without source to fail';
  EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'Caught expected invalid bytecode error when source is NULL';
  END;
END;
$$;
DROP FUNCTION IF EXISTS test_bc_no_src();

-- Updating source without specifying bytecode clears stale bytecode automatically
DO LANGUAGE pljs $$
  const src = 'export function staleCheck() { return 10; }';
  const bc = pljs.compile_bytecode(src, 'stale_bc_mod');
  pljs.execute(
    'INSERT INTO pljs.modules (path, source, bytecode) VALUES ($1, $2, $3)',
    ['stale_bc_mod', src, bc]
  );
$$;

CREATE OR REPLACE FUNCTION test_stale_bc() RETURNS int LANGUAGE pljs AS $$
  import { staleCheck } from 'stale_bc_mod';
  return staleCheck();
$$;

SELECT test_stale_bc();

UPDATE pljs.modules
SET source = 'export function staleCheck() { return 20; }'
WHERE path = 'stale_bc_mod';

SELECT bytecode IS NULL AS bytecode_cleared FROM pljs.modules WHERE path = 'stale_bc_mod';
SELECT test_stale_bc();

-- Also verify INSERT ... ON CONFLICT DO UPDATE SET source = EXCLUDED.source clears stale bytecode
DO LANGUAGE pljs $$
  const src = 'export function staleCheck() { return 25; }';
  const bc = pljs.compile_bytecode(src, 'stale_bc_mod');
  pljs.execute(
    'UPDATE pljs.modules SET source = $1, bytecode = $2 WHERE path = $3',
    [src, bc, 'stale_bc_mod']
  );
$$;
SELECT test_stale_bc();

INSERT INTO pljs.modules (path, source)
VALUES ('stale_bc_mod', 'export function staleCheck() { return 30; }')
ON CONFLICT (path) WHERE package_name IS NULL
DO UPDATE SET source = EXCLUDED.source;

SELECT bytecode IS NULL AS upsert_bytecode_cleared FROM pljs.modules WHERE path = 'stale_bc_mod';
SELECT test_stale_bc();
DROP FUNCTION test_stale_bc();

-- Bytecode compiled with a mismatched module name resolves relative imports and
-- requires against its actual storage location in pljs.modules
DO LANGUAGE pljs $$
  const bc = pljs.compile_bytecode(
    'import { doubleLeaf } from "../helper.js"; export function fromRelBc(x) { const h = require("../helper.js"); return doubleLeaf(x) + h.doubleLeaf(x); }',
    'wrong/other/dir/mod.js'
  );
  pljs.execute(
    'INSERT INTO pljs.modules (path, bytecode, requires, is_trusted) VALUES ($1, $2, $3, true)',
    ['pkg/sub/rel_bc.js', bc, ['pkg/helper.js']]
  );
$$;

CREATE OR REPLACE FUNCTION test_relocated_bc(x int) RETURNS int LANGUAGE pljs AS $$
  import { fromRelBc } from 'pkg/sub/rel_bc.js';
  return fromRelBc(x);
$$;

SELECT test_relocated_bc(5);

DROP FUNCTION test_relocated_bc(int);
DROP FUNCTION test_bc_fallback();
DROP FUNCTION test_pljs_native_static_import(int, int);
DROP FUNCTION test_esm_singleton();
DROP FUNCTION test_untrusted_tle();
DROP FUNCTION test_rel_and_semi(int);
DROP ROLE pljs_unpriv_tle_user;
DELETE FROM pljs.modules;
DELETE FROM pljs.packages;
