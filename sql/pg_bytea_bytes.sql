-- bytea reaches JavaScript as a Uint8Array of its bytes, and goes back
-- unchanged.
--
-- It arrived as a String built from its bytes read as UTF-8: every invalid
-- sequence became U+FFFD and the byte after it was lost, so a value that went
-- back to PostgreSQL was rewritten -- by a trigger that returned NEW, even
-- one that changed some other column.

CREATE FUNCTION bb_echo(b bytea) RETURNS bytea LANGUAGE pljs AS $$
  return b;
$$;
SELECT bb_echo('\xdeadbeef') AS deadbeef,
       bb_echo('\xff00c3') AS ff00c3,
       bb_echo('\x89504e470d0a1a0a') AS png_header,
       bb_echo('\x') AS empty;

CREATE FUNCTION bb_describe(b bytea) RETURNS text LANGUAGE pljs AS $$
  return Object.prototype.toString.call(b) + ' ' + b.length + ': ' +
         Array.from(b).join(',');
$$;
SELECT bb_describe('\xdeadbeef');

-- A trigger that returns NEW, changing only another column.
CREATE TABLE bb_tbl (id int4, note text, data bytea);
CREATE FUNCTION bb_trig() RETURNS trigger LANGUAGE pljs AS $$
  NEW.note = 'seen';
  return NEW;
$$;
CREATE TRIGGER bb_trig BEFORE INSERT OR UPDATE ON bb_tbl
  FOR EACH ROW EXECUTE FUNCTION bb_trig();
INSERT INTO bb_tbl VALUES (1, NULL, '\xdeadbeef'), (2, NULL, '\x89504e47');
UPDATE bb_tbl SET id = id + 10;
SELECT id, note, data FROM bb_tbl ORDER BY id;

-- A domain over bytea.
CREATE DOMAIN bb_blob AS bytea CHECK (length(VALUE) < 8);
CREATE FUNCTION bb_domain(b bb_blob) RETURNS bb_blob LANGUAGE pljs AS $$
  return b;
$$;
SELECT bb_domain('\xdeadbeef');

-- Every byte value, through a query and back as a parameter.
DO LANGUAGE pljs $$
  const all = pljs.execute(
    "SELECT decode(string_agg(lpad(to_hex(i), 2, '0'), ''), 'hex') AS b " +
    "FROM generate_series(0, 255) AS i")[0].b;
  const back = pljs.execute('SELECT $1::bytea AS b', [all])[0].b;
  let same = all.length === 256 && back.length === 256;

  for (let i = 0; i < 256; i++) {
    same = same && all[i] === i && back[i] === i;
  }

  pljs.elog(NOTICE, 'all bytes round trip: ' + same);
$$;

-- A view onto part of a buffer writes the bytes it covers.
CREATE FUNCTION bb_view() RETURNS bytea LANGUAGE pljs AS $$
  const bytes = new Uint8Array([1, 2, 3, 4, 5]);
  return bytes.subarray(1, 4);
$$;
SELECT bb_view();

DROP TABLE bb_tbl;
DROP FUNCTION bb_echo(bytea), bb_describe(bytea), bb_trig(),
  bb_domain(bb_blob), bb_view();
DROP DOMAIN bb_blob;
