-- The text I/O fallback frees what a type's I/O functions allocate.
--
-- A type without a conversion of its own reaches JavaScript through its
-- output function and comes back through its input function, which free at
-- most the string or value they return, and not what they allocated on the
-- way to it: range_out()'s and range_in()'s buffers, a detoasted copy of the
-- value.  Run in the caller's context, that piled up for as long as the
-- context lasted -- 2kB for every tstzrange, and a pljs.execute() of 50,000
-- of them grew the backend by 112MB.

CREATE FUNCTION fsm_growth_mb(what text) RETURNS float8 LANGUAGE pljs AS $$
  const q = 'SELECT sum(total_bytes)::float8 AS b FROM pg_backend_memory_contexts';
  const before = pljs.execute(q)[0].b;

  if (what === 'output') {
    // Read 20,000 ranges.
    pljs.execute("SELECT tstzrange(now(), now() + make_interval(secs => i)) AS r " +
                 "FROM generate_series(1, 20000) AS i");
  } else {
    // Bind 20,000 ranges.
    const plan = pljs.prepare('SELECT $1::daterange AS r', ['daterange']);
    for (let i = 0; i < 20000; i++) {
      plan.execute(['[2020-01-01,2020-01-02)']);
    }
    plan.free();
  }

  return (pljs.execute(q)[0].b - before) / (1024 * 1024);
$$;
SELECT fsm_growth_mb('output') < 10 AS output_bounded,
       fsm_growth_mb('input') < 10 AS input_bounded;

-- The same for rows of a set passed to return_next().
CREATE FUNCTION fsm_ranges() RETURNS SETOF daterange LANGUAGE pljs AS $$
  const q = 'SELECT sum(total_bytes)::float8 AS b FROM pg_backend_memory_contexts';
  const before = pljs.execute(q)[0].b;

  for (let i = 0; i < 20000; i++) {
    pljs.return_next('[2020-01-01,2020-01-02)');
  }

  pljs.elog(NOTICE, 'return_next bounded: ' +
            ((pljs.execute(q)[0].b - before) / (1024 * 1024) < 10));
$$;
SELECT count(*) FROM fsm_ranges();

-- The values themselves are intact.
CREATE FUNCTION fsm_echo(r tstzrange, d daterange) RETURNS text LANGUAGE pljs AS $$
  return typeof r + ' ' + r + ' / ' + typeof d + ' ' + d;
$$;
SET TIME ZONE 'UTC';
SELECT fsm_echo('[2020-01-01 00:00+00,2020-01-02 00:00+00)', '[2020-01-01,2020-01-05)');
RESET TIME ZONE;

DROP FUNCTION fsm_growth_mb(text), fsm_ranges(), fsm_echo(tstzrange, daterange);
