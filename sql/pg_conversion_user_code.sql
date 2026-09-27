-- JavaScript that runs while a value is being converted.
--
-- Converting a JavaScript value can run the value's own code -- a getter,
-- valueOf(), toString(), toJSON() -- and converting a PostgreSQL value used
-- to run code the value never asked for, a setter it inherited.

-- 1) An exception thrown during a conversion is the error.  Most paths
-- reported "could not convert JavaScript value to a number" or "... to a
-- string" instead; float8 and numeric became NaN, oid became 0, a jsonb
-- property became "", an array whose length threw became {} and a fallback
-- type's {is_null} getter became SQL NULL, all silently; and json crashed the
-- backend.
CREATE TYPE cuc_pair AS (a int4, b int4);
CREATE FUNCTION cuc_record(OUT a int4, OUT b int4) LANGUAGE pljs AS $$
  return { get a() { throw new Error('record getter'); }, b: 1 };
$$;
SELECT * FROM cuc_record();

CREATE FUNCTION cuc_int4() RETURNS int4 LANGUAGE pljs AS $$
  return { valueOf() { throw new Error('int4 valueOf'); } };
$$;
SELECT cuc_int4();

CREATE FUNCTION cuc_float8() RETURNS float8 LANGUAGE pljs AS $$
  return { valueOf() { throw new Error('float8 valueOf'); } };
$$;
SELECT cuc_float8();

CREATE FUNCTION cuc_numeric() RETURNS numeric LANGUAGE pljs AS $$
  return { valueOf() { throw new Error('numeric valueOf'); } };
$$;
SELECT cuc_numeric();

CREATE FUNCTION cuc_oid() RETURNS oid LANGUAGE pljs AS $$
  return { valueOf() { throw new Error('oid valueOf'); } };
$$;
SELECT cuc_oid();

CREATE FUNCTION cuc_text() RETURNS text LANGUAGE pljs AS $$
  return { toString() { throw new Error('text toString'); } };
$$;
SELECT cuc_text();

CREATE FUNCTION cuc_uuid() RETURNS uuid LANGUAGE pljs AS $$
  return { toString() { throw new Error('uuid toString'); } };
$$;
SELECT cuc_uuid();

CREATE FUNCTION cuc_uuid_is_null() RETURNS uuid LANGUAGE pljs AS $$
  return { get is_null() { throw new Error('is_null getter'); } };
$$;
SELECT cuc_uuid_is_null();

CREATE FUNCTION cuc_json() RETURNS json LANGUAGE pljs AS $$
  return { toJSON() { throw new Error('json toJSON'); } };
$$;
SELECT cuc_json();

CREATE FUNCTION cuc_jsonb() RETURNS jsonb LANGUAGE pljs AS $$
  return { get a() { throw new Error('jsonb getter'); } };
$$;
SELECT cuc_jsonb();

CREATE FUNCTION cuc_array_element() RETURNS int4[] LANGUAGE pljs AS $$
  const a = [1, 2];
  Object.defineProperty(a, 1, { get() { throw new Error('element getter'); } });
  return a;
$$;
SELECT cuc_array_element();

CREATE FUNCTION cuc_array_length() RETURNS int4[] LANGUAGE pljs AS $$
  return new Proxy([1, 2], {
    get(target, key) {
      if (key === 'length') {
        throw new Error('length getter');
      }
      return target[key];
    }
  });
$$;
SELECT cuc_array_length();

CREATE FUNCTION cuc_date() RETURNS timestamptz LANGUAGE pljs AS $$
  const d = new Date(1000);
  d.valueOf = () => { throw new Error('date valueOf'); };
  return d;
$$;
SELECT cuc_date();

-- A PostgreSQL error raised from a getter keeps its SQLSTATE.
CREATE FUNCTION cuc_getter_sql_error(OUT a int4, OUT b int4) LANGUAGE pljs AS $$
  return { get a() { return pljs.execute('SELECT 1 / 0 AS x')[0].x; }, b: 1 };
$$;
SELECT * FROM cuc_getter_sql_error();
\echo :LAST_ERROR_SQLSTATE

