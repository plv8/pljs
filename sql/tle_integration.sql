-- Test cross-extension integration between pg_tle and pljs
CREATE EXTENSION IF NOT EXISTS pg_tle;
CREATE EXTENSION IF NOT EXISTS pljs;

-- 1. Install modules in pljs.packages / pljs.modules with pljs.compile_bytecode()
INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES
  ('tle-math', '1.0.0', 'index.js', '{}'),
  ('tle-geom', '1.0.0', 'index.js', '{"tle-math": "^1.0.0"}');

INSERT INTO pljs.modules (path, package_name, package_version, source) VALUES
  ('index.js', 'tle-math', '1.0.0', 'export function hypot(a, b) { return Math.sqrt(a * a + b * b); }');

DO LANGUAGE pljs $$
  const src = 'import { hypot } from "tle-math@1.0.0"; export function dist(x1, y1, x2, y2) { return hypot(x2 - x1, y2 - y1); }';
  const bc = pljs.compile_bytecode(src, 'tle-geom@1.0.0');
  pljs.execute(
    'INSERT INTO pljs.modules (path, package_name, package_version, source, bytecode) VALUES ($1, $2, $3, $4, $5)',
    ['index.js', 'tle-geom', '1.0.0', src, bc]
  );
$$;

-- 2. Verify resolution of bytecode module
CREATE OR REPLACE FUNCTION pljs_tle_dist(x1 float8, y1 float8, x2 float8, y2 float8)
RETURNS float8 LANGUAGE pljs AS $$
  import { dist } from 'tle:tle-geom@1.0.0';
  return dist(x1, y1, x2, y2);
$$;

SELECT pljs_tle_dist(0, 0, 3, 4);
SELECT pljs_tle_dist(1, 2, 4, 6);

-- 3. Verify module update and cache invalidation in pljs.modules
UPDATE pljs.modules
SET source = 'import { hypot } from "tle-math@1.0.0"; export function dist(x1, y1, x2, y2) { return hypot(x2 - x1, y2 - y1) * 10; }',
    bytecode = NULL
WHERE package_name = 'tle-geom' AND package_version = '1.0.0' AND path = 'index.js';

SELECT pljs_tle_dist(0, 0, 3, 4);

DROP FUNCTION pljs_tle_dist(float8, float8, float8, float8);
DELETE FROM pljs.modules;
DELETE FROM pljs.packages;
DROP EXTENSION pg_tle CASCADE;
