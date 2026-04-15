-- Test hooks GUC infrastructure
-- The extension (and GUCs) are loaded by init-extension.sql which runs first.

-- hooks_enabled defaults to off
-- We use SET to test since SHOW only works for preloaded libraries
SET pljs.hooks_enabled = false;

-- Force pljs library load so GUC check callbacks are registered.
-- Without this, SET for unknown dotted GUCs stores placeholders
-- with no permission checks.
DO $$ $$ LANGUAGE pljs;

-- Non-superuser cannot set hooks_enabled (check callback enforces)
CREATE ROLE hooks_test_role;
SET SESSION AUTHORIZATION hooks_test_role;
SET pljs.hooks_enabled = true;
RESET SESSION AUTHORIZATION;

-- Non-superuser cannot set hook function GUCs either
SET SESSION AUTHORIZATION hooks_test_role;
SET pljs.executor_start_hook = 'malicious_function';
RESET SESSION AUTHORIZATION;
DROP ROLE hooks_test_role;

-- Enable hooks
SET pljs.hooks_enabled = true;

-- Create a test table used by several hooks
CREATE TABLE hooks_test (id serial PRIMARY KEY, val text);

-- ============================================================
-- ExecutorStart hook
-- ============================================================
CREATE FUNCTION hook_executor_start(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'executor_start: ' + arguments[0].operation);
$$ LANGUAGE pljs;

SET pljs.executor_start_hook = 'hook_executor_start';
INSERT INTO hooks_test (val) VALUES ('start_test');
SET pljs.executor_start_hook = '';

-- ============================================================
-- ExecutorRun hook
-- ============================================================
CREATE FUNCTION hook_executor_run(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'executor_run: ' + arguments[0].operation + ' dir=' + arguments[0].direction);
$$ LANGUAGE pljs;

SET pljs.executor_run_hook = 'hook_executor_run';
SELECT val FROM hooks_test WHERE val = 'start_test';
SET pljs.executor_run_hook = '';

-- ============================================================
-- ExecutorEnd hook
-- ============================================================
CREATE FUNCTION hook_executor_end(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'executor_end: ' + arguments[0].operation);
$$ LANGUAGE pljs;

SET pljs.executor_end_hook = 'hook_executor_end';
SELECT 1 as test_end;
SET pljs.executor_end_hook = '';

-- ============================================================
-- Planner hook
-- ============================================================
CREATE FUNCTION hook_planner(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'planner: ' + arguments[0].operation);
$$ LANGUAGE pljs;

SET pljs.planner_hook = 'hook_planner';
SELECT 1 as test_planner;
SET pljs.planner_hook = '';

-- ============================================================
-- emit_log hook - verify it doesn't crash (can't elog from inside)
-- ============================================================
CREATE FUNCTION hook_emit_log(jsonb) RETURNS void AS $$
  // Silently observe. The test verifies we don't crash.
$$ LANGUAGE pljs;

SET pljs.emit_log_hook = 'hook_emit_log';
DO $$ pljs.elog(NOTICE, 'emit_log test') $$ LANGUAGE pljs;
SET pljs.emit_log_hook = '';

-- ============================================================
-- needs_fmgr hook - fires for user-defined function calls
-- ============================================================
CREATE FUNCTION test_simple() RETURNS int AS $$ return 42; $$ LANGUAGE pljs;

CREATE FUNCTION hook_needs_fmgr(int8) RETURNS boolean AS $$
  pljs.elog(NOTICE, 'needs_fmgr called');
  return false;
$$ LANGUAGE pljs;

SET pljs.needs_fmgr_hook = 'hook_needs_fmgr';
SELECT test_simple();
SET pljs.needs_fmgr_hook = '';

-- ============================================================
-- fmgr hook - needs needs_fmgr to return true to fire
-- ============================================================
CREATE FUNCTION hook_needs_fmgr_true(int8) RETURNS boolean AS $$
  return true;
$$ LANGUAGE pljs;

CREATE FUNCTION hook_fmgr(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'fmgr: ' + arguments[0].event);
$$ LANGUAGE pljs;

