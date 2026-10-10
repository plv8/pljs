-- Upgrade from a 1.0.x release.
--
-- Every 1.0.x release has the same SQL objects, so this one script is used for
-- all of them; the Makefile makes a copy for each release that can be upgraded
-- from.  Keep it in step with pljs.sql: the upgrade regression test compares
-- what it leaves behind with a fresh install.

CREATE OR REPLACE FUNCTION pljs_reset() RETURNS void
 AS 'MODULE_PATHNAME', 'pljs_reset'
 LANGUAGE C;

CREATE SCHEMA pljs;

CREATE TABLE pljs.packages (
  name TEXT NOT NULL,
  version TEXT NOT NULL,
  entry_point TEXT NOT NULL,
  dependencies JSONB DEFAULT '{}',
  created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE,
  PRIMARY KEY (name, version)
);

CREATE TABLE pljs.modules (
  path TEXT NOT NULL,
  source TEXT,
  package_name TEXT,
  package_version TEXT,
  created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE,
  -- A CommonJS module that needs sloppy mode (implicit globals, `with`, octal
  -- literals) rather than the strict mode an ES module imposes.  Has no effect
  -- on an ES module, which is always strict.
  sloppy BOOLEAN NOT NULL DEFAULT false,
  FOREIGN KEY (package_name, package_version) REFERENCES pljs.packages (name, version)
);

-- What is installed in these tables is the user's, not the extension's, so a
-- dump has to include it: without this, pg_dump skips the tables' contents and
-- a restore leaves them empty.
SELECT pg_catalog.pg_extension_config_dump('pljs.packages', '');
SELECT pg_catalog.pg_extension_config_dump('pljs.modules', '');

CREATE UNIQUE INDEX pljs_modules_standalone_path ON pljs.modules (path)
  WHERE package_name IS NULL;
CREATE UNIQUE INDEX pljs_modules_package_path ON pljs.modules (package_name, package_version, path)
  WHERE package_name IS NOT NULL;

-- Non-partial indexes for catalog scans (systable_beginscan_ordered).
CREATE INDEX pljs_modules_path ON pljs.modules (path);
CREATE INDEX pljs_modules_pkg ON pljs.modules (package_name, package_version, path);
CREATE INDEX pljs_packages_name ON pljs.packages (name);

-- Changing the module tables invalidates every backend's loaded modules: this
-- trigger makes the change visible to them (see src/modules.c).  It is ENABLE
-- ALWAYS so that replication and restores keep invalidating too.
CREATE OR REPLACE FUNCTION pljs_modules_changed() RETURNS trigger
 AS 'MODULE_PATHNAME', 'pljs_modules_changed'
 LANGUAGE C;

CREATE TRIGGER pljs_modules_changed
  AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON pljs.modules
  FOR EACH STATEMENT EXECUTE FUNCTION pljs_modules_changed();
ALTER TABLE pljs.modules ENABLE ALWAYS TRIGGER pljs_modules_changed;

CREATE TRIGGER pljs_modules_changed
  AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON pljs.packages
  FOR EACH STATEMENT EXECUTE FUNCTION pljs_modules_changed();
ALTER TABLE pljs.packages ENABLE ALWAYS TRIGGER pljs_modules_changed;
