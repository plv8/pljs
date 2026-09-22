-- Types without a dedicated case convert through their own I/O functions.
--
-- pljs_datum_to_jsvalue() has a case per type for the handful it knows about --
-- the integers, float, numeric, text, json, bytea, the date/timestamp family --
-- and sends everything else to a fallback.  "Everything else" is most of the
-- type system: uuid, inet, cidr, macaddr, interval, time, timetz, the geometric
-- types, bit and bit varying, money, tsvector, every enum, every domain, and
-- every extension type such as ltree.
--
-- That fallback used to hand JavaScript the datum's *internal* bytes, and take
-- them back the same way, which was wrong in three separate directions:
--
--   1. JS_NewStringLen() decodes UTF-8, so every byte above 0x7F became U+FFFD.
--      A uuid round-tripped 0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f into
--      01efbfbd-efbf-bd3a-4b7c-5defbfbd0a1b.  A BEFORE INSERT trigger that
--      returns NEW corrupts a uuid primary key this way, which surfaces much
--      later as a duplicate key on an unrelated row.
--
--   2. Coming back, the string's bytes were memcpy'd over the type's internal
--      representation with nothing validating them, so the datum was not just
--      wrong but malformed.  Returning a `bit varying` unchanged built a varbit
--      whose bit length disagreed with its allocation and crashed the backend
--      in varbit_out().
--
--   3. Pass-by-value types went through int32.  money lost its high half, time
--      came back as 00:4294967264:28.640256, and an enum reached JavaScript as
--      its pg_enum OID rather than its label.
--
-- Both directions now use the type's text I/O functions, which is what psql,
-- COPY and to_json() do, and what plv8 does for the same set of types.
CREATE EXTENSION IF NOT EXISTS pljs;

-- money, interval, timestamp and bytea all render through settings that vary
-- by environment; pin them so the expected output does not depend on the
-- server's defaults.
SET lc_monetary = 'C';
SET DateStyle = 'ISO, MDY';
SET IntervalStyle = 'postgres';
SET bytea_output = 'hex';

-- 1) The reported case: a uuid survives a round trip.
CREATE FUNCTION fb_uuid(u uuid) RETURNS uuid LANGUAGE pljs AS $$ return u; $$;
SELECT fb_uuid('0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f');

-- ...and reaches JavaScript as the text a user would recognise, not 16 bytes
-- of UTF-8 wreckage.
CREATE FUNCTION fb_uuid_seen(u uuid) RETURNS text LANGUAGE pljs AS $$
  return typeof u + ' of length ' + u.length + ': ' + u;
$$;
SELECT fb_uuid_seen('0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f');

-- 2) The trigger path, where the corruption is silent and lands on disk.
CREATE TABLE fb_t (id uuid PRIMARY KEY, n int);
CREATE FUNCTION fb_trig() RETURNS trigger LANGUAGE pljs AS $$ NEW.n = 2; return NEW; $$;
CREATE TRIGGER fb_trig BEFORE INSERT ON fb_t FOR EACH ROW EXECUTE FUNCTION fb_trig();
INSERT INTO fb_t VALUES ('0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f', 1);
-- Two distinct keys stay distinct.  Every uuid used to collapse onto whatever
-- byte sequence the replacement characters encoded to, so this second insert
-- failed on the primary key.
INSERT INTO fb_t VALUES ('0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e60', 3);
SELECT * FROM fb_t ORDER BY id;

-- 3) Pass-by-reference types, fixed length and varlena alike.
CREATE FUNCTION fb_inet(v inet) RETURNS inet LANGUAGE pljs AS $$ return v; $$;
SELECT fb_inet('192.168.1.100/24');

CREATE FUNCTION fb_cidr(v cidr) RETURNS cidr LANGUAGE pljs AS $$ return v; $$;
SELECT fb_cidr('2001:db8::/32');

CREATE FUNCTION fb_interval(v interval) RETURNS interval LANGUAGE pljs AS $$ return v; $$;
SELECT fb_interval('1 year 2 months 3 days 04:05:06');

