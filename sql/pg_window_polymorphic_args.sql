-- Window function arguments resolve to the type of the call being made.
--
-- Window functions are almost always declared over a polymorphic type --
-- js_lag(arg anyelement), js_first_value(arg anyelement) -- so the declared
-- argument type is a pseudo-type and the concrete type is only known per call
-- site.  Two separate places got that wrong.
--
-- convert_arguments_to_javascript() resolves polymorphic arguments for an
-- ordinary call but its window branch passed the declared type straight
-- through.  anyelement is a 4-byte pass-by-value pseudo-type, so the
-- conversion produced JS_NewInt32() of the datum, which for anything
-- pass-by-reference is the truncated *address* of the value: a window
-- function reading its own text argument saw `number:281317736`.
--
-- The winobj.get_func_arg_* accessors had the opposite problem.  They read the
-- resolved type out of pljs_func.argtypes[], which is correct when it is
-- written but is then cached against the function's OID and reused for every
-- later call whatever the types at that call site.  So the first use of a
-- polymorphic window function pinned its argument type for the whole session.
CREATE EXTENSION IF NOT EXISTS pljs;

CREATE TABLE wp (n int, t text, d date);
INSERT INTO wp VALUES (1, 'alpha', '2020-01-01'),
                      (2, 'beta',  '2021-06-15');

-- 1) A window function that reads its own declared argument.
CREATE FUNCTION wp_show(arg anyelement) RETURNS text AS $$
  return typeof arg + ':' + String(arg);
$$ LANGUAGE pljs WINDOW;

SELECT t, wp_show(t) OVER () FROM wp ORDER BY n;
SELECT n, wp_show(n) OVER () FROM wp ORDER BY n;

-- 2) The same argument through winobj.get_func_arg_*.  The text call comes
-- first deliberately: it is what used to pin the cached argument type for the
-- integer call below, which then read the integer itself as a text pointer and
-- crashed the backend.
CREATE FUNCTION wp_prev(arg anyelement) RETURNS anyelement AS $$
  var w = pljs.get_window_object();
  return w.get_func_arg_in_partition(0, -1, w.SEEK_CURRENT, false);
$$ LANGUAGE pljs WINDOW;

SELECT t, wp_prev(t) OVER (ORDER BY n) FROM wp ORDER BY n;
SELECT n, wp_prev(n) OVER (ORDER BY n) FROM wp ORDER BY n;
SELECT d, wp_prev(d) OVER (ORDER BY n) FROM wp ORDER BY n;

CREATE FUNCTION wp_current(arg anyelement) RETURNS text AS $$
  var w = pljs.get_window_object();
  var v = w.get_func_arg_current(0);
  return typeof v + ':' + String(v);
$$ LANGUAGE pljs WINDOW;

SELECT t, wp_current(t) OVER () FROM wp ORDER BY n;
SELECT n, wp_current(n) OVER () FROM wp ORDER BY n;

CREATE FUNCTION wp_frame(arg anyelement) RETURNS anyelement AS $$
  var w = pljs.get_window_object();
  return w.get_func_arg_in_frame(0, 0, w.SEEK_HEAD, false);
$$ LANGUAGE pljs WINDOW;

SELECT t, wp_frame(t) OVER (ORDER BY n) FROM wp ORDER BY n;
SELECT n, wp_frame(n) OVER (ORDER BY n) FROM wp ORDER BY n;

-- 3) The argument number comes from JavaScript, so it is whatever the function
-- author typed.  It used to index both argtypes[] and the executor's own
-- per-argument state unchecked, and took the backend down.
CREATE FUNCTION wp_out_of_range(arg anyelement) RETURNS text AS $$
  var w = pljs.get_window_object();
  return String(w.get_func_arg_current(500));
$$ LANGUAGE pljs WINDOW;

SELECT wp_out_of_range(n) OVER () FROM wp;

CREATE FUNCTION wp_negative(arg anyelement) RETURNS text AS $$
  var w = pljs.get_window_object();
  return String(w.get_func_arg_current(-1));
$$ LANGUAGE pljs WINDOW;

SELECT wp_negative(n) OVER () FROM wp;

