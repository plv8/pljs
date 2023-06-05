CREATE TABLE test_tbl (i integer, s text);
CREATE FUNCTION test_sql() RETURNS integer AS
$$
	// for name[] conversion test, add current_schemas()
	let rows = pljs.execute("SELECT i, 's' || i AS s, current_schemas(true) AS c FROM generate_series(1, 4) AS t(i)");
	for (let r = 0; r < rows.length; r++)
	{
		let result = pljs.execute("INSERT INTO test_tbl VALUES(" + rows[r].i + ",'" + rows[r].s + "')");
		pljs.elog(NOTICE, JSON.stringify(rows[r]), result);
	}
	return rows.length;
$$
LANGUAGE pljs;
SELECT test_sql();
SELECT * FROM test_tbl;


-- SPI operations
CREATE FUNCTION prep1() RETURNS void AS $$
  let plan = pljs.prepare("SELECT * FROM test_tbl");
  pljs.elog(INFO, plan.toString());
  let rows = plan.execute();

  for(let i = 0; i < rows.length; i++) {
    pljs.elog(INFO, JSON.stringify(rows[i]));
  }

  let cursor = plan.cursor();
  pljs.elog(INFO, cursor.toString());

  let row;
  while(row = cursor.fetch()) {
    pljs.elog(INFO, JSON.stringify(row));
  }
  cursor.close();

  cursor = plan.cursor();

  rows = cursor.fetch(2);
  pljs.elog(INFO, JSON.stringify(rows));

  rows = cursor.fetch(-2);
  pljs.elog(INFO, JSON.stringify(rows));

  cursor.move(1);
  rows = cursor.fetch(3);
  pljs.elog(INFO, JSON.stringify(rows));

  cursor.move(-2);
  rows = cursor.fetch(3);
  pljs.elog(INFO, JSON.stringify(rows));
  cursor.close();

  plan.free();

  plan = pljs.prepare("SELECT * FROM test_tbl WHERE i = $1 and s = $2", ["int", "text"]);
  rows = plan.execute([2, "s2"]);
  pljs.elog(INFO, "rows.length = ", rows.length);

  cursor = plan.cursor([2, "s2"]);
  pljs.elog(INFO, JSON.stringify(cursor.fetch()));
  cursor.close();
  plan.free();

  try{
    plan = pljs.prepare("SELECT * FROM test_tbl");
    plan.free();
    plan.execute();
  }catch(e){
    pljs.elog(WARNING, e);
  }
  try{
    plan = pljs.prepare("SELECT * FROM test_tbl");
    cursor = plan.cursor();
    cursor.close();
    cursor.fetch();
  }catch(e){
    pljs.elog(WARNING, e);
  }
$$ LANGUAGE pljs STRICT;
SELECT prep1();
