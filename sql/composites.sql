CREATE TYPE acomp AS (x int, y text, z timestamptz);
DO LANGUAGE pljs $$
  var jres = pljs.execute("select $1::acomp[]", [ [ { "x": 2, "z": null, "y": null } ] ]);
  pljs.elog(NOTICE,JSON.stringify(jres));
$$;
DROP TYPE acomp;

-- composite return with null fields
CREATE TYPE nullcomp AS (a int, b text, c float8);

CREATE FUNCTION make_null_comp() RETURNS nullcomp AS $$
  return { a: 1, b: null, c: null };
$$ LANGUAGE pljs;
SELECT * FROM make_null_comp();
DROP FUNCTION make_null_comp();
DROP TYPE nullcomp;

-- RETURNS TABLE with null fields
CREATE FUNCTION srf_with_nulls() RETURNS TABLE(id int, val text) AS $$
  pljs.return_next({ id: 1, val: 'hello' });
  pljs.return_next({ id: 2, val: null });
  pljs.return_next({ id: null, val: 'world' });
$$ LANGUAGE pljs;
SELECT * FROM srf_with_nulls();
DROP FUNCTION srf_with_nulls();
