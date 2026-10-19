CREATE EXTENSION pljs;

-- pljs_version() and pljs_info()
SELECT length(pljs_version()) > 0 AS has_version;
SELECT pljs_info() IS NOT NULL AS has_info;

-- pljs.toString()
DO $$
  var s = pljs.toString();
  pljs.elog(NOTICE, 'pljs.toString() = ' + s);
$$ LANGUAGE pljs;