CREATE FUNCTION fb_point(v point) RETURNS point LANGUAGE pljs AS $$ return v; $$;
SELECT fb_point('(1.5,-2.25)');

-- bit varying is the one that crashed the backend rather than merely corrupting.
CREATE FUNCTION fb_varbit(v varbit) RETURNS varbit LANGUAGE pljs AS $$ return v; $$;
SELECT fb_varbit(B'10110');

CREATE FUNCTION fb_tsvector(v tsvector) RETURNS tsvector LANGUAGE pljs AS $$ return v; $$;
SELECT fb_tsvector('a fat cat'::tsvector);

-- 4) Pass-by-value types.  The fallback used to squeeze these through int32,
-- which is why fixing only the pass-by-reference half is not enough.
CREATE FUNCTION fb_time(v time) RETURNS time LANGUAGE pljs AS $$ return v; $$;
SELECT fb_time('12:34:56');

-- money is int64: the high half went missing entirely.
CREATE FUNCTION fb_money(v money) RETURNS money LANGUAGE pljs AS $$ return v; $$;
SELECT fb_money('12.34'), fb_money('92233720368.54');

-- An enum is a 4-byte OID by value, so it round-tripped only because both
-- directions agreed to pass the raw OID around; JavaScript never saw the label.
CREATE TYPE fb_mood AS ENUM ('sad', 'ok', 'happy');
CREATE FUNCTION fb_enum_seen(v fb_mood) RETURNS text LANGUAGE pljs AS $$
  return typeof v + ': ' + v;
$$;
SELECT fb_enum_seen('happy');

CREATE FUNCTION fb_enum_upper(v fb_mood) RETURNS text LANGUAGE pljs AS $$
  return v.toUpperCase();
$$;
SELECT fb_enum_upper('happy');

CREATE FUNCTION fb_enum(v fb_mood) RETURNS fb_mood LANGUAGE pljs AS $$ return v; $$;
SELECT fb_enum('ok');

CREATE FUNCTION fb_xid(v xid) RETURNS xid LANGUAGE pljs AS $$ return v; $$;
SELECT fb_xid('42');

-- 5) A value the type rejects now raises, where it used to be written as
-- whatever the bytes happened to mean.
CREATE FUNCTION fb_bad_uuid() RETURNS uuid LANGUAGE pljs AS $$ return 'not-a-uuid'; $$;
SELECT fb_bad_uuid();

CREATE FUNCTION fb_bad_enum() RETURNS fb_mood LANGUAGE pljs AS $$ return 'furious'; $$;
SELECT fb_bad_enum();

-- ...and it is an ordinary catchable error, not a crash.
DO $$
  try {
    pljs.execute("SELECT fb_bad_uuid()");
    pljs.elog(NOTICE, 'unexpectedly succeeded');
  } catch (e) {
    pljs.elog(NOTICE, 'raised: ' + (e.message.indexOf('uuid') >= 0));
  }
$$ LANGUAGE pljs;

-- 6) NULL still arrives as null and goes back as NULL.
CREATE FUNCTION fb_null(u uuid) RETURNS uuid LANGUAGE pljs AS $$
  pljs.elog(NOTICE, 'argument is null: ' + (u === null));
  return null;
$$;
SELECT fb_null(NULL) IS NULL AS returned_null;

-- 7) A domain is converted as its base type, and then checked as the domain.
--
-- A domain has its own OID and its own pg_type row, so it matches none of the
-- cases in pljs_datum_to_jsvalue() and lands here.  That is right for a domain
-- over uuid and wrong for a domain over anything pljs has a case for: routing
-- those through the output function too would hand JavaScript the string "5"
-- for a domain over int4, so `return v + 1` would answer "51", and "f" for a
-- domain over boolean, which is truthy.  Dispatching on the base type fixes
-- that; running domain_check() afterwards is what keeps the constraints.
CREATE DOMAIN fb_even_uuid AS uuid CHECK (VALUE IS NOT NULL);
CREATE FUNCTION fb_domain(v fb_even_uuid) RETURNS fb_even_uuid LANGUAGE pljs AS $$ return v; $$;
SELECT fb_domain('0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f');