SET pljs.needs_fmgr_hook = 'hook_needs_fmgr_true';
SET pljs.fmgr_hook = 'hook_fmgr';
SELECT test_simple();
SET pljs.fmgr_hook = '';
SET pljs.needs_fmgr_hook = '';

-- ============================================================
-- get_relation_info hook
-- ============================================================
CREATE FUNCTION hook_get_relation_info(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'get_relation_info: inhparent=' + arguments[0].inhparent);
$$ LANGUAGE pljs;

SET pljs.get_relation_info_hook = 'hook_get_relation_info';
SELECT * FROM hooks_test;
SET pljs.get_relation_info_hook = '';

-- ============================================================
-- set_rel_pathlist hook
-- ============================================================
CREATE FUNCTION hook_set_rel_pathlist(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'set_rel_pathlist: rti=' + arguments[0].rti);
$$ LANGUAGE pljs;

SET pljs.set_rel_pathlist_hook = 'hook_set_rel_pathlist';
SELECT * FROM hooks_test;
SET pljs.set_rel_pathlist_hook = '';

-- ============================================================
-- create_upper_paths hook
-- ============================================================
CREATE FUNCTION hook_create_upper_paths(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'create_upper_paths: stage=' + arguments[0].stage);
$$ LANGUAGE pljs;

SET pljs.create_upper_paths_hook = 'hook_create_upper_paths';
SELECT * FROM hooks_test;
SET pljs.create_upper_paths_hook = '';

-- ============================================================
-- set_join_pathlist hook
-- ============================================================
CREATE TABLE hooks_test2 (id serial PRIMARY KEY, ref_id int);
INSERT INTO hooks_test2 (ref_id) VALUES (1);

CREATE FUNCTION hook_set_join_pathlist(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'set_join_pathlist: type=' + arguments[0].joinType);
$$ LANGUAGE pljs;

SET pljs.set_join_pathlist_hook = 'hook_set_join_pathlist';
SELECT * FROM hooks_test t1 JOIN hooks_test2 t2 ON t1.id = t2.ref_id;
SET pljs.set_join_pathlist_hook = '';

-- ============================================================
-- join_search hook
-- ============================================================
CREATE FUNCTION hook_join_search(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'join_search: levels=' + arguments[0].levelsNeeded + ' rels=' + arguments[0].initialRelsCount);
$$ LANGUAGE pljs;

SET pljs.join_search_hook = 'hook_join_search';
SELECT * FROM hooks_test t1 JOIN hooks_test2 t2 ON t1.id = t2.ref_id;
SET pljs.join_search_hook = '';

-- ============================================================
-- object_access hook
-- ============================================================
CREATE FUNCTION hook_object_access(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'object_access: ' + arguments[0].access);
$$ LANGUAGE pljs;

SET pljs.object_access_hook = 'hook_object_access';
CREATE TABLE hooks_oa_test (id int);
DROP TABLE hooks_oa_test;
SET pljs.object_access_hook = '';

-- ============================================================
-- object_access_hook_str
-- ============================================================
CREATE FUNCTION hook_object_access_str(jsonb) RETURNS void AS $$
  if (arguments[0].objectName) {
    pljs.elog(NOTICE, 'object_access_str: ' + arguments[0].access + ' name=' + arguments[0].objectName);
  }
$$ LANGUAGE pljs;

SET pljs.object_access_hook_str = 'hook_object_access_str';
CREATE TABLE hooks_oas_test (id int);
DROP TABLE hooks_oas_test;
SET pljs.object_access_hook_str = '';

-- ============================================================
-- Recursion: hook itself calls pljs.execute(), triggering the
-- same hook again. The hook guards against infinite recursion
-- by checking the query text.
-- ============================================================
CREATE FUNCTION hook_planner_recursive(jsonb) RETURNS void AS $$
  var q = arguments[0].queryString || '';
  pljs.elog(NOTICE, 'planner hook: ' + arguments[0].operation);
  // Only recurse on the outer query, not on our own inner SELECT
  if (q.indexOf('recurse_test') >= 0) {
    pljs.elog(NOTICE, 'planner hook: executing inner query');
    pljs.execute('SELECT 1');
    pljs.elog(NOTICE, 'planner hook: inner query done');
  }
