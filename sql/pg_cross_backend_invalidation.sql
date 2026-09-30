-- A function replaced by *another* backend must not keep running the old body
-- here.
--
-- pljs caches the compiled function per (user, OID) and registers no syscache
-- invalidation callback, so nothing tells this backend that the definition
-- changed.  The validator drops the entry directly, which covers only the backend
-- that issued the DDL.  Every other session that had already called the function
-- went on running the body it first compiled, for the rest of its life, however
-- many times the function was replaced.
--
-- pljs_func already declared fn_xmin and fn_tid for this purpose; nothing ever
-- wrote or read them.  They are now recorded at compile time and checked on every
-- cache hit, which is what plpgsql does (see plpgsql_compile()).  CREATE OR
-- REPLACE writes a new pg_proc tuple version while keeping the OID, so the
-- mismatch is what makes the staleness visible.
--
-- The second backend comes from \!.  psql inherits PGHOST/PGPORT from pg_regress
-- and finds psql on PATH via --bindir, but \! does not interpolate psql
-- variables, so the database name is handed over through the environment with
-- \setenv rather than hardcoded.
CREATE EXTENSION IF NOT EXISTS pljs;

\setenv PGDATABASE :DBNAME

CREATE FUNCTION xb_fn() RETURNS int LANGUAGE pljs AS $$ return 1; $$;

-- Compile and cache it in this backend.
SELECT xb_fn() AS first_call;

-- Replaced elsewhere.  Without the xmin/tid check this reports 1 forever.
\! psql -X -q -c 'CREATE OR REPLACE FUNCTION xb_fn() RETURNS int LANGUAGE pljs AS $$ return 2; $$'
SELECT xb_fn() AS after_other_backend_replaced_it;

-- Again, to show it is not a one-shot invalidation.
\! psql -X -q -c 'CREATE OR REPLACE FUNCTION xb_fn() RETURNS int LANGUAGE pljs AS $$ return 3; $$'
SELECT xb_fn() AS after_second_replacement;

-- A changed signature, not just a changed body: the argument list is part of what
-- the cache entry carries.
\! psql -X -q -c 'CREATE OR REPLACE FUNCTION xb_fn() RETURNS int LANGUAGE pljs AS $$ return 40 + 2; $$'
SELECT xb_fn() AS after_body_rewrite;

-- Same check for a function this backend reaches through pljs.find_function(),
-- which is the other cache lookup site.
CREATE FUNCTION xb_target() RETURNS int LANGUAGE pljs AS $$ return 10; $$;
CREATE FUNCTION xb_caller() RETURNS int LANGUAGE pljs AS $$
  return pljs.find_function('xb_target')();
$$;
SELECT xb_caller() AS caller_first;
\! psql -X -q -c 'CREATE OR REPLACE FUNCTION xb_target() RETURNS int LANGUAGE pljs AS $$ return 20; $$'
SELECT xb_caller() AS caller_after_replacement;

DROP FUNCTION xb_caller();
DROP FUNCTION xb_target();
DROP FUNCTION xb_fn();