-- A domain over a type with a case of its own keeps that case's JavaScript
-- type, rather than becoming a string.
CREATE DOMAIN fb_dint AS int4;
CREATE FUNCTION fb_dint_seen(v fb_dint) RETURNS text LANGUAGE pljs AS $$
  return typeof v + ':' + v;
$$;
SELECT fb_dint_seen(5);

CREATE FUNCTION fb_dint_add(v fb_dint) RETURNS fb_dint LANGUAGE pljs AS $$ return v + 1; $$;
SELECT fb_dint_add(5);

CREATE DOMAIN fb_dbool AS boolean;
CREATE FUNCTION fb_dbool_seen(v fb_dbool) RETURNS text LANGUAGE pljs AS $$
  return typeof v + ':' + v + ' truthy=' + (v ? 'yes' : 'no');
$$;
SELECT fb_dbool_seen(false);

CREATE FUNCTION fb_dbool_not(v fb_dbool) RETURNS fb_dbool LANGUAGE pljs AS $$ return !v; $$;
SELECT fb_dbool_not(false);

-- jsonb arrives parsed, and goes back as a value the jsonb case stringifies,
-- not as "[object Object]".
CREATE DOMAIN fb_djsonb AS jsonb;
CREATE FUNCTION fb_djsonb_seen(v fb_djsonb) RETURNS text LANGUAGE pljs AS $$
  return typeof v + ':' + v.a;
$$;
SELECT fb_djsonb_seen('{"a": 1}');

CREATE FUNCTION fb_djsonb_add(v fb_djsonb) RETURNS fb_djsonb LANGUAGE pljs AS $$
  v.b = 2;
  return v;
$$;
SELECT fb_djsonb_add('{"a": 1}');

-- A domain over int8 keeps the exactness the int8 case gives it; the old
-- pass-by-value fallback truncated this to 1.
CREATE DOMAIN fb_dint8 AS int8;
CREATE FUNCTION fb_dint8_echo(v fb_dint8) RETURNS fb_dint8 LANGUAGE pljs AS $$ return v; $$;
SELECT fb_dint8_echo(9007199254740993);

-- The domain's own constraints still apply to whatever JavaScript returns.
CREATE DOMAIN fb_dpos AS int4 CHECK (VALUE > 0);
CREATE FUNCTION fb_dpos_bad() RETURNS fb_dpos LANGUAGE pljs AS $$ return -5; $$;
SELECT fb_dpos_bad();

CREATE DOMAIN fb_dnn AS int4 NOT NULL;
CREATE FUNCTION fb_dnn_null() RETURNS fb_dnn LANGUAGE pljs AS $$ return null; $$;
SELECT fb_dnn_null();

-- A domain over a domain resolves all the way down, and every constraint in
-- the chain is still checked.
CREATE DOMAIN fb_dpos_small AS fb_dpos CHECK (VALUE < 100);
CREATE FUNCTION fb_nested(v fb_dpos_small) RETURNS fb_dpos_small LANGUAGE pljs AS $$
  return v * 10;
$$;
SELECT fb_nested(5);
SELECT fb_nested(50);

-- Building the base type's datum is also what lets a domain accept the
-- JavaScript values its base type's case accepts.  Through the fallback, a
-- Uint8Array returned for a domain over bytea stringified to "1,2,3" and was
-- stored as those five characters, and a Date returned for a domain over
-- timestamp went through whatever timestamp_in() made of Date.toString().
CREATE DOMAIN fb_dbytea AS bytea;
CREATE FUNCTION fb_dbytea_ret() RETURNS fb_dbytea LANGUAGE pljs AS $$
  return new Uint8Array([1, 2, 3]);
$$;
SELECT fb_dbytea_ret();

CREATE DOMAIN fb_dts AS timestamp;
CREATE FUNCTION fb_dts_seen(v fb_dts) RETURNS text LANGUAGE pljs AS $$
  return typeof v + ' isDate=' + (v instanceof Date) + ' year=' + v.getUTCFullYear();
$$;
SELECT fb_dts_seen('2020-03-04 05:06:07');