$$ LANGUAGE pljs;

SET pljs.planner_hook = 'hook_planner_recursive';
SELECT 1 as recurse_test;
SET pljs.planner_hook = '';

-- ============================================================
-- Recursion: executor_run hook calls pljs.execute() inside,
-- triggering itself recursively with a user-managed guard.
-- ============================================================
CREATE FUNCTION hook_executor_run_recursive(jsonb) RETURNS void AS $$
  var src = arguments[0].sourceText || '';
  pljs.elog(NOTICE, 'executor_run: ' + arguments[0].operation);
  if (src.indexOf('recurse_run_test') >= 0) {
    pljs.elog(NOTICE, 'executor_run: executing inner query');
    pljs.execute('SELECT 1');
    pljs.elog(NOTICE, 'executor_run: inner query done');
  }
$$ LANGUAGE pljs;

SET pljs.executor_run_hook = 'hook_executor_run_recursive';
SELECT 1 as recurse_run_test;
SET pljs.executor_run_hook = '';

-- ============================================================
-- Recursion depth limit: a hook that always recurses hits the
-- pljs.hooks_max_depth limit (default 5) and produces a WARNING.
-- ============================================================
CREATE FUNCTION hook_infinite_recurse(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'depth call');
  pljs.execute('SELECT 1');
$$ LANGUAGE pljs;

SET pljs.executor_start_hook = 'hook_infinite_recurse';
SELECT 1 as depth_limit_test;
SET pljs.executor_start_hook = '';

-- Verify the GUC can be changed: set to 2 and re-test
SET pljs.hooks_max_depth = 2;
SET pljs.executor_start_hook = 'hook_infinite_recurse';
SELECT 1 as depth_limit_2_test;
SET pljs.executor_start_hook = '';
SET pljs.hooks_max_depth = 5;

-- ============================================================
-- Coverage: executor_start with UPDATE, DELETE operations
-- (covers CMD_UPDATE, CMD_DELETE in cmdtype_to_string)
-- ============================================================
SET pljs.executor_start_hook = 'hook_executor_start';
UPDATE hooks_test SET val = 'updated' WHERE id = 1;
DELETE FROM hooks_test WHERE val = 'updated';
INSERT INTO hooks_test (val) VALUES ('restored');
SET pljs.executor_start_hook = '';

-- ============================================================
-- Coverage: emit_log hook actually fires
-- (covers pljs_errordata_to_jsvalue, error_severity_to_string,
--  and the emit_log hook body)
-- ============================================================
CREATE FUNCTION hook_emit_log_verbose(jsonb) RETURNS void AS $$
  // This runs inside the error reporting system.
  // We cannot elog from here, just silently process.
$$ LANGUAGE pljs;

SET pljs.emit_log_hook = 'hook_emit_log_verbose';
-- Generate different log levels to cover error_severity_to_string branches
DO $$ pljs.elog(NOTICE, 'emit test notice') $$ LANGUAGE pljs;
DO $$ pljs.elog(WARNING, 'emit test warning') $$ LANGUAGE pljs;
DO $$ pljs.elog(INFO, 'emit test info') $$ LANGUAGE pljs;
SET pljs.emit_log_hook = '';

-- ============================================================
-- Coverage: create_upper_paths with GROUP BY, DISTINCT, ORDER BY
-- (covers UPPERREL_GROUP_AGG, UPPERREL_DISTINCT, UPPERREL_ORDERED
--  in upperrelkind_to_string)
-- ============================================================
SET pljs.create_upper_paths_hook = 'hook_create_upper_paths';
SELECT DISTINCT val FROM hooks_test ORDER BY val;
SELECT val, count(*) FROM hooks_test GROUP BY val;
SET pljs.create_upper_paths_hook = '';

