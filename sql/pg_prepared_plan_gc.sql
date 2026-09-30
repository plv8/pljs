-- Regression: a prepared statement that JavaScript never .free()'s used to
-- leak its saved SPI plan plus CacheMemoryContext parstate for the life of the
-- backend.  Preparing 20k plans in a loop grew CacheMemoryContext by ~64MB
-- with no way to reclaim it -- exactly the kind of slow leak a long-running
-- worker hits.  The prepared-statement handle class now has a GC finalizer, so
-- once the handle becomes unreachable the plan is reclaimed and growth stays
-- bounded by the GC interval.
CREATE EXTENSION IF NOT EXISTS pljs;

-- Prepare many plans without ever calling free(); after a GC the CacheMemory
-- growth must stay far below the ~64MB the leak used to produce.  We compare in
-- float8 so the arithmetic stays in JS Number space (mixing BigInt and Number
-- throws a TypeError).
CREATE FUNCTION prep_no_free(n int) RETURNS boolean LANGUAGE pljs AS $$
  // Match the leak-bearing contexts by name rather than by parent:
  // pg_backend_memory_contexts dropped the `parent` column in PostgreSQL 18
  // (replaced by `type` and `path`), so a parent-based query does not even
  // parse there.  An unfreed plan leaves behind a CachedPlanSource and an
  // "SPI Plan" context apiece, so 20k of them is tens of MB -- while the fixed
  // build stays around 5KB.
  var q = "SELECT sum(total_bytes)::float8 AS b FROM pg_backend_memory_contexts" +
          " WHERE name IN ('CacheMemoryContext', 'CachedPlanSource'," +
          " 'CachedPlanQuery', 'SPI Plan')";
  var before = pljs.execute(q)[0].b;
  for (var i = 0; i < n; i++) {
    var p = pljs.prepare("SELECT $1::int + " + i, ["int"]);
    p.execute([i]);
  }
  pljs.gc();
  var after = pljs.execute(q)[0].b;
  return (after - before) < 10 * 1024 * 1024;
$$;

SELECT prep_no_free(20000) AS leak_bounded;

-- Explicitly freeing a plan and then triggering GC must not double-free the
-- underlying SPI plan (the free path clears the handle's opaque).
CREATE FUNCTION free_then_gc() RETURNS int LANGUAGE pljs AS $$
  var p = pljs.prepare("SELECT 1");
  p.execute();
  p.free();
  pljs.gc();
  return 1;
$$;

SELECT free_then_gc();
SELECT 1 AS alive;

DROP FUNCTION prep_no_free(int);
DROP FUNCTION free_then_gc();
