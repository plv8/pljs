-- An array too long for pljs is a clean error.
--
-- An array's length was read as an int32, so a length of 2^31 or more wrapped
-- to a negative number, which every caller took for a length that had thrown.
-- No exception was pending, so the error reported was whatever the runtime
-- had left over.  It is a RangeError now.

-- 1) A function's result.
CREATE FUNCTION alr_result() RETURNS int4[] LANGUAGE pljs AS $$
  const a = [];
  a.length = 3e9;
  return a;
$$;
SELECT alr_result();

-- 2) Parameters, and a set's rows, which JavaScript can catch.
CREATE FUNCTION alr_caught() RETURNS SETOF text LANGUAGE pljs AS $$
  const a = [];
  a.length = 2 ** 31;

  try {
    pljs.execute('SELECT 1', a);
  } catch (e) {
    pljs.return_next('execute: ' + e.name + ': ' + e.message);
  }

  try {
    pljs.prepare('SELECT 1', a);
  } catch (e) {
    pljs.return_next('prepare: ' + e.name + ': ' + e.message);
  }
$$;
SELECT * FROM alr_caught();

CREATE FUNCTION alr_rows() RETURNS SETOF int4 LANGUAGE pljs AS $$
  const a = [1, 2];
  a.length = 2 ** 32 - 1;
  return a;
$$;
SELECT * FROM alr_rows();

-- 3) A length just short of it is read as it is: a sparse array's holes are
-- NULL.
CREATE FUNCTION alr_sparse() RETURNS int4[] LANGUAGE pljs AS $$
  const a = [1];
  a.length = 3;
  return a;
$$;
SELECT alr_sparse();

DROP FUNCTION alr_result(), alr_caught(), alr_rows(), alr_sparse();
