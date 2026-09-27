-- plan.execute() and plan.cursor() build their parameters in a context of
-- their own, and release it however they end.
--
-- The parameters were freed one by one instead.  That missed every parameter
-- of a plan prepared with type names, which leaked until the call ended, and
-- pfree() of a composite parameter raised "pfree called with invalid
-- pointer": its Datum points inside the tuple's allocation.  The error was
-- raised where JavaScript could not catch it, and unwound past the
-- interpreter's live frames, so the next Error the session built crashed the
-- backend.

-- 1) A composite parameter, with and without type names, and to a cursor.
CREATE TYPE ppm_c AS (a int4, b text);
DO LANGUAGE pljs $$
  const untyped = pljs.prepare('SELECT ($1::ppm_c).b AS b');
  const typed = pljs.prepare('SELECT ($1).b AS b', ['ppm_c']);

  for (const [name, plan] of [['untyped', untyped], ['typed', typed]]) {
    try {
      pljs.elog(NOTICE, name + ' execute: ' + plan.execute([{a: 1, b: 'x'}])[0].b);

      const cursor = plan.cursor([{a: 2, b: 'y'}]);
      pljs.elog(NOTICE, name + ' cursor: ' + cursor.fetch().b);
      cursor.close();
    } catch (e) {
      pljs.elog(NOTICE, name + ' caught: ' + e.message);
    }
    plan.free();
  }
$$;

-- The session is intact: building an Error walks the interpreter's frames.
DO LANGUAGE pljs $$
  try {
    throw new Error('x');
  } catch (e) {
    pljs.elog(NOTICE, 'after: ' + e.message);
  }
$$;

-- 2) The parameters of a plan prepared with type names are released.
CREATE FUNCTION ppm_growth_mb(typed boolean, cursor boolean) RETURNS float8
LANGUAGE pljs AS $$
  const q = 'SELECT sum(total_bytes)::float8 AS b FROM pg_backend_memory_contexts';
  const plan = typed ? pljs.prepare('SELECT length($1) AS n', ['text'])
                     : pljs.prepare('SELECT length($1::text) AS n');
  const s = 'x'.repeat(100000);
  const before = pljs.execute(q)[0].b;

  for (let i = 0; i < 200; i++) {
    if (cursor) {
      const c = plan.cursor([s]);
      c.fetch();
      c.close();
    } else {
      plan.execute([s]);
    }
  }

  const after = pljs.execute(q)[0].b;
  plan.free();
  return (after - before) / (1024 * 1024);
$$;
SELECT ppm_growth_mb(true, false) < 5 AS typed_execute_bounded,
       ppm_growth_mb(false, false) < 5 AS untyped_execute_bounded,
       ppm_growth_mb(true, true) < 5 AS typed_cursor_bounded,
       ppm_growth_mb(false, true) < 5 AS untyped_cursor_bounded;

-- 3) A plan dropped or freed while its parameters are converted.  The
-- conversion can run JavaScript, and a getter that dropped the last other
-- reference to the plan's handle let its finalizer free the plan under it.
DO LANGUAGE pljs $$
  const p = pljs.prepare('SELECT $1::int4 + 1 AS v', ['int4']);
  const dropped = {
    valueOf() {
      p.plan = null;
      pljs.gc();
      return 41;
    }
  };
  pljs.elog(NOTICE, 'dropped: ' + p.execute([dropped])[0].v);

  const q = pljs.prepare('SELECT $1::int4 + 1 AS v', ['int4']);
  const freed = {
    valueOf() {
      q.free();
      return 41;
    }
  };
  try {
    q.execute([freed]);
  } catch (e) {
    pljs.elog(NOTICE, 'freed: ' + e.message);
  }

  const r = pljs.prepare('SELECT $1::int4 + 1 AS v', ['int4']);
  const freed_cursor = {
    valueOf() {
      r.free();
      return 41;
    }
  };
  try {
    r.cursor([freed_cursor]);
  } catch (e) {
    pljs.elog(NOTICE, 'freed cursor: ' + e.message);
  }
$$;

DROP FUNCTION ppm_growth_mb(boolean, boolean);
DROP TYPE ppm_c;
