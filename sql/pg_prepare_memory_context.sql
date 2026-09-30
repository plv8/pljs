-- pljs.prepare() leaves CurrentMemoryContext where it found it.
--
-- SPI_prepare() and SPI_saveplan() return with CurrentMemoryContext set to
-- SPI's procedure context, and pljs.prepare() did not switch back.  Called
-- from a getter or toString() on a function's result, it left the rest of the
-- result to be built there, and SPI_finish() freed it before the result was
-- returned: the first column below read the second call's value, from memory
-- the first call had freed and the second reused.

CREATE FUNCTION pmc_text(tag text) RETURNS text LANGUAGE pljs AS $$
  return {
    toString() {
      pljs.prepare('SELECT 1');
      return tag + ':' + 'y'.repeat(40);
    }
  };
$$;
SELECT pmc_text('first') AS a, pmc_text('second') AS b;

-- A field of a composite result.
CREATE TYPE pmc_pair AS (a text, b text);
CREATE FUNCTION pmc_pair(tag text) RETURNS pmc_pair LANGUAGE pljs AS $$
  return {
    get a() {
      pljs.prepare('SELECT 1');
      return tag + ':' + 'y'.repeat(40);
    },
    b: tag
  };
$$;
SELECT (pmc_pair('first')).a AS a1, (pmc_pair('second')).a AS a2;

DROP FUNCTION pmc_text(text);
DROP FUNCTION pmc_pair(text);
DROP TYPE pmc_pair;
