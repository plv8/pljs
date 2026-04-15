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

-- subtransaction with rollback
CREATE TABLE subtxn_test (id int);

DO $$
  pljs.subtransaction(function() {
    pljs.execute('INSERT INTO subtxn_test VALUES (1)');
  });
  try {
    pljs.subtransaction(function() {
      pljs.execute('INSERT INTO subtxn_test VALUES (2)');
      throw new Error('rollback this');
    });
  } catch(e) {
    pljs.elog(NOTICE, 'caught: ' + e.message);
  }
$$ LANGUAGE pljs;

SELECT * FROM subtxn_test;
DROP TABLE subtxn_test;
