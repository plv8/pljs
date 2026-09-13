-- Cursor/plan lifetime: a cursor must keep its plan alive.
--
-- pljs_plan_handle_finalizer() calls SPI_freeplan(), and it runs as soon as the
-- plan handle becomes unreachable.  The natural idiom makes that immediate,
-- because QuickJS is refcount-primary -- the temporary plan object's count drops
-- to zero the moment .cursor() returns:
--
--     var c = pljs.prepare('select ... where id = $1', ['int']).cursor([1]);
--     while (c.fetch()) { ... }
--
-- SPI_freeplan -> DropCachedPlan sets plansource->magic = 0 and deletes the
-- plansource's context, while the portal opened from it is still live and gets
-- re-entered by fetch/move/close.  SPI_freeplan's own contract says a plan in
-- use must not be freed.
--
-- HONEST NOTE ON WHAT THIS TEST PROVES: it does not discriminate.  All three
-- cases below pass with and without the fix, on a build that has
-- CLOBBER_FREED_MEMORY enabled (--enable-cassert defines it), including after
-- deliberately churning plancache and palloc memory to encourage reuse of the
-- freed plansource.  The portal holds a refcount on the CachedPlan rather than
-- on the CachedPlanSource and does not appear to dereference the plansource
-- during a fetch, so the contract violation does not surface as a crash here.
--
-- The tests are kept because they pin the observable behaviour -- correct row
-- counts across GC and across an explicit free -- so a future change that breaks
-- cursors outright is caught.  The fix itself is justified by SPI's contract, not
-- by a reproduction.
CREATE FUNCTION curlife_gc() RETURNS int AS $$
  // The plan is unreachable immediately after .cursor() returns.
  const c = pljs.prepare('SELECT i FROM generate_series(1, 50) i', []).cursor([]);
  pljs.gc();
  pljs.gc();
  let n = 0;
  while (c.fetch()) n++;
  c.close();
  return n;
$$ LANGUAGE pljs;

SELECT curlife_gc() AS rows_after_gc;

-- An explicit free() while the cursor is open must not corrupt memory.  It
-- currently still returns rows; pljs_plan_free()'s JS_SetOpaque(ptr, NULL) is
-- what stops the finalizer double-freeing afterwards.
CREATE FUNCTION curlife_explicit_free() RETURNS text AS $$
  const p = pljs.prepare('SELECT i FROM generate_series(1, 50) i', []);
  const c = p.cursor([]);
  p.free();
  try {
    let n = 0;
    while (c.fetch()) n++;
    c.close();
    return 'fetched ' + n;
  } catch (e) {
    return 'raised: ' + e.message;
  }
$$ LANGUAGE pljs;

SELECT curlife_explicit_free() AS after_explicit_free;

-- Same as the first case, but churning plancache and palloc memory between the
-- collection and the fetch, which is the shape most likely to reuse a freed
-- plansource.
CREATE FUNCTION curlife_churn() RETURNS int AS $$
  const c = pljs.prepare('SELECT i FROM generate_series(1, 200) i', []).cursor([]);
  pljs.gc();
  for (let i = 0; i < 200; i++) {
    pljs.execute('SELECT repeat($1, 50) AS r', ['x' + i]);
    const p2 = pljs.prepare('SELECT $1::int AS v', ['int4']);
    p2.execute([i]);
    p2.free();
  }
  pljs.gc();
  let n = 0;
  while (c.fetch()) n++;
  c.close();
  return n;
$$ LANGUAGE pljs;

SELECT curlife_churn() AS rows_after_churn;

-- move() and close() go through the same portal, so exercise them too.
CREATE FUNCTION curlife_move() RETURNS int AS $$
  const c = pljs.prepare('SELECT i FROM generate_series(1, 100) i', []).cursor([]);
  pljs.gc();
  c.move(10);
  const row = c.fetch();
  c.close();
  return row.i;
$$ LANGUAGE pljs;

SELECT curlife_move() AS row_after_move;

DROP FUNCTION curlife_gc, curlife_explicit_free, curlife_churn, curlife_move;