-- ============================================================
-- Coverage: LEFT JOIN, RIGHT JOIN
-- (covers JOIN_LEFT, JOIN_RIGHT in jointype_to_string)
-- ============================================================
SET pljs.set_join_pathlist_hook = 'hook_set_join_pathlist';
SELECT * FROM hooks_test t1 LEFT JOIN hooks_test2 t2 ON t1.id = t2.ref_id;
SELECT * FROM hooks_test t1 RIGHT JOIN hooks_test2 t2 ON t1.id = t2.ref_id;
SET pljs.set_join_pathlist_hook = '';

-- ============================================================
-- Coverage: object_access with TRUNCATE
-- (covers OAT_TRUNCATE in objectaccess_to_string)
-- ============================================================
CREATE TABLE hooks_trunc_test (id int);
INSERT INTO hooks_trunc_test VALUES (1);
SET pljs.object_access_hook = 'hook_object_access';
TRUNCATE hooks_trunc_test;
SET pljs.object_access_hook = '';
DROP TABLE hooks_trunc_test;

-- ============================================================
-- Coverage: object_access with function execute
-- (covers OAT_FUNCTION_EXECUTE in objectaccess_to_string)
-- ============================================================
SET pljs.object_access_hook = 'hook_object_access';
SELECT test_simple();
SET pljs.object_access_hook = '';

-- ============================================================
-- Coverage: depth limits on planner and executor_end hooks
-- (covers depth warning branches for more hook types)
-- ============================================================
CREATE FUNCTION hook_planner_infinite(jsonb) RETURNS void AS $$
  pljs.execute('SELECT 1');
$$ LANGUAGE pljs;

SET pljs.hooks_max_depth = 2;
SET pljs.planner_hook = 'hook_planner_infinite';
SELECT 1 as planner_depth_test;
SET pljs.planner_hook = '';

CREATE FUNCTION hook_executor_end_infinite(jsonb) RETURNS void AS $$
  pljs.execute('SELECT 1');
$$ LANGUAGE pljs;

SET pljs.executor_end_hook = 'hook_executor_end_infinite';
SELECT 1 as executor_end_depth_test;
SET pljs.executor_end_hook = '';
SET pljs.hooks_max_depth = 5;

-- ============================================================
-- Coverage: error-throwing hooks on different hook types
-- (covers PG_CATCH paths for planner, executor_run, executor_end)
-- ============================================================
CREATE FUNCTION hook_throws(jsonb) RETURNS void AS $$
  throw new Error('intentional hook error');
$$ LANGUAGE pljs;

SET pljs.planner_hook = 'hook_throws';
SELECT 1 as planner_error_test;
SET pljs.planner_hook = '';

SET pljs.executor_run_hook = 'hook_throws';
SELECT 1 as executor_run_error_test;
SET pljs.executor_run_hook = '';

SET pljs.executor_end_hook = 'hook_throws';
SELECT 1 as executor_end_error_test;
SET pljs.executor_end_hook = '';

SET pljs.get_relation_info_hook = 'hook_throws';
SELECT * FROM hooks_test LIMIT 1;
SET pljs.get_relation_info_hook = '';

SET pljs.set_rel_pathlist_hook = 'hook_throws';
SELECT * FROM hooks_test LIMIT 1;
SET pljs.set_rel_pathlist_hook = '';

SET pljs.create_upper_paths_hook = 'hook_throws';
SELECT * FROM hooks_test LIMIT 1;
SET pljs.create_upper_paths_hook = '';

SET pljs.object_access_hook = 'hook_throws';
CREATE TABLE hooks_err_test (id int);
DROP TABLE hooks_err_test;
SET pljs.object_access_hook = '';

-- Invalid function name triggers PG_CATCH in resolve
SET pljs.executor_start_hook = 'nonexistent_hook_function';
SELECT 1 as invalid_func_test;
SET pljs.executor_start_hook = '';

SET pljs.planner_hook = 'nonexistent_hook_function';
SELECT 1 as invalid_planner_test;
SET pljs.planner_hook = '';

