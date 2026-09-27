-- An error converting a query's rows to JavaScript is a JavaScript exception.
--
-- pljs.execute(), plan.execute(), cursor.fetch() and the window object's
-- argument getters converted what they read after their PG_TRY, so an error
-- converting a value -- a numeric too large for a double, a multidimensional
-- array -- could not be caught, and it unwound past the interpreter's live
-- frames: the next Error the session built crashed the backend.

CREATE FUNCTION bce_catch(what text) RETURNS text LANGUAGE pljs AS $$
  const huge = 'SELECT 1e400::numeric AS n';
  const nested = 'SELECT ARRAY[[1, 2], [3, 4]] AS a';

  try {
    switch (what) {
    case 'execute':
      pljs.execute(huge);
      break;
    case 'execute nested':
      pljs.execute(nested);
      break;
    case 'plan':
      pljs.prepare(nested).execute();
      break;
    case 'fetch':
      pljs.prepare(huge).cursor().fetch();
      break;
    }
    return what + ': no error';
  } catch (e) {
    return what + ': caught ' + e.sqlstate;
  }
$$;
SELECT bce_catch('execute');
SELECT bce_catch('execute nested');
SELECT bce_catch('plan');
SELECT bce_catch('fetch');

-- The session is intact: building an Error walks the interpreter's frames.
DO LANGUAGE pljs $$
  try {
    throw new Error('x');
  } catch (e) {
    pljs.elog(NOTICE, 'after: ' + e.message);
  }
$$;

-- A window object's argument getter, whose errors it re-threw.
CREATE FUNCTION bce_window(a int4) RETURNS text LANGUAGE pljs WINDOW AS $$
  const w = pljs.get_window_object();
  try {
    return 'value ' + w.get_func_arg_in_partition(0, 0, 99, false);
  } catch (e) {
    return 'caught ' + e.message;
  }
$$;
SELECT bce_window(a) OVER () FROM (VALUES (1)) AS t(a);

DO LANGUAGE pljs $$
  try {
    throw new Error('y');
  } catch (e) {
    pljs.elog(NOTICE, 'after window: ' + e.message);
  }
$$;

-- Uncaught, the conversion error is the function's.
CREATE FUNCTION bce_uncaught() RETURNS int4 LANGUAGE pljs AS $$
  pljs.execute('SELECT ARRAY[[1, 2], [3, 4]] AS a');
  return 1;
$$;
SELECT bce_uncaught();

DROP FUNCTION bce_catch(text), bce_window(int4), bce_uncaught();
