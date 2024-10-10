CREATE FUNCTION callee(a int) RETURNS int AS $$ return a * a $$ LANGUAGE pljs;
CREATE FUNCTION sqlf(int) RETURNS int AS $$ SELECT $1 * $1 $$ LANGUAGE sql;
CREATE FUNCTION caller(a int, t int) RETURNS int AS $$
  var func;
  if (t == 1) {
    func = pljs.find_function("callee");
  } else if (t == 2) {
    func = pljs.find_function("callee(int)");
  } else if (t == 3) {
    func = pljs.find_function("sqlf");
  } else if (t == 4) {
    func = pljs.find_function("callee(int, int)");
  } else if (t == 5) {
    try{
      func = pljs.find_function("caller()");
    }catch(e){
      func = function(a){ return a };
    }
  }
  return func(a);
$$ LANGUAGE pljs;

SELECT caller(10, 1);
SELECT caller(10, 2);
SELECT caller(10, 3);
SELECT caller(10, 4);
SELECT caller(10, 5);


-- test find_function permissions failure
CREATE FUNCTION perm() RETURNS void AS $$ pljs.elog(NOTICE, 'nope'); $$ LANGUAGE pljs;

CREATE ROLE someone_else;

REVOKE EXECUTE ON FUNCTION perm() FROM public;

SET ROLE TO someone_else;
DO $$ const func = pljs.find_function('perm') $$ LANGUAGE pljs;
DO $$ const func = pljs.find_function('perm()') $$ LANGUAGE pljs;

RESET ROLE;
DROP ROLE someone_else;

DROP FUNCTION perm();
