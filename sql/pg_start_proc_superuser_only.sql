-- pljs.start_proc runs as whoever creates the JavaScript context, so a role
-- that is not a superuser must not be able to set it in any spelling.
DO $$ pljs.elog(NOTICE, 'loaded') $$ LANGUAGE pljs;

CREATE ROLE start_proc_owner;
CREATE DATABASE pljs_start_proc_owned OWNER start_proc_owner;
GRANT CREATE ON SCHEMA public TO start_proc_owner;

SET ROLE start_proc_owner;
SET pljs.start_proc = 'start';
SELECT set_config('pljs.start_proc', 'start', false);
ALTER DATABASE pljs_start_proc_owned SET pljs.start_proc = 'start';
ALTER ROLE start_proc_owner SET pljs.start_proc = 'start';
CREATE FUNCTION planted() RETURNS void AS $$ $$ LANGUAGE pljs
  SET pljs.start_proc = 'start';
RESET ROLE;

-- a superuser still can
SET pljs.start_proc = 'start';
SHOW pljs.start_proc;
RESET pljs.start_proc;

REVOKE CREATE ON SCHEMA public FROM start_proc_owner;
DROP DATABASE pljs_start_proc_owned;
DROP ROLE start_proc_owner;
