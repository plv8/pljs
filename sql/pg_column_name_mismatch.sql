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


-- A row the function returns, rather than passes to return_next(), is checked
-- the same way.  It was not, and a missing column was stored as NULL.
CREATE FUNCTION cnm_returned() RETURNS TABLE(user_id int, name text)
LANGUAGE pljs AS $$
  return [{userId: 1, name: 'x'}];
$$;

SELECT * FROM cnm_returned();

CREATE FUNCTION cnm_returned_ok() RETURNS TABLE(user_id int, name text)
LANGUAGE pljs AS $$
  return [{user_id: 1, name: 'x', extra: 'ignored'}];
$$;

SELECT * FROM cnm_returned_ok();

DROP FUNCTION cnm_returned, cnm_returned_ok;

-- A column is a property of the row, its own or one it inherits, as converting
-- the row reads it: a class's getters.  The check of a returned row took only
-- its own properties, and refused rows that converted.
CREATE FUNCTION cnm_inherited() RETURNS TABLE(a int, b text)
LANGUAGE pljs AS $$
  class P { get a() { return 1; } get b() { return 'x'; } }

  pljs.return_next(new P());
  return [new P(), Object.create({a: 2, b: 'y'})];
$$;

SELECT * FROM cnm_inherited();

-- A returned row that is not an object is refused, as return_next() refuses
-- one.  It was stored as a row of NULLs.
CREATE FUNCTION cnm_primitive() RETURNS TABLE(a int, b text)
LANGUAGE pljs AS $$
  return [5];
$$;

SELECT * FROM cnm_primitive();

-- A Proxy whose trap throws: its error, not the missing column's.
CREATE FUNCTION cnm_proxy() RETURNS TABLE(a int, b text) LANGUAGE pljs AS $$
  pljs.return_next(new Proxy({a: 1}, {ownKeys() { throw new Error('trap'); }}));
$$;

SELECT * FROM cnm_proxy();

-- A key that is not valid text is listed as '?', not an encoding error.
CREATE FUNCTION cnm_surrogate() RETURNS TABLE(a int, b text)
LANGUAGE pljs AS $$
  const row = {a: 1};

  row[String.fromCharCode(0xD800)] = 1;
  pljs.return_next(row);
$$;

SELECT * FROM cnm_surrogate();

-- But not one every object inherits from Object.prototype: a row with no
-- "toString" or "constructor" property was taken to have that column, and the
-- builtin function was stored as its value.
CREATE FUNCTION cnm_builtin() RETURNS TABLE(a int, "toString" text)
LANGUAGE pljs AS $$
  return [{a: 1}];
$$;

SELECT * FROM cnm_builtin();

CREATE FUNCTION cnm_builtin_next() RETURNS TABLE(a int, "constructor" text)
LANGUAGE pljs AS $$
  pljs.return_next({a: 1});
$$;

SELECT * FROM cnm_builtin_next();

CREATE FUNCTION cnm_builtin_own() RETURNS TABLE(a int, "toString" text)
LANGUAGE pljs AS $$
  return [{a: 1, toString: 'mine'}];
$$;

SELECT * FROM cnm_builtin_own();

-- A Proxy row is read through its get trap, as a record is: it was read from
-- its target, and the two disagreed.
CREATE FUNCTION cnm_proxy_get() RETURNS TABLE(a int, b text)
LANGUAGE pljs AS $$
  const row = new Proxy({a: 1, b: 'raw'}, {
    get(target, key) { return key === 'a' ? 100 : 'from-trap'; }
  });

  pljs.return_next(row);
  return [row];
$$;

SELECT * FROM cnm_proxy_get();

-- A Proxy whose get trap makes its columns up, over an empty target.
CREATE FUNCTION cnm_proxy_made_up() RETURNS TABLE(a int, b text)
LANGUAGE pljs AS $$
  const row = new Proxy({}, {
    get(target, key) { return key === 'a' ? 1 : 'x'; }
  });

  pljs.return_next(row);
  return [row];
$$;

SELECT * FROM cnm_proxy_made_up();

