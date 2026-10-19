-- ============================================================
-- Who may call pljs_info() and pljs_reset()
-- ============================================================
CREATE ROLE pljs_priv_user;

-- An ordinary role can use the language, and ask for the version.
SET ROLE pljs_priv_user;
SELECT pljs_version() IS NOT NULL AS can_ask_version;
DO $$ pljs.elog(INFO, 'an ordinary role can run pljs code'); $$ LANGUAGE pljs;

-- It cannot see the session's internals, or discard its contexts.
SELECT pljs_info();
SELECT pljs_reset();
RESET ROLE;

-- Nothing was reset by the attempt: state set by the superuser is still there.
DO $$ globalThis.priv_marker = 'still here'; $$ LANGUAGE pljs;
SET ROLE pljs_priv_user;
SELECT pljs_reset();
RESET ROLE;
DO $$ pljs.elog(INFO, String(globalThis.priv_marker)); $$ LANGUAGE pljs;

-- It can be allowed to.
GRANT EXECUTE ON FUNCTION pljs_info(), pljs_reset() TO pljs_priv_user;
SET ROLE pljs_priv_user;
SELECT pljs_info() IS NOT NULL AS can_see_info;
SELECT pljs_reset();
RESET ROLE;

-- The superuser can, with nothing granted.
REVOKE EXECUTE ON FUNCTION pljs_info(), pljs_reset() FROM pljs_priv_user;
SELECT pljs_info() IS NOT NULL AS superuser_can_see_info;
SELECT pljs_reset();

DROP ROLE pljs_priv_user;
