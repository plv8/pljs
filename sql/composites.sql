CREATE TYPE acomp AS (x int, y text, z timestamptz);
DO LANGUAGE pljs $$
  var jres = pljs.execute("select $1::acomp[]", [ [ { "x": 2, "z": null, "y": null } ] ]);
  pljs.elog(NOTICE,JSON.stringify(jres));
$$;
DROP TYPE acomp;
