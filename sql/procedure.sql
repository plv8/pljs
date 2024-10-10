-- create a table to hold the data
CREATE TABLE test1 (a INTEGER);

-- and create a procedure that uses rollback and commit
-- note this will fail on versions older than 11
CREATE PROCEDURE transaction_test1()
LANGUAGE pljs
AS $$
for (let i = 0; i < 10; i++) {
  pljs.execute('INSERT INTO test1 (a) VALUES ($1)', [i]);
  if (i % 2 == 0)
    pljs.commit();
  else
    pljs.rollback();
}
$$;

call transaction_test1();

-- and get the results back
SELECT a FROM test1;

-- cleanup
DROP TABLE test1;
DROP PROCEDURE transaction_test1;