-- ...and it is only an argument number if it is already one.  JS_ToInt32()
-- returns the ECMAScript ToInt32 coercion, which turns a non-numeric value
-- into 0 through NaN and wraps anything past 2^31, so these all landed on
-- argument 0 -- inside the range check, and silently the wrong argument.
CREATE FUNCTION wp_coerced(arg anyelement) RETURNS text AS $$
  var w = pljs.get_window_object();
  var out = [];

  [['x', 'x'], ['4294967296', 4294967296], ['1.5', 1.5], ['NaN', NaN]]
    .forEach(function (c) {
      try {
        out.push(c[0] + ' -> ' + String(w.get_func_arg_current(c[1])));
      } catch (e) {
        out.push(c[0] + ' -> ' + e.message);
      }
    });

  return out.join('; ');
$$ LANGUAGE pljs WINDOW;

SELECT wp_coerced(n) OVER () FROM wp ORDER BY n LIMIT 1;

-- A conversion that raises must reach the caller, rather than leaving the
-- exception pending and the argument number at 0.
CREATE FUNCTION wp_throwing_argno(arg anyelement) RETURNS text AS $$
  var w = pljs.get_window_object();
  var bad = { valueOf: function () { throw new Error('boom'); } };

  try {
    return 'no throw, read argument ' + String(w.get_func_arg_current(bad));
  } catch (e) {
    return 'threw: ' + e.message;
  }
$$ LANGUAGE pljs WINDOW;

SELECT wp_throwing_argno(n) OVER () FROM wp ORDER BY n LIMIT 1;

-- 4) `"any"` is a pseudo-type too, but IsPolymorphicType() excludes it, so it
-- reached the conversion unresolved.  In the window branch that produced
-- JS_NewInt32() of the datum -- the truncated address for anything
-- pass-by-reference.
CREATE FUNCTION wp_any("any") RETURNS text AS $$
  return typeof arguments[0] + ':' + String(arguments[0]);
$$ LANGUAGE pljs WINDOW;

SELECT t, wp_any(t) OVER () FROM wp ORDER BY n;
SELECT n, wp_any(n) OVER () FROM wp ORDER BY n;

-- Arguments are converted before the body runs, so this matters even for a
-- function that only ever reads its arguments through the accessors.
CREATE FUNCTION wp_any_acc("any") RETURNS text AS $$
  var w = pljs.get_window_object();
  var v = w.get_func_arg_current(0);
  return typeof v + ':' + String(v);
$$ LANGUAGE pljs WINDOW;

SELECT t, wp_any_acc(t) OVER () FROM wp ORDER BY n;

-- 5) The same unresolved `"any"` in an ordinary call was worse than in the
-- window branch: pljs_type_fill() marks every pseudo-type composite, so the
-- datum was read as a tuple header.  Against text this reported "type with OID
-- 97 does not exist", and against int it crashed the backend.
CREATE FUNCTION wp_any_plain("any") RETURNS text AS $$
  return typeof arguments[0] + ':' + String(arguments[0]);
$$ LANGUAGE pljs;

SELECT wp_any_plain(t) FROM wp ORDER BY n;
SELECT wp_any_plain(n) FROM wp ORDER BY n;

-- 6) VARIADIC "any" is declared with one argument, but the call can supply
-- more.  The accessors must count fcinfo->nargs, not the declared inargs.
CREATE FUNCTION wp_variadic(VARIADIC "any") RETURNS text AS $$
  var w = pljs.get_window_object();
  var out = [];
  for (var i = 0; i < 3; i++) {
    var v = w.get_func_arg_current(i);
    out.push(typeof v + ':' + String(v));
  }
  try {
    w.get_func_arg_current(3);
    out.push('3 in range');
  } catch (e) {
    out.push('3: ' + e.name);
  }
  return out.join(',');
$$ LANGUAGE pljs WINDOW;

SELECT wp_variadic(n, t, t) OVER () FROM wp ORDER BY n;

DROP FUNCTION wp_show, wp_prev, wp_current, wp_frame, wp_out_of_range,
              wp_negative, wp_coerced, wp_throwing_argno, wp_any, wp_any_acc,
              wp_any_plain, wp_variadic;
DROP TABLE wp;
