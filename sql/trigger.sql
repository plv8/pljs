-- TRIGGER
CREATE TABLE test_tbl (i integer, s text);

CREATE FUNCTION test_trigger() RETURNS trigger AS
$$
	pljs.elog(NOTICE, "NEW = ", JSON.stringify(NEW));
	pljs.elog(NOTICE, "OLD = ", JSON.stringify(OLD));
	pljs.elog(NOTICE, "TG_OP = ", TG_OP);
	pljs.elog(NOTICE, "TG_ARGV = ", TG_ARGV);
	if (TG_OP == "UPDATE") {
		NEW.i = 102;
		return NEW;
	}

	return NEW;
$$
LANGUAGE pljs;

CREATE TRIGGER test_trigger
  BEFORE INSERT OR UPDATE OR DELETE
  ON test_tbl FOR EACH ROW
  EXECUTE PROCEDURE test_trigger();

INSERT INTO test_tbl VALUES(100, 'ABC');
UPDATE test_tbl SET i = 101, s = 'DEF' WHERE i = 1;
DELETE FROM test_tbl WHERE i >= 100;
SELECT * FROM test_tbl;

-- One more trigger
CREATE FUNCTION test_trigger2() RETURNS trigger AS
$$
	var tuple;
	switch (TG_OP) {
	case "INSERT":
		tuple = NEW;
		break;
	case "UPDATE":
		tuple = OLD;
		break;
	case "DELETE":
		tuple = OLD;
		break;
	default:
		return;
	}
	if (tuple.subject == "skip") {
		return null;
	}
	if (tuple.subject == "modify" && NEW) {
		NEW.val = tuple.val * 2;
		return NEW;
	}
$$
LANGUAGE pljs;

CREATE TABLE trig_table (subject text, val int);
INSERT INTO trig_table VALUES('skip', 1);
CREATE TRIGGER test_trigger2
  BEFORE INSERT OR UPDATE OR DELETE
  ON trig_table FOR EACH ROW
  EXECUTE PROCEDURE test_trigger2();

INSERT INTO trig_table VALUES
  ('skip', 1), ('modify', 2), ('noop', 3);
SELECT * FROM trig_table;
UPDATE trig_table SET val = 10;
SELECT * FROM trig_table;
DELETE FROM trig_table;
SELECT * FROM trig_table;