-- A bound parameter's error reaches the JavaScript that bound it.
DO $$
  try {
    pljs.execute('SELECT $1::int4 AS x', [{ valueOf() { throw new Error('param valueOf'); } }]);
  } catch (e) {
    pljs.elog(NOTICE, 'caught: ' + e.message);
  }
$$ LANGUAGE pljs;

-- 2) Rows a single-column set returns take the same forms return_next()
-- accepts.  `[{a: 7}]` converted the object itself as the int4.
CREATE FUNCTION cuc_rows() RETURNS TABLE (a int4) LANGUAGE pljs AS $$
  return [{ a: 7 }, { get a() { return 8; } }, { b: 9 }, 10,
          { valueOf() { return 11; } }];
$$;
SELECT * FROM cuc_rows();

CREATE FUNCTION cuc_row() RETURNS TABLE (a int4) LANGUAGE pljs AS $$
  return { a: 12 };
$$;
SELECT * FROM cuc_row();

CREATE FUNCTION cuc_row_ambiguous() RETURNS TABLE (a int4) LANGUAGE pljs AS $$
  return [{ x: 1, y: 2 }];
$$;
SELECT * FROM cuc_row_ambiguous();

-- A json column still takes the object as its value.
CREATE FUNCTION cuc_json_rows() RETURNS TABLE (j jsonb) LANGUAGE pljs AS $$
  return [{ j: 1 }, { x: 2 }];
$$;
SELECT * FROM cuc_json_rows();

-- 3) Objects and arrays built from PostgreSQL values define their properties
-- rather than set them.  Setting ran setters inherited from Object.prototype
-- and Array.prototype -- during argument conversion, before the call had an
-- SPI connection -- and the setter took the value: here the array's first
-- element and the SPI result's first row, which made rows[0] undefined.  A
-- column named __proto__ replaced the row object's prototype instead of
-- becoming a property.
CREATE FUNCTION cuc_install() RETURNS void LANGUAGE pljs AS $$
  globalThis.cuc_hits = '';
  Object.defineProperty(Object.prototype, 'a', {
    set(v) { globalThis.cuc_hits += 'a=' + v + ';'; },
    configurable: true
  });
  Object.defineProperty(Array.prototype, 0, {
    set(v) { globalThis.cuc_hits += '[0]=' + v + ';'; },
    configurable: true
  });
$$;
CREATE FUNCTION cuc_uninstall() RETURNS text LANGUAGE pljs AS $$
  delete Object.prototype.a;
  delete Array.prototype[0];
  return globalThis.cuc_hits;
$$;
CREATE FUNCTION cuc_values(r cuc_pair, xs int4[], j jsonb) RETURNS text
  LANGUAGE pljs AS $$
  const rows = pljs.execute('SELECT 5 AS a');
  const hits = globalThis.cuc_hits;

  return [r.a, xs[0], xs.length, j.a, j.list[0], rows[0].a,
          'hits: ' + hits].join(' ');
$$;
SELECT cuc_install();
SELECT cuc_values((7, 1)::cuc_pair, ARRAY[1, 2], '{"a": 3, "list": [4]}');
SELECT cuc_uninstall() = '' AS no_setter_ran;

CREATE FUNCTION cuc_proto() RETURNS text LANGUAGE pljs AS $$
  const r = pljs.execute('SELECT 1 AS "__proto__", 2 AS x')[0];

  return JSON.stringify([Object.keys(r),
                         Object.getPrototypeOf(r) === Object.prototype]);
$$;
SELECT cuc_proto();

DROP FUNCTION cuc_record, cuc_int4, cuc_float8, cuc_numeric, cuc_oid, cuc_text,
              cuc_uuid, cuc_uuid_is_null, cuc_json, cuc_jsonb,
              cuc_array_element, cuc_array_length, cuc_date,
              cuc_getter_sql_error, cuc_rows, cuc_row, cuc_row_ambiguous,
              cuc_json_rows, cuc_install, cuc_uninstall, cuc_values, cuc_proto;
DROP TYPE cuc_pair;
