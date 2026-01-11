CREATE FUNCTION valid_arraybuffer_bytea(len integer) RETURNS bytea
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  arr = new ArrayBuffer(len);
  for(i = 0; i < len; i++) {
    arr[i] = i;
  }

  return arr;
$$;

SELECT length(valid_arraybuffer_bytea(20));

CREATE FUNCTION valid_int8array_bytea(len integer) RETURNS bytea
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  return new Int8Array(len);
$$;

SELECT length(valid_int8array_bytea(20));

CREATE FUNCTION valid_int16array_bytea(len integer) RETURNS bytea
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  return new Int16Array(len);
$$;

SELECT length(valid_int16array_bytea(20));

CREATE FUNCTION filled_int8array_bytea() RETURNS bytea
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  arr = new Int8Array(4);
  arr[0] = 1;
  arr[1] = 2;
  arr[2] = 3;
  arr[3] = 4;

  return arr;
$$;

SELECT filled_int8array_bytea() = '\x01020304'::bytea;

CREATE FUNCTION filled_int16array_bytea() RETURNS bytea
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  arr = new Int16Array(4);
  arr[0] = 1;
  arr[1] = 2;
  arr[2] = 3;
  arr[3] = 4;

  return arr;
$$;

SELECT filled_int16array_bytea();

CREATE FUNCTION filled_int32array_bytea() RETURNS bytea
LANGUAGE pljs IMMUTABLE STRICT
AS $$
  arr = new Int32Array(4);
  arr[0] = 1;
  arr[1] = 2;
  arr[2] = 3;
  arr[3] = 4;

  return arr;
$$;

SELECT filled_int32array_bytea();

DO $$
  const test = 'string test';
  const res = pljs.execute(`select $1::bytea`, [test]);
  const result = res[0].bytea;

  if (result === test) {
    pljs.elog(INFO, 'OK');
  } else {
    pljs.elog(WARNING, 'FAIL');
  }
$$ language pljs;
