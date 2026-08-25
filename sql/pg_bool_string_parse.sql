-- A JavaScript string bound to bool is parsed, not coerced by truthiness.
--
-- JS_ToBool() reports every non-empty string as true, so "false", "f", "no"
-- and "0" all became true, while "" became false. bool's input function
-- accepts the same spellings SQL does and raises on anything else.
--
-- Breaking change: binding or returning the string 'false' used to store
-- true. A real JS boolean, and the truthiness of a non-string, are unchanged.
CREATE EXTENSION IF NOT EXISTS pljs;

-- 1) every spelling SQL accepts, in both directions.
DO $$
  const truthy = ['true', 't', 'TRUE', 'True', 'yes', 'y', 'on', '1', '  true  '];
  const falsy  = ['false', 'f', 'FALSE', 'False', 'no', 'n', 'off', '0', '  false  '];
  const bad = [];

  for (const s of truthy) {
    const v = pljs.execute('SELECT $1::bool AS v', [s])[0].v;
    if (v !== true) { bad.push(JSON.stringify(s) + ' -> ' + v); }
  }
  for (const s of falsy) {
    const v = pljs.execute('SELECT $1::bool AS v', [s])[0].v;
    if (v !== false) { bad.push(JSON.stringify(s) + ' -> ' + v); }
  }

  pljs.elog(NOTICE, 'all bool spellings correct: ' + (bad.length === 0) +
                    (bad.length ? ' failures: ' + bad.join(', ') : ''));
$$ LANGUAGE pljs;

-- 2) a JS boolean stringified and bound must round-trip to itself.
--    (needsSnapshot ? "true" : "false" is the shape that used to invert.)
DO $$
  for (const b of [true, false]) {
    const s = b ? 'true' : 'false';
    const v = pljs.execute('SELECT $1::bool AS v', [s])[0].v;
    pljs.elog(NOTICE, 'stringified ' + s + ' -> ' + v + ' correct=' + (v === b));
  }
$$ LANGUAGE pljs;

-- the SQL-side workaround (? = 'true') still agrees.
DO $$
  for (const s of ['true', 'false']) {
    const v = pljs.execute('SELECT ($1 = $2) AS v', [s, 'true'])[0].v;
    pljs.elog(NOTICE, "idiom (? = 'true') " + s + ' -> ' + v);
  }
$$ LANGUAGE pljs;

-- 3) a bool return value, not just a bind.
CREATE FUNCTION bsp_false_str() RETURNS bool LANGUAGE pljs AS $$ return 'false'; $$;
CREATE FUNCTION bsp_true_str() RETURNS bool LANGUAGE pljs AS $$ return 'true'; $$;
CREATE FUNCTION bsp_f_str() RETURNS bool LANGUAGE pljs AS $$ return 'f'; $$;
SELECT bsp_false_str(), bsp_true_str(), bsp_f_str();

-- a bool column via return_next.
CREATE FUNCTION bsp_col() RETURNS TABLE(flag bool, k int) LANGUAGE pljs AS $$
  pljs.return_next({flag: 'false', k: 1});
  pljs.return_next({flag: 'true', k: 2});
$$;
SELECT k, flag FROM bsp_col() ORDER BY k;

-- 4) text that is not a boolean raises instead of silently becoming true,
--    and the empty string no longer masquerades as false.
CREATE FUNCTION bsp_bad() RETURNS bool LANGUAGE pljs AS $$ return 'maybe'; $$;
CREATE FUNCTION bsp_empty() RETURNS bool LANGUAGE pljs AS $$ return ''; $$;
SELECT bsp_bad();
SELECT bsp_empty();

-- 5) real JS booleans and truthiness of non-strings are unchanged.
CREATE FUNCTION bsp_true() RETURNS bool LANGUAGE pljs AS $$ return true; $$;
CREATE FUNCTION bsp_false() RETURNS bool LANGUAGE pljs AS $$ return false; $$;
CREATE FUNCTION bsp_zero() RETURNS bool LANGUAGE pljs AS $$ return 0; $$;
CREATE FUNCTION bsp_one() RETURNS bool LANGUAGE pljs AS $$ return 1; $$;
CREATE FUNCTION bsp_obj() RETURNS bool LANGUAGE pljs AS $$ return {}; $$;
SELECT bsp_true(), bsp_false(), bsp_zero(), bsp_one(), bsp_obj();

-- 6) a NULL return is still NULL.
CREATE FUNCTION bsp_null() RETURNS bool LANGUAGE pljs AS $$ return null; $$;
SELECT bsp_null() IS NULL AS null_ok;

DROP FUNCTION bsp_false_str, bsp_true_str, bsp_f_str, bsp_col, bsp_bad,
  bsp_empty, bsp_true, bsp_false, bsp_zero, bsp_one, bsp_obj, bsp_null;
