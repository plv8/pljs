-- Typed-array -> bytea conversion, including 32-bit widths and offset views.
--
-- Regression test for a heap-buffer overflow in the Uint32Array / Int32Array
-- -> bytea path in src/types.c: the copy loop iterated 4*length times while the
-- destination held only length uint32 slots, overwriting 3*length slots past
-- the end of the allocation. (The 8- and 16-bit paths already looped over
-- length correctly.) The fix bounds the 32-bit loop by length. This pins the
-- correct little-endian bytes for 8/16/32-bit views, an offset view
-- (new Uint32Array(ab, byteOffset, n)), and that cursor.fetch still works when
-- invoked through Function.prototype.apply.
CREATE FUNCTION ta_u8()  RETURNS bytea LANGUAGE pljs AS $$ return new Uint8Array([1, 2, 3, 255]); $$;
CREATE FUNCTION ta_u16() RETURNS bytea LANGUAGE pljs AS $$ return new Uint16Array([1, 258]); $$;
CREATE FUNCTION ta_u32() RETURNS bytea LANGUAGE pljs AS $$ return new Uint32Array([1, 2, 3]); $$;
CREATE FUNCTION ta_i32() RETURNS bytea LANGUAGE pljs AS $$ return new Int32Array([-1, 256]); $$;
CREATE FUNCTION ta_off() RETURNS bytea LANGUAGE pljs AS $$
  const ab = new ArrayBuffer(16);
  const full = new Uint32Array(ab);
  full[0] = 9; full[1] = 1; full[2] = 2; full[3] = 9;
  return new Uint32Array(ab, 4, 2);   // view over elements 1,2 only
$$;
SELECT ta_u8() AS u8, ta_u16() AS u16, ta_u32() AS u32, ta_i32() AS i32, ta_off() AS off;

DO $$
  const cur = pljs.prepare('SELECT 42 AS x').cursor();
  const row = cur.fetch.apply(cur);
  pljs.elog(NOTICE, 'fetch.apply: ' + JSON.stringify(row));
  cur.close();
$$ LANGUAGE pljs;

DROP FUNCTION ta_u8();
DROP FUNCTION ta_u16();
DROP FUNCTION ta_u32();
DROP FUNCTION ta_i32();
DROP FUNCTION ta_off();
