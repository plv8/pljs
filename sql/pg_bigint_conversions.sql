-- A BigInt is converted to json and jsonb as JSON.stringify() converts it,
-- and to numeric exactly, without leaking.

-- 1) JSON.stringify() has no JSON for a BigInt.  jsonb stored it as a string,
-- "10", where json raised; every int8 a query returns is a BigInt.
CREATE FUNCTION bic_json() RETURNS json LANGUAGE pljs AS $$
  return {a: 10n};
$$;
SELECT bic_json();

CREATE FUNCTION bic_jsonb() RETURNS jsonb LANGUAGE pljs AS $$
  return {a: 10n};
$$;
SELECT bic_jsonb();

CREATE FUNCTION bic_jsonb_element() RETURNS jsonb LANGUAGE pljs AS $$
  return [1, 10n];
$$;
SELECT bic_jsonb_element();

-- A BigInt object, as JSON.stringify() unwraps one.  It was written as its
-- properties, {}.
CREATE FUNCTION bic_jsonb_object() RETURNS jsonb LANGUAGE pljs AS $$
  return {a: Object(10n)};
$$;
SELECT bic_jsonb_object();

-- A data error, which a caller can tell from an internal one.
DO $$
BEGIN
  PERFORM bic_jsonb();
EXCEPTION WHEN invalid_parameter_value THEN
  RAISE NOTICE 'invalid_parameter_value: %', SQLERRM;
END $$;

CREATE FUNCTION bic_jsonb_row() RETURNS jsonb LANGUAGE pljs AS $$
  const row = pljs.execute('SELECT 10::int8 AS id')[0];
  return {id: row.id};
$$;
SELECT bic_jsonb_row();

-- Converted as a Number or a String, it is stored.
CREATE FUNCTION bic_jsonb_converted() RETURNS jsonb LANGUAGE pljs AS $$
  const row = pljs.execute('SELECT 10::int8 AS id')[0];
  return {id: Number(row.id), s: String(row.id)};
$$;
SELECT bic_jsonb_converted();

-- As JSON.stringify(), a BigInt with a toJSON() is written as what it gives.
CREATE FUNCTION bic_jsonb_to_json() RETURNS text LANGUAGE pljs AS $$
  BigInt.prototype.toJSON = function () { return Number(this); };

  try {
    return JSON.stringify(pljs.execute('SELECT $1::jsonb AS j',
                                       [{a: 10n, b: [1n], c: Object(2n)}])[0].j);
  } finally {
    delete BigInt.prototype.toJSON;
  }
$$;
SELECT bic_jsonb_to_json();
SELECT bic_jsonb();

-- 2) numeric from a BigInt, exactly, and with the typmod applied.
CREATE FUNCTION bic_numeric() RETURNS SETOF numeric LANGUAGE pljs AS $$
  pljs.return_next(5n);
  pljs.return_next(-123456789012345678901234567890n);
  pljs.return_next({n: 7n});
$$;
SELECT * FROM bic_numeric();

CREATE TYPE bic_scaled AS (n numeric(5, 1));
CREATE FUNCTION bic_numeric_typmod() RETURNS SETOF bic_scaled
LANGUAGE pljs AS $$
  pljs.return_next({n: 12n});
$$;
SELECT * FROM bic_numeric_typmod();

-- The BigInt's text was rendered twice and released neither time: a string of
-- about 300 bytes for every row here, 6MB in all.
CREATE FUNCTION bic_mb() RETURNS float8 LANGUAGE sql AS $$
  SELECT (pljs_info() ->> 'malloc_size')::float8 / (1024 * 1024);
$$;
CREATE FUNCTION bic_gc() RETURNS void LANGUAGE pljs AS $$ pljs.gc(); $$;

CREATE FUNCTION bic_numeric_rows(n int4) RETURNS SETOF numeric
LANGUAGE pljs AS $$
  const big = 10n ** 300n;

  for (let i = 0; i < n; i++) {
    pljs.return_next(big + BigInt(i));
  }
$$;

DO $$
DECLARE
  before float8;
BEGIN
  PERFORM count(*) FROM bic_numeric_rows(10);
  PERFORM bic_gc();
  before := bic_mb();
  PERFORM count(*) FROM bic_numeric_rows(20000);
  PERFORM bic_gc();
  RAISE NOTICE 'released: %', bic_mb() - before < 2;
END $$;

DROP FUNCTION bic_numeric_rows(int4);
DROP FUNCTION bic_gc();
DROP FUNCTION bic_mb();
DROP FUNCTION bic_numeric_typmod();
DROP TYPE bic_scaled;
DROP FUNCTION bic_numeric();
DROP FUNCTION bic_jsonb_to_json();
DROP FUNCTION bic_jsonb_converted();
DROP FUNCTION bic_jsonb_row();
DROP FUNCTION bic_jsonb_object();
DROP FUNCTION bic_jsonb_element();
DROP FUNCTION bic_jsonb();
DROP FUNCTION bic_json();
