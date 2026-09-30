-- What a builtin holds is released when it fails, and nothing held by the
-- transaction a failed commit ended is used again.

-- 1) A commit that fails -- a deferred foreign key -- has rolled its
-- transaction back and begun the next.  The resource owner of the one that
-- failed was put back, and every builtin after it used that owner, which had
-- been freed: "ResourceOwnerEnlarge called after release started".
CREATE TABLE rcl_parent (id int4 PRIMARY KEY);
CREATE TABLE rcl_child (pid int4 REFERENCES rcl_parent
                        DEFERRABLE INITIALLY DEFERRED);

CREATE PROCEDURE rcl_commit_fails() LANGUAGE pljs AS $$
  pljs.execute('INSERT INTO rcl_child VALUES (1)');

  try {
    pljs.commit();
  } catch (e) {
    pljs.elog(NOTICE, 'commit failed: ' + e.sqlstate);
  }

  for (let i = 0; i < 3; i++) {
    pljs.execute('SELECT 1');
  }

  pljs.prepare('SELECT $1::int4 AS i', ['int4']).execute([1]);
  pljs.elog(NOTICE, 'still running');
$$;

CALL rcl_commit_fails();

SELECT count(*) AS children FROM rcl_child;

-- 2) A failed pljs.prepare(): parsing the statement or a type name pinned the
-- catalogs, and only rolling back releases the pins.  COMMIT warned of each.
BEGIN;
DO $$
  try {
    pljs.prepare('SELECT rcl_no_such_column FROM pg_class');
  } catch (e) {
    pljs.elog(NOTICE, 'prepare: ' + e.message);
  }

  try {
    pljs.prepare('SELECT $1 AS v', ['varchar(0)']);
  } catch (e) {
    pljs.elog(NOTICE, 'type name: ' + e.message);
  }
$$ LANGUAGE pljs;
COMMIT;

-- 3) A pljs.find_function() whose function's top-level code throws: the
-- catalog pin was left.
CREATE FUNCTION rcl_top_throws() RETURNS void LANGUAGE pljs AS $$
  return;
} throw new Error('top level'); function rcl_unused() {
$$;

BEGIN;
DO $$
  try {
    pljs.find_function('rcl_top_throws');
  } catch (e) {
    pljs.elog(NOTICE, 'find_function: ' + e.message);
  }
$$ LANGUAGE pljs;
COMMIT;

SELECT 1 AS still_connected;

DROP FUNCTION rcl_top_throws();
DROP PROCEDURE rcl_commit_fails();
DROP TABLE rcl_child;
DROP TABLE rcl_parent;
