-- A string from JavaScript is checked against the database's encoding.
--
-- QuickJS writes a lone surrogate -- '\uD800', half of a pair -- as the three
-- bytes ED A0 80, which are not UTF-8.  In a UTF8 database the fallback,
-- which converts through the type's own input function, passed them on
-- unchecked, and so did jsonb: a refcursor, a citext or any other type the
-- fallback converts, and a jsonb string or key, stored them, and a dump of
-- the table could not then be restored.

-- 1) Through the fallback.
CREATE FUNCTION u8_lone() RETURNS refcursor LANGUAGE pljs AS $$
  return 'a\uD800b';
$$;
SELECT u8_lone();

-- A pair is a character, and is stored as one.
CREATE FUNCTION u8_pair() RETURNS refcursor LANGUAGE pljs AS $$
  return 'a\uD83D\uDE00b';
$$;
SELECT encode(convert_to(u8_pair()::text, 'UTF8'), 'hex') AS pair;

-- As a query's parameter.
DO $$
  try {
    pljs.execute('SELECT $1::refcursor AS c', ['\uDC00']);
  } catch (e) {
    pljs.elog(NOTICE, e.message);
  }
$$ LANGUAGE pljs;

-- 2) In a jsonb string and key.
CREATE FUNCTION u8_jsonb_value() RETURNS jsonb LANGUAGE pljs AS $$
  return {a: '\uDC00'};
$$;
SELECT u8_jsonb_value();

CREATE FUNCTION u8_jsonb_key() RETURNS jsonb LANGUAGE pljs AS $$
  return {['\uD800']: 1};
$$;
SELECT u8_jsonb_key();

CREATE FUNCTION u8_jsonb_pair() RETURNS jsonb LANGUAGE pljs AS $$
  return {'\uD83D\uDE00': 'e\u0301'};
$$;
SELECT encode(convert_to(u8_jsonb_pair()::text, 'UTF8'), 'hex') AS pair;

-- 3) The types with cases of their own were built from QuickJS's bytes too.
CREATE FUNCTION u8_text() RETURNS text LANGUAGE pljs AS $$
  return 'a\uD800b';
$$;
SELECT u8_text();

CREATE FUNCTION u8_varchar() RETURNS varchar(5) LANGUAGE pljs AS $$
  return '\uDC00';
$$;
SELECT u8_varchar();

CREATE FUNCTION u8_name() RETURNS name LANGUAGE pljs AS $$
  return 'n\uD800';
$$;
SELECT u8_name();

-- JSON.stringify() writes a lone surrogate as an escape, which is valid.
CREATE FUNCTION u8_json() RETURNS json LANGUAGE pljs AS $$
  return {a: '\uD800'};
$$;
SELECT u8_json();

CREATE FUNCTION u8_text_pair() RETURNS text LANGUAGE pljs AS $$
  return 'caf\u00e9 \uD83D\uDE00';
$$;
SELECT encode(convert_to(u8_text_pair(), 'UTF8'), 'hex') AS pair;

-- 4) In a LATIN1 database, text is converted both ways.  It was given to
-- QuickJS as it was stored, so an accented letter became U+FFFD, and stored
-- as QuickJS wrote it, as the two bytes of its UTF-8.  Everything below is
-- ASCII, so what is compared is only what pljs converted.
SELECT current_database() AS u8_regress_db \gset
DROP DATABASE IF EXISTS pljs_u8_latin1;
CREATE DATABASE pljs_u8_latin1 ENCODING 'LATIN1' LOCALE 'C'
  LOCALE_PROVIDER libc TEMPLATE template0;
\c pljs_u8_latin1
CREATE EXTENSION pljs;

CREATE TABLE u8_l1 (t text, v varchar(10), c char(3), n name, j json,
                    b jsonb, a text[]);
INSERT INTO u8_l1 VALUES (chr(233), 'caf' || chr(233), chr(252),
                          'n' || chr(241), json_build_object(chr(233), chr(224)),
                          jsonb_build_object(chr(233), chr(224)),
                          ARRAY[chr(233)]);

-- What JavaScript reads, as code points.
DO $$
  const r = pljs.execute('SELECT * FROM u8_l1')[0];
  const cp = (s) => Array.from(s, (ch) => ch.codePointAt(0).toString(16)).join(',');
  const pair = (o) => cp(Object.keys(o)[0]) + ':' + cp(Object.values(o)[0]);

  pljs.elog(NOTICE, [cp(r.t), cp(r.v), cp(r.c), cp(r.n), pair(r.j), pair(r.b),
                     cp(r.a[0])].join(' | '));
$$ LANGUAGE pljs;

-- What JavaScript writes.
CREATE FUNCTION u8_l1_rows() RETURNS SETOF u8_l1 LANGUAGE pljs AS $$
  pljs.return_next({t: '\u00e9', v: 'caf\u00e9', c: '\u00fc', n: 'n\u00f1',
                    j: {'\u00e9': '\u00e0'}, b: {'\u00e9': '\u00e0'},
                    a: ['\u00e9']});
$$;
SELECT encode(convert_to(t, 'LATIN1'), 'hex') AS t,
       encode(convert_to(v, 'LATIN1'), 'hex') AS v,
       encode(convert_to(c, 'LATIN1'), 'hex') AS c,
       encode(convert_to(n::text, 'LATIN1'), 'hex') AS n,
       encode(convert_to(j::text, 'LATIN1'), 'hex') AS j,
       b = jsonb_build_object(chr(233), chr(224)) AS b,
       a = ARRAY[chr(233)] AS a
  FROM u8_l1_rows();

-- And back again, through a trigger that returns NEW unchanged.
CREATE FUNCTION u8_l1_trig() RETURNS trigger LANGUAGE pljs AS $$
  return NEW;
$$;
CREATE TRIGGER u8_l1_trig BEFORE UPDATE ON u8_l1
  FOR EACH ROW EXECUTE FUNCTION u8_l1_trig();
UPDATE u8_l1 SET t = t;
SELECT t = chr(233) AS t, v = 'caf' || chr(233) AS v, c = chr(252) AS c,
       n = 'n' || chr(241) AS n,
       j::jsonb = jsonb_build_object(chr(233), chr(224)) AS j,
       b = jsonb_build_object(chr(233), chr(224)) AS b,
       a = ARRAY[chr(233)] AS a
  FROM u8_l1;

-- A character LATIN1 has no byte for.
CREATE FUNCTION u8_l1_euro() RETURNS text LANGUAGE pljs AS $$
  return '\u20ac';
$$;
SELECT u8_l1_euro();

\c :u8_regress_db
DROP DATABASE pljs_u8_latin1;

DROP FUNCTION u8_lone(), u8_pair(), u8_jsonb_value(), u8_jsonb_key(),
  u8_jsonb_pair(), u8_text(), u8_varchar(), u8_name(), u8_json(),
  u8_text_pair();
