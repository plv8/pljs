CREATE OR REPLACE FUNCTION pljs_call_handler() RETURNS language_handler
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE FUNCTION pljs_call_validator(oid) RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE FUNCTION pljs_inline_handler(internal) RETURNS void
  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE OR REPLACE TRUSTED LANGUAGE pljs
 HANDLER pljs_call_handler
 INLINE pljs_inline_handler
 VALIDATOR pljs_call_validator;

CREATE OR REPLACE FUNCTION pljs_version()
 RETURNS TEXT
 AS 'MODULE_PATHNAME', 'pljs_version'
 LANGUAGE C STRICT IMMUTABLE;

CREATE OR REPLACE FUNCTION pljs_info() RETURNS JSON
 AS 'MODULE_PATHNAME', 'pljs_info'
 LANGUAGE C;

CREATE OR REPLACE FUNCTION pljs_reset() RETURNS void
 AS 'MODULE_PATHNAME', 'pljs_reset'
 LANGUAGE C;

-- These act on the whole session rather than on one function: pljs_info shows
-- its memory and configuration, and pljs_reset discards the cached contexts of
-- every user in it.  They are for superusers, or the roles they are granted to.
REVOKE ALL ON FUNCTION pljs_info() FROM PUBLIC;
REVOKE ALL ON FUNCTION pljs_reset() FROM PUBLIC;

CREATE SCHEMA IF NOT EXISTS pljs;
GRANT USAGE ON SCHEMA pljs TO PUBLIC;

CREATE TABLE IF NOT EXISTS pljs.packages (
  name TEXT NOT NULL,
  version TEXT NOT NULL,
  entry_point TEXT NOT NULL,
  dependencies JSONB DEFAULT '{}',
  created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMP WITH TIME ZONE,
  PRIMARY KEY (name, version)
);

CREATE TABLE IF NOT EXISTS pljs.modules (
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
  bytecode BYTEA,
  requires TEXT[],
  is_trusted BOOLEAN NOT NULL DEFAULT true,
  FOREIGN KEY (package_name, package_version) REFERENCES pljs.packages (name, version)
);

GRANT SELECT ON pljs.packages TO PUBLIC;
GRANT SELECT ON pljs.modules TO PUBLIC;

-- What is installed in these tables is the user's, not the extension's, so a
-- dump has to include it: without this, pg_dump skips the tables' contents and
-- a restore leaves them empty.
SELECT pg_catalog.pg_extension_config_dump('pljs.packages', '');
SELECT pg_catalog.pg_extension_config_dump('pljs.modules', '');

CREATE UNIQUE INDEX IF NOT EXISTS pljs_modules_standalone_path ON pljs.modules (path)
  WHERE package_name IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS pljs_modules_package_path ON pljs.modules (package_name, package_version, path)
  WHERE package_name IS NOT NULL;

-- Non-partial indexes for catalog scans (systable_beginscan_ordered).
CREATE INDEX IF NOT EXISTS pljs_modules_path ON pljs.modules (path);
CREATE INDEX IF NOT EXISTS pljs_modules_pkg ON pljs.modules (package_name, package_version, path);
CREATE INDEX IF NOT EXISTS pljs_packages_name ON pljs.packages (name);

-- Changing the module tables invalidates every backend's loaded modules: this
-- trigger makes the change visible to them (see src/modules.c).  It is ENABLE
-- ALWAYS so that replication and restores keep invalidating too.
CREATE OR REPLACE FUNCTION pljs_modules_changed() RETURNS trigger
 AS 'MODULE_PATHNAME', 'pljs_modules_changed'
 LANGUAGE C;

CREATE TRIGGER pljs_modules_guard
  BEFORE INSERT OR UPDATE OR DELETE ON pljs.modules
  FOR EACH ROW EXECUTE FUNCTION pljs_modules_changed();
ALTER TABLE pljs.modules ENABLE ALWAYS TRIGGER pljs_modules_guard;

CREATE TRIGGER pljs_modules_changed
  AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON pljs.modules
  FOR EACH STATEMENT EXECUTE FUNCTION pljs_modules_changed();
ALTER TABLE pljs.modules ENABLE ALWAYS TRIGGER pljs_modules_changed;

CREATE TRIGGER pljs_modules_changed
  AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON pljs.packages
  FOR EACH STATEMENT EXECUTE FUNCTION pljs_modules_changed();
ALTER TABLE pljs.packages ENABLE ALWAYS TRIGGER pljs_modules_changed;