SET pljs.set_join_pathlist_hook = 'nonexistent_hook_function';
SELECT * FROM hooks_test t1 JOIN hooks_test2 t2 ON t1.id = t2.ref_id;
SET pljs.set_join_pathlist_hook = '';

SET pljs.join_search_hook = 'nonexistent_hook_function';
SELECT * FROM hooks_test t1 JOIN hooks_test2 t2 ON t1.id = t2.ref_id;
SET pljs.join_search_hook = '';

-- ============================================================
-- Coverage: FULL JOIN (covers JOIN_FULL in jointype_to_string)
-- ============================================================
SET pljs.set_join_pathlist_hook = 'hook_set_join_pathlist';
SELECT * FROM hooks_test t1 FULL JOIN hooks_test2 t2 ON t1.id = t2.ref_id;
SET pljs.set_join_pathlist_hook = '';

-- ============================================================
-- Coverage: WINDOW function stage
-- (covers UPPERREL_WINDOW in upperrelkind_to_string)
-- ============================================================
SET pljs.create_upper_paths_hook = 'hook_create_upper_paths';
SELECT val, row_number() OVER (ORDER BY val) FROM hooks_test;
SET pljs.create_upper_paths_hook = '';

-- ============================================================
-- Coverage: function name with parentheses
-- (covers regprocedurein branch in pljs_resolve_hook_function)
-- ============================================================
SET pljs.executor_start_hook = 'hook_executor_start(jsonb)';
SELECT 1 as parens_test;
SET pljs.executor_start_hook = '';

-- ============================================================
-- Error handling: hook function that throws should not crash
-- ============================================================
SET pljs.executor_start_hook = 'hook_throws';
-- This should produce a WARNING but still execute successfully
SELECT 1 as error_handling_test;
SET pljs.executor_start_hook = '';

-- ============================================================
-- Hooks disabled: setting hooks_enabled=false should stop all hooks
-- ============================================================
CREATE FUNCTION hook_should_not_fire(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'THIS SHOULD NOT APPEAR');
$$ LANGUAGE pljs;

SET pljs.executor_start_hook = 'hook_should_not_fire';
SET pljs.hooks_enabled = false;
-- This should NOT produce any NOTICE from the hook
SELECT 1 as hooks_disabled_test;
SET pljs.hooks_enabled = true;
SET pljs.executor_start_hook = '';

-- ============================================================
-- Cleanup
-- ============================================================
SET pljs.hooks_enabled = false;

DROP TABLE hooks_test2;
DROP TABLE hooks_test;
DROP FUNCTION test_simple();

DROP FUNCTION hook_executor_start(jsonb);
DROP FUNCTION hook_executor_run(jsonb);
DROP FUNCTION hook_executor_end(jsonb);
DROP FUNCTION hook_planner(jsonb);
DROP FUNCTION hook_emit_log(jsonb);
DROP FUNCTION hook_needs_fmgr(int8);
DROP FUNCTION hook_needs_fmgr_true(int8);
DROP FUNCTION hook_fmgr(jsonb);
DROP FUNCTION hook_get_relation_info(jsonb);
DROP FUNCTION hook_set_rel_pathlist(jsonb);
DROP FUNCTION hook_create_upper_paths(jsonb);
DROP FUNCTION hook_set_join_pathlist(jsonb);
DROP FUNCTION hook_join_search(jsonb);
DROP FUNCTION hook_object_access(jsonb);
DROP FUNCTION hook_object_access_str(jsonb);
DROP FUNCTION hook_planner_recursive(jsonb);
DROP FUNCTION hook_executor_run_recursive(jsonb);
DROP FUNCTION hook_infinite_recurse(jsonb);
DROP FUNCTION hook_emit_log_verbose(jsonb);
DROP FUNCTION hook_planner_infinite(jsonb);
DROP FUNCTION hook_executor_end_infinite(jsonb);
DROP FUNCTION hook_throws(jsonb);
DROP FUNCTION hook_should_not_fire(jsonb);
