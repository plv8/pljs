set pljs.memory_limit = 64;

do $$
  const limit = pljs.execute(`select setting from pg_settings where name = $1`, ['pljs.memory_limit'])[0].setting;
  const a = new ArrayBuffer(limit*1024*1024/2);
$$ language pljs;

do $$
  const limit = pljs.execute(`select setting from pg_settings where name = $1`, ['pljs.memory_limit'])[0].setting;
  const a = new ArrayBuffer(limit*1024*1024/2);
$$ language pljs;

do $$
  const limit = pljs.execute(`select setting from pg_settings where name = $1`, ['pljs.memory_limit'])[0].setting;
  const a = new ArrayBuffer(limit*1024*1024);
$$ language pljs;

do $$
  const limit = pljs.execute(`select setting from pg_settings where name = $1`, ['pljs.memory_limit'])[0].setting;
  const a = new ArrayBuffer(limit*1024*1024/1.5);
  const s = [];
  while(true) {
    s.push(new ArrayBuffer(63)) // small non-aligned allocations
  }
$$ language pljs;