-- A Proxy that forwards to a plain object has no more columns than it does.
-- What every object inherits -- constructor(), valueOf() -- was taken for a
-- column of the Proxy, and the builtin function was stored as its value.
CREATE FUNCTION cnm_proxy_forwarding() RETURNS TABLE(constructor text, a int)
LANGUAGE pljs AS $$
  pljs.return_next(new Proxy({a: 1}, {
    get(target, key) { return Reflect.get(target, key); }
  }));
$$;

SELECT * FROM cnm_proxy_forwarding();

-- A single-column set reads a Proxy row too, through its get trap, by the
-- name the function gives its column.  The Proxy was converted as the
-- column's value itself.
CREATE FUNCTION cnm_proxy_single() RETURNS TABLE(a int) LANGUAGE pljs AS $$
  pljs.return_next(new Proxy({a: 7}, {}));
  pljs.return_next(new Proxy({a: 0}, {
    get(target, key) { return key === 'a' ? 8 : Reflect.get(target, key); }
  }));
  pljs.return_next(new Proxy({}, {
    get(target, key) { return key === 'a' ? 9 : undefined; }
  }));
$$;

SELECT * FROM cnm_proxy_single();

CREATE FUNCTION cnm_proxy_single_returned() RETURNS TABLE(a int)
LANGUAGE pljs AS $$
  return [new Proxy({a: 9}, {})];
$$;

SELECT * FROM cnm_proxy_single_returned();

-- But an array's Proxy is the array, as it is for a function that returns
-- one: it was taken for a row object, which named no column.
CREATE FUNCTION cnm_proxy_array() RETURNS SETOF int[] LANGUAGE pljs AS $$
  pljs.return_next(new Proxy([1, 2, 3], {}));
  return [new Proxy([4, 5], {})];
$$;

SELECT * FROM cnm_proxy_array();

-- A method is not a column's value in a set of several columns either, as it
-- is not in a set of one: its source was stored as text, and NaN as a number.
CREATE FUNCTION cnm_method() RETURNS TABLE(id int, name text)
LANGUAGE pljs AS $$
  class Person {
    constructor() { this.id = 1; }
    name() { return 'n'; }
  }

  pljs.return_next(new Person());
$$;

SELECT * FROM cnm_method();

CREATE FUNCTION cnm_arrow() RETURNS TABLE(id int, amount float8)
LANGUAGE pljs AS $$
  return [{id: 1, amount: () => 5}];
$$;

SELECT * FROM cnm_arrow();

-- Except a json or jsonb column, where a function is SQL NULL, as it is
-- anywhere in a document.
CREATE FUNCTION cnm_json_function() RETURNS TABLE(id int, j jsonb)
LANGUAGE pljs AS $$
  pljs.return_next({id: 1, j: () => 1});
$$;

SELECT * FROM cnm_json_function();

-- A row that is not an object at all is datatype_mismatch too, as a returned
-- one is.  It was thrown to JavaScript, and came out as XX000.
CREATE FUNCTION cnm_not_object() RETURNS TABLE(id int, b int)
LANGUAGE pljs AS $$
  pljs.return_next(5);
$$;

DO $$
BEGIN
  PERFORM * FROM cnm_not_object();
EXCEPTION WHEN datatype_mismatch THEN
  RAISE NOTICE 'datatype_mismatch: %', SQLERRM;
END $$;

-- A RETURNS TABLE column is a parameter of the function, left undefined, and
-- a body can assign to it, strict or not.
CREATE FUNCTION cnm_table_column() RETURNS TABLE(id int, label text)
LANGUAGE pljs AS $$
  'use strict';
  id = 1;
  label = 'one';
  pljs.return_next({id: id, label: label});
$$;

SELECT * FROM cnm_table_column();

DROP FUNCTION cnm_inherited, cnm_primitive, cnm_proxy, cnm_surrogate,
  cnm_builtin, cnm_builtin_next, cnm_builtin_own, cnm_proxy_get,
  cnm_proxy_made_up, cnm_proxy_forwarding, cnm_proxy_single,
  cnm_proxy_single_returned, cnm_proxy_array, cnm_method, cnm_arrow,
  cnm_json_function, cnm_not_object,
  cnm_table_column;
