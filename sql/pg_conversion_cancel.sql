-- A cancel that interrupts JavaScript run by a conversion raises as a cancel.
--
-- QuickJS stops a script for a pending cancel by throwing "interrupted", and
-- every call path runs CHECK_FOR_INTERRUPTS() before it reports what
-- JavaScript threw.  A conversion reported it without one, so a
-- statement_timeout during a result's toString() failed as XX000
-- "interrupted" rather than 57014, and a caller that retries on a timeout, or
-- catches query_canceled, did not see one.

CREATE FUNCTION cc_spin_text() RETURNS text LANGUAGE pljs AS $$
  return {toString() { while (true) {} }};
$$;

CREATE FUNCTION cc_spin_int() RETURNS int4 LANGUAGE pljs AS $$
  return {valueOf() { while (true) {} }};
$$;

SET statement_timeout = '300ms';

DO $$
BEGIN
  PERFORM cc_spin_text();
EXCEPTION WHEN query_canceled THEN
  RAISE NOTICE 'text: %', SQLERRM;
END $$;

DO $$
BEGIN
  PERFORM cc_spin_int();
EXCEPTION WHEN query_canceled THEN
  RAISE NOTICE 'int4: %', SQLERRM;
END $$;

RESET statement_timeout;

-- A cancel ends the call, even one JavaScript catches.  By the time a builtin
-- hands it to JavaScript it is no longer pending, and nothing interrupted a
-- function that caught it and went on: a loop after the catch ran for ever.
-- Each loop here gives up after five seconds, and says the cancel was caught.
CREATE FUNCTION cc_catch_conversion() RETURNS text LANGUAGE pljs AS $$
  try {
    pljs.execute('SELECT $1::text AS t', [{toString() { while (true) {} }}]);
  } catch (e) {}

  const end = Date.now() + 5000;
  while (Date.now() < end) {}

  return 'the cancel was caught';
$$;

CREATE FUNCTION cc_catch_query() RETURNS text LANGUAGE pljs AS $$
  try {
    pljs.execute('SELECT pg_sleep(10)');
  } catch (e) {}

  const end = Date.now() + 5000;
  while (Date.now() < end) {}

  return 'the cancel was caught';
$$;

SET statement_timeout = '300ms';
SELECT cc_catch_conversion();
SELECT cc_catch_query();
RESET statement_timeout;

-- Nothing commits once the call has to end with an error.  A Promise's
-- executor turns the exception that was to end the call into a rejection, and
-- the commit after it committed the work of a call that then failed.
CREATE TABLE cc_rows (i int4);

CREATE PROCEDURE cc_commit_after_cancel() LANGUAGE pljs AS $$
  pljs.execute('INSERT INTO cc_rows VALUES (1)');
  new Promise(() => pljs.execute('SELECT pg_sleep(10)'));
  pljs.commit();
$$;

SET statement_timeout = '300ms';
CALL cc_commit_after_cancel();
RESET statement_timeout;

SELECT count(*) AS committed FROM cc_rows;

-- Nor does any other SQL: the sequence's nextval() outlived the call.
CREATE SEQUENCE cc_seq;

CREATE FUNCTION cc_execute_after_cancel() RETURNS void LANGUAGE pljs AS $$
  new Promise(() => pljs.execute('SELECT pg_sleep(10)'));
  pljs.execute("SELECT nextval('cc_seq')");
$$;

SET statement_timeout = '300ms';
SELECT cc_execute_after_cancel();
RESET statement_timeout;

SELECT is_called FROM cc_seq;

DROP FUNCTION cc_execute_after_cancel();
DROP SEQUENCE cc_seq;

-- What the function returns is not converted either: an error converting it
-- replaced the timeout, as "returned row must be an object".
CREATE FUNCTION cc_return_after_cancel() RETURNS TABLE(a int4, b text)
LANGUAGE pljs AS $$
  new Promise(() => pljs.execute('SELECT pg_sleep(10)'));
  return [5];
$$;

SET statement_timeout = '300ms';

DO $$
BEGIN
  PERFORM * FROM cc_return_after_cancel();
EXCEPTION WHEN query_canceled THEN
  RAISE NOTICE 'returned: %', SQLERRM;
END $$;

RESET statement_timeout;

DROP FUNCTION cc_return_after_cancel();

SELECT 1 AS still_connected;

DROP PROCEDURE cc_commit_after_cancel();
DROP TABLE cc_rows;
DROP FUNCTION cc_catch_query();
DROP FUNCTION cc_catch_conversion();
DROP FUNCTION cc_spin_int();
DROP FUNCTION cc_spin_text();
