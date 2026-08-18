-- The column/property mismatch error says which column and what was offered.
--
-- return_next() on a composite set raised a bare "field name / property name
-- mismatch", which gave the function author nothing to act on: not which column
-- was missing, and not what the object actually contained.  For a wide RETURNS
-- TABLE that meant reading the whole declaration against the whole object by eye.
--
-- The overwhelmingly common cause is a case difference -- JavaScript property
-- names are case sensitive while PostgreSQL folds unquoted identifiers to lower
-- case, so a `MixedCol` key never matches a `mixedcol` column, and the two look
-- identical at a glance.  Naming both sides makes that immediate.
--
-- Note this deliberately stays an error rather than filling the column with NULL.
-- NULL-filling would make a typo'd or wrong-case key silently produce a NULL
-- column, turning a loud, fixable mistake into exactly the kind of silent data
-- loss the rest of these fixes exist to remove.
CREATE EXTENSION IF NOT EXISTS pljs;

-- 1) a genuinely missing column names itself and lists what was provided.
CREATE FUNCTION cnm_missing() RETURNS TABLE(a int, b text) LANGUAGE pljs AS $$
  pljs.return_next({a: 1});
$$;
SELECT * FROM cnm_missing();

-- 2) the case-mismatch case, which is what this is really for.
CREATE FUNCTION cnm_case() RETURNS TABLE(mixedcol int, b text) LANGUAGE pljs AS $$
  pljs.return_next({MixedCol: 7, b: 'x'});
$$;
SELECT * FROM cnm_case();

-- 3) an object with no properties at all.
CREATE FUNCTION cnm_empty() RETURNS TABLE(a int, b text) LANGUAGE pljs AS $$
  pljs.return_next({});
$$;
SELECT * FROM cnm_empty();

-- 4) the first missing column is the one reported, even when several are absent.
CREATE FUNCTION cnm_several() RETURNS TABLE(alpha int, beta text, gamma int)
LANGUAGE pljs AS $$
  pljs.return_next({beta: 'only this one'});
$$;
SELECT * FROM cnm_several();

-- 5) the error is catchable in JavaScript and carries the detail.
DO $$
  try {
    pljs.execute('SELECT * FROM cnm_case()');
    pljs.elog(NOTICE, 'unexpectedly succeeded');
  } catch (e) {
    pljs.elog(NOTICE, 'names the column: ' + (e.message.indexOf('mixedcol') >= 0));
    pljs.elog(NOTICE, 'lists the keys: ' + (e.message.indexOf('MixedCol') >= 0));
  }
$$ LANGUAGE pljs;

-- 6) a complete object still works, and extra properties remain ignored.
CREATE FUNCTION cnm_ok() RETURNS TABLE(a int, b text) LANGUAGE pljs AS $$
  pljs.return_next({a: 1, b: 'x', extra: 'ignored'});
$$;
SELECT a, b FROM cnm_ok();

DROP FUNCTION cnm_missing, cnm_case, cnm_empty, cnm_several, cnm_ok;

-- The provided-key list is capped.  An object with thousands of properties would
-- otherwise put every name into the message, which lands in the server log as
-- well as the client; ten names plus the total is enough to spot a typo.
CREATE FUNCTION cnm_many() RETURNS TABLE(a int, b text) LANGUAGE pljs AS $$
  const row = {};
  for (let i = 0; i < 500; i++) row['prop' + i] = i;
  pljs.return_next(row);
$$;

SELECT * FROM cnm_many();

DROP FUNCTION cnm_many();

