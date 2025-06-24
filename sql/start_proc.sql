CREATE FUNCTION start() RETURNS void AS
$$
  pljs.elog(NOTICE, 'start function executed');
  pljs.current_scope = {
    "name": "current_scope"
  };
$$ LANGUAGE pljs;

SET pljs.start_proc = 'start';

DO $$ pljs.elog(NOTICE, pljs.current_scope.name); $$ language pljs;

-- setting a start proc after the context is created should change do anything
SET pljs.start_proc = 'end';

DO $$ pljs.elog(NOTICE, pljs.current_scope.name); $$ language pljs;

-- a missing start proc should cause an error
-- force the creation of a new context
CREATE ROLE new_context;
SET ROLE new_context;

SET pljs.start_proc = 'end';
DO $$ pljs.elog(NOTICE, pljs.current_scope.name); $$ language pljs;

RESET ROLE;
DROP ROLE new_context;

-- reset the start_proc
SET pljs.start_proc = '';

DROP FUNCTION start;

-- test startup permissions failure
CREATE FUNCTION start() RETURNS void AS $$ pljs.elog(NOTICE, 'nope'); $$ LANGUAGE pljs;

CREATE ROLE someone_else;

REVOKE EXECUTE ON FUNCTION start() FROM public;

SET pljs.start_proc = 'start';
REVOKE EXECUTE ON FUNCTION start() FROM public;

SET ROLE TO someone_else;
DO $$ pljs.elog(NOTICE, 'hello') $$ LANGUAGE pljs;

RESET ROLE;
DROP ROLE someone_else;

RESET pljs.start_proc;

DROP FUNCTION start;
