-- What the builtins know about the running call is not found through the
-- global `pljs` object.
--
-- return_next(), the window object and pljs.commit()'s guard read the running
-- call's set, window and state from storage that was kept as the opaque of the
-- global `pljs` object, which JavaScript can replace or delete.  Without it
-- the storage was NULL, and the next call dereferenced it and crashed the
-- backend, for any user of the trusted language.  A DO block and a trigger
-- never installed storage of their own, so return_next() from one crashed
-- too, or added rows to the set of whatever function had run it.

-- 1) A function that replaces pljs.
CREATE FUNCTION cs_plain() RETURNS int4 LANGUAGE pljs AS $$ return 42; $$;
CREATE FUNCTION cs_clobber() RETURNS int4 LANGUAGE pljs AS $$
  globalThis.cs_saved_pljs = pljs;
  globalThis.pljs = undefined;
  return 1;
$$;
SELECT cs_clobber();

-- The JSContext is kept for the session, so the functions after it run with
-- no pljs at all.
SELECT cs_plain();
DO LANGUAGE pljs $$ globalThis.pljs = cs_saved_pljs; $$;

-- 2) A DO block that deletes it.
DO LANGUAGE pljs $$
  globalThis.cs_saved_pljs = pljs;
  delete globalThis.pljs;
$$;
SELECT cs_plain();
DO LANGUAGE pljs $$ globalThis.pljs = cs_saved_pljs; $$;

-- 3) return_next() through a reference kept while pljs is gone.
CREATE FUNCTION cs_srf_clobber() RETURNS SETOF int4 LANGUAGE pljs AS $$
  const saved = pljs;
  globalThis.pljs = undefined;
  try {
    saved.return_next(1);
    saved.return_next(2);
  } finally {
    globalThis.pljs = saved;
  }
$$;
SELECT * FROM cs_srf_clobber();

-- 4) return_next() from a DO block and from a trigger.
DO LANGUAGE pljs $$
  try {
    pljs.return_next(1);
  } catch (e) {
    pljs.elog(NOTICE, 'DO: ' + e.message);
  }
$$;

CREATE TABLE cs_tbl (a int4);
CREATE FUNCTION cs_trig() RETURNS trigger LANGUAGE pljs AS $$
  try {
    pljs.return_next(1);
  } catch (e) {
    pljs.elog(NOTICE, 'trigger: ' + e.message);
  }
  return NEW;
$$;
CREATE TRIGGER cs_trig BEFORE INSERT ON cs_tbl
  FOR EACH ROW EXECUTE FUNCTION cs_trig();
INSERT INTO cs_tbl VALUES (1);

-- A DO block run by a set-returning function has no set of its own, and does
-- not add rows to the function's.
CREATE FUNCTION cs_srf_do() RETURNS SETOF int4 LANGUAGE pljs AS $$
  pljs.return_next(1);
  pljs.execute(`DO LANGUAGE pljs $d$
    try {
      pljs.return_next(2);
    } catch (e) {
      pljs.elog(NOTICE, 'nested DO: ' + e.message);
    }
  $d$`);
  pljs.return_next(3);
$$;
SELECT * FROM cs_srf_do();

-- 5) A window object used after its window function has returned.
CREATE FUNCTION cs_win() RETURNS int4 LANGUAGE pljs WINDOW AS $$
  globalThis.cs_saved_window = pljs.get_window_object();
  return globalThis.cs_saved_window.get_current_position();
$$;
SELECT cs_win() OVER ();
DO LANGUAGE pljs $$
  try {
    cs_saved_window.get_current_position();
  } catch (e) {
    pljs.elog(NOTICE, 'window: ' + e.message);
  }
$$;
SELECT cs_plain();

-- 6) pljs.commit() stays refused while a procedure's result is converted
-- with pljs replaced.  The refusal was read through the replacement, found
-- no storage, and committed half way through the conversion: the CALL failed
-- and what it had written stayed committed.
CREATE TABLE cs_log (n int4);
CREATE TYPE cs_pair AS (a int4, b int4);
CREATE PROCEDURE cs_guard(INOUT r cs_pair) LANGUAGE pljs AS $$
  const commit = pljs.commit, saved = pljs;
  return {
    r: {
      get a() {
        saved.execute('INSERT INTO cs_log VALUES (1)');
        globalThis.pljs = {};
        try {
          commit();
        } catch (e) {
          saved.elog(NOTICE, 'commit: ' + e.message);
        } finally {
          globalThis.pljs = saved;
        }
        return 1;
      },
      get b() {
        throw new Error('b failed');
      }
    }
  };
$$;
CALL cs_guard(NULL);
SELECT count(*) AS committed FROM cs_log;

DROP TABLE cs_tbl, cs_log;
DROP PROCEDURE cs_guard(cs_pair);
DROP TYPE cs_pair;
DROP FUNCTION cs_plain(), cs_clobber(), cs_srf_clobber(), cs_trig(),
  cs_srf_do(), cs_win();
