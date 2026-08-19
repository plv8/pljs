-- Creating or replacing one function must not throw away every user's compiled
-- state.  The previous behaviour was a full pljs_cache_reset(), which destroys the
-- per-user JSContext -- and JS_FreeContext() will not free a context that still
-- has live references, so a backend doing repeated DDL grew without bound.
--
-- Whether the context survived is observable from JavaScript: anything held on
-- globalThis lives in that context, so it is still there after unrelated DDL
-- exactly when the context was not destroyed.
CREATE FUNCTION tin_set() RETURNS void AS $$
  globalThis.tin_marker = 'kept';
$$ LANGUAGE pljs;

CREATE FUNCTION tin_get() RETURNS text AS $$
  return globalThis.tin_marker || '(gone)';
$$ LANGUAGE pljs;

SELECT tin_set();
SELECT tin_get() AS before_ddl;

-- DDL on an unrelated function.
CREATE FUNCTION tin_other() RETURNS int AS $$ return 1; $$ LANGUAGE pljs;

SELECT tin_get() AS after_unrelated_create;

CREATE OR REPLACE FUNCTION tin_other() RETURNS int AS $$ return 2; $$ LANGUAGE pljs;

SELECT tin_get() AS after_unrelated_replace;

-- The replaced function itself does pick up its new body, which is the whole point
-- of invalidating anything at all.
SELECT tin_other() AS new_body;

DROP FUNCTION tin_set, tin_get, tin_other;
