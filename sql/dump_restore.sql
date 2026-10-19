-- ============================================================
-- Modules and packages survive a dump and restore
-- ============================================================
-- Done in databases of their own, restored with the pg_dump on the path.
\set regression_db :DBNAME
CREATE DATABASE pljs_dump_source;
CREATE DATABASE pljs_dump_target;
\connect pljs_dump_source

CREATE EXTENSION pljs;
INSERT INTO pljs.packages (name, version, entry_point, dependencies) VALUES
  ('DumpDep', '1.0.0', 'index', '{}'),
  ('DumpApp', '1.0.0', 'index', '{"DumpDep": "^1.0.0"}');
INSERT INTO pljs.modules (path, source, package_name, package_version, sloppy) VALUES
  ('index', 'module.exports = "dep";', 'DumpDep', '1.0.0', false),
  ('index', 'implicit = "app+"; module.exports = implicit + pljs.require("DumpDep");', 'DumpApp', '1.0.0', true);
INSERT INTO pljs.modules (path, source) VALUES
  ('dump/standalone', 'export const v = "standalone";');

-- Both tables are in the dump, the packages first, as the modules refer to them.
\! pg_dump -d pljs_dump_source | grep '^COPY pljs\.' | sed 's/ (.*//'

\! pg_dump -d pljs_dump_source | psql -X -q -o /dev/null -d pljs_dump_target

\connect pljs_dump_target

SELECT name, version, entry_point, dependencies FROM pljs.packages ORDER BY name;
SELECT path, package_name, package_version, sloppy FROM pljs.modules
  ORDER BY package_name NULLS LAST, path;

-- What was restored works, and so does changing it afterwards.
DO $$
  pljs.elog(INFO, pljs.require('DumpApp'));
  pljs.elog(INFO, pljs.import('dump/standalone').v);
$$ LANGUAGE pljs;
UPDATE pljs.modules SET source = 'export const v = "changed";' WHERE path = 'dump/standalone';
DO $$ pljs.elog(INFO, pljs.import('dump/standalone').v); $$ LANGUAGE pljs;

\connect :regression_db
DROP DATABASE pljs_dump_source;
DROP DATABASE pljs_dump_target;
