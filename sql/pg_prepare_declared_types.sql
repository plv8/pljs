-- pljs.prepare() uses the parameter types it is given.
--
-- It had plv8's condition the wrong way round: given type names, it parsed
-- them, discarded them and had the parser infer the types instead; given
-- none, it prepared with no parameters at all.  So a declared type changed
-- nothing -- `SELECT $1 AS x` with ['int4'] bound $1 as text -- a parameter
-- declared as a domain was never checked against it, and a plan without type
-- names could not use a parameter.

-- 1) The declared type is the parameter's type.
DO $$
  const plan = pljs.prepare('SELECT pg_typeof($1)::text AS t, $1 AS x', ['int4']);
  const row = plan.execute(['7'])[0];

  pljs.elog(NOTICE, row.t + ' ' + row.x + ' ' + typeof row.x);
  plan.free();
$$ LANGUAGE pljs;

-- The same through the trailing-arguments form.
DO $$
  const plan = pljs.prepare('SELECT pg_typeof($1)::text AS t, pg_typeof($2)::text AS u',
                            'int8', 'numeric');

  pljs.elog(NOTICE, JSON.stringify(plan.execute([1, 2])[0]));
  plan.free();
$$ LANGUAGE pljs;

-- Arguments passed after the SQL rather than in an array are the caller's,
-- borrowed from QuickJS.  pljs_values_to_array() put them into an array
-- without taking a reference, so freeing the array released the caller's
-- string constants, and the backend crashed when the block's bytecode was
-- freed -- on main as well, for every function that takes trailing arguments.
DO $$
  pljs.elog(NOTICE, pljs.execute('SELECT $1 || $2 AS s', 'a', 'b')[0].s);

  const plan = pljs.prepare('SELECT $1 || $2 AS s', 'text', 'text');

  pljs.elog(NOTICE, plan.execute('c', 'd')[0].s);

  const cursor = plan.cursor('e', 'f');

  pljs.elog(NOTICE, cursor.fetch().s);
  cursor.close();
  plan.free();
$$ LANGUAGE pljs;

-- 2) A value is converted to a declared domain, and checked against it, by
-- both execute() and cursor().
CREATE DOMAIN pdt_pos AS int4 CHECK (VALUE > 0);
DO $$
  const plan = pljs.prepare('SELECT $1 AS x', ['pdt_pos']);

  for (const v of [5, 0]) {
    try {
      pljs.elog(NOTICE, 'execute ' + plan.execute([v])[0].x);
    } catch (e) {
      pljs.elog(NOTICE, 'execute: ' + e.message);
    }

    try {
      const cursor = plan.cursor([v]);

      pljs.elog(NOTICE, 'cursor ' + cursor.fetch().x);
      cursor.close();
    } catch (e) {
      pljs.elog(NOTICE, 'cursor: ' + e.message);
    }
  }

  plan.free();
$$ LANGUAGE pljs;

-- 3) An unknown type name is a JavaScript error, and says which.
DO $$
  try {
    pljs.prepare('SELECT $1 AS x', ['pdt_no_such_type']);
  } catch (e) {
    pljs.elog(NOTICE, e.message);
  }
$$ LANGUAGE pljs;

-- 4) Without type names, the parameters' types are inferred.
DO $$
  const plan = pljs.prepare('SELECT $1::int4 + 1 AS x');

  pljs.elog(NOTICE, 'inferred ' + plan.execute([41])[0].x);
  plan.free();
$$ LANGUAGE pljs;

-- 5) Type names declare every parameter, as in plv8.
DO $$
  try {
    pljs.prepare('SELECT $1::int4 + $2::int4 AS x', ['int4']);
  } catch (e) {
    pljs.elog(NOTICE, e.message);
  }
$$ LANGUAGE pljs;

DROP DOMAIN pdt_pos;