CREATE FUNCTION fb_dts_ret() RETURNS fb_dts LANGUAGE pljs AS $$
  return new Date(Date.UTC(2021, 0, 2, 3, 4, 5));
$$;
SELECT fb_dts_ret();

-- A domain used as an array element, and as a composite column, reaches the
-- conversion through a different path each time.
CREATE FUNCTION fb_dint_array(v fb_dint[]) RETURNS fb_dint[] LANGUAGE pljs AS $$
  return v.map(function (x) { return x + 1; });
$$;
SELECT fb_dint_array(ARRAY[1, 2, 3]::fb_dint[]);

-- ...and the constraint is still enforced per element.
CREATE FUNCTION fb_dpos_array() RETURNS fb_dpos[] LANGUAGE pljs AS $$ return [1, -2]; $$;
SELECT fb_dpos_array();

CREATE TYPE fb_dcomp AS (i fb_dint, ts fb_dts);
CREATE FUNCTION fb_dcomp_seen(v fb_dcomp) RETURNS text LANGUAGE pljs AS $$
  return typeof v.i + '/' + (v.ts instanceof Date);
$$;
SELECT fb_dcomp_seen(ROW(7, '2020-03-04 05:06:07')::fb_dcomp);

-- 8) Arrays and composites convert element by element and column by column, so
-- they reach the same fallback.
CREATE FUNCTION fb_uuid_array(v uuid[]) RETURNS uuid[] LANGUAGE pljs AS $$ return v; $$;
SELECT fb_uuid_array(ARRAY['0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f',
                           '0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e60']::uuid[]);

CREATE TYPE fb_comp AS (id uuid, addr inet, when_ interval);
CREATE FUNCTION fb_composite(v fb_comp) RETURNS fb_comp LANGUAGE pljs AS $$ return v; $$;
SELECT fb_composite(ROW('0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f',
                        '10.0.0.1/8', '3 days')::fb_comp);

-- 9) A toasted value.  The old code detoasted by hand; the output function does
-- it itself, so a value long enough to be pushed out of line still reads back.
CREATE TABLE fb_toast (v varbit);
INSERT INTO fb_toast SELECT repeat('1011', 40000)::varbit;
CREATE FUNCTION fb_len(v varbit) RETURNS int LANGUAGE pljs AS $$ return v.length; $$;
SELECT length(v) AS stored_bits, fb_len(v) AS seen_by_js FROM fb_toast;

-- 10) The same conversion is used for SPI parameters and results.
CREATE FUNCTION fb_spi() RETURNS text LANGUAGE pljs AS $$
  const rows = pljs.execute('SELECT $1::uuid AS u, $2::inet AS a',
                            ['0192f1c2-3a4b-7c5d-8e6f-0a1b2c3d4e5f', '10.1.2.3']);
  return rows[0].u + ' / ' + rows[0].a;
$$;
SELECT fb_spi();

DROP TABLE fb_t, fb_toast;
DROP FUNCTION fb_uuid, fb_uuid_seen, fb_trig, fb_inet, fb_cidr, fb_interval,
              fb_point, fb_varbit, fb_tsvector, fb_time, fb_money, fb_enum_seen,
              fb_enum_upper, fb_enum, fb_xid, fb_bad_uuid, fb_bad_enum, fb_null,
              fb_domain, fb_uuid_array, fb_composite, fb_len, fb_spi,
              fb_dint_seen, fb_dint_add, fb_dbool_seen, fb_dbool_not,
              fb_djsonb_seen, fb_djsonb_add, fb_dint8_echo, fb_dpos_bad,
              fb_dnn_null, fb_nested, fb_dbytea_ret, fb_dts_seen, fb_dts_ret,
              fb_dint_array, fb_dpos_array, fb_dcomp_seen;
DROP TYPE fb_dcomp;
DROP DOMAIN fb_even_uuid, fb_dbool, fb_djsonb, fb_dint8,
            fb_dpos_small, fb_dpos, fb_dnn, fb_dbytea, fb_dts, fb_dint;
DROP TYPE fb_comp;
DROP TYPE fb_mood;
