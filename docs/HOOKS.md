# PLJS Hooks

PLJS can intercept PostgreSQL internal events by hooking into the engine's execution pipeline. You write a PLJS function, point a GUC at it, and your JavaScript runs each time that event fires.

## Configuration

Hooks are controlled through GUC settings, available via `SET` or in `postgresql.conf`. Hooks are disabled by default and require a superuser to enable.

| Setting                          | Description                                                      | Default |
| -------------------------------- | ---------------------------------------------------------------- | ------- |
| `pljs.hooks_enabled`             | Master switch for all hooks                                      | `false` |
| `pljs.hooks_max_depth`           | Maximum recursion depth per hook (prevents infinite loops)       | `5`     |
| `pljs.executor_start_hook`       | Function called at the start of query execution                  |         |
| `pljs.executor_run_hook`         | Function called when the executor runs to produce tuples         |         |
| `pljs.executor_end_hook`         | Function called at the end of query execution                    |         |
| `pljs.planner_hook`              | Function called during query planning                            |         |
| `pljs.create_upper_paths_hook`   | Function called when upper-level planner paths are created       |         |
| `pljs.set_rel_pathlist_hook`     | Function called after paths are generated for a base relation    |         |
| `pljs.set_join_pathlist_hook`    | Function called after join paths are generated                   |         |
| `pljs.join_search_hook`          | Function called during join enumeration                          |         |
| `pljs.get_relation_info_hook`    | Function called after relation information is loaded             |         |
| `pljs.needs_fmgr_hook`           | Function called to decide if `fmgr_hook` should fire             |         |
| `pljs.fmgr_hook`                 | Function called on function entry, exit, and abort               |         |
| `pljs.object_access_hook`        | Function called on object operations (CREATE, DROP, ALTER, etc.) |         |
| `pljs.object_access_hook_str`    | Same as above, but identifies objects by name instead of OID     |         |
| `pljs.emit_log_hook`             | Function called before a log message is emitted                  |         |

## Enabling Hooks

Hooks must be explicitly enabled before any hook function will fire:

```
SET pljs.hooks_enabled = true;
```

Only superusers can enable hooks or set hook function GUCs. Non-superusers will receive an error.

## Writing Hook Functions

A hook function is a regular PLJS function that accepts a single `jsonb` argument. The argument contains information about the event that triggered the hook. The function should return `void` (except for `needs_fmgr_hook`, which returns `boolean`).

```
CREATE FUNCTION my_hook(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'hook fired: ' + JSON.stringify(arguments[0]));
$$ LANGUAGE pljs;

SET pljs.executor_start_hook = 'my_hook';
```

You can also specify the function with its argument types:

```
SET pljs.executor_start_hook = 'my_hook(jsonb)';
```

To disable a hook, set the GUC to an empty string:

```
SET pljs.executor_start_hook = '';
```

## Executor Hooks

### `executor_start_hook`

Called at the start of query execution, before the executor initializes.

The argument object contains:

| Property     | Type   | Description                           |
| ------------ | ------ | ------------------------------------- |
| `operation`  | string | `SELECT`, `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `UTILITY`, or `NOTHING` |
| `sourceText` | string | Original SQL text                     |
| `eflags`     | object | Executor flags (see below)            |

The `eflags` object contains boolean properties: `explainOnly`, `rewind`, `backward`, `mark`, `skipTriggers`.

```
CREATE FUNCTION log_queries(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'executor_start: ' + arguments[0].operation);
$$ LANGUAGE pljs;

SET pljs.executor_start_hook = 'log_queries';
INSERT INTO my_table (val) VALUES ('test');
-- NOTICE:  executor_start: INSERT
```

### `executor_run_hook`

Called when the executor runs to produce tuples.

| Property      | Type    | Description                        |
| ------------- | ------- | ---------------------------------- |
| `operation`   | string  | Command type                       |
| `sourceText`  | string  | Original SQL text                  |
| `direction`   | string  | `forward`, `backward`, or `none`   |
| `count`       | number  | Max tuples to return (0 = all)     |
| `executeOnce` | boolean | Whether the plan executes only once |

### `executor_end_hook`

Called at the end of query execution.

| Property     | Type   | Description      |
| ------------ | ------ | ---------------- |
| `operation`  | string | Command type     |
| `sourceText` | string | Original SQL text |

## Planner Hooks

### `planner_hook`

Called during query planning.

| Property        | Type   | Description           |
| --------------- | ------ | --------------------- |
| `operation`     | string | Command type          |
| `queryString`   | string | Original SQL text     |
| `cursorOptions` | number | Cursor option flags   |

```
CREATE FUNCTION log_plans(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'planner: ' + arguments[0].operation);
$$ LANGUAGE pljs;

SET pljs.planner_hook = 'log_plans';
SELECT 1;
-- NOTICE:  planner: SELECT
```

### `create_upper_paths_hook`

Called when the planner creates upper-level (post-scan/join) paths.

| Property        | Type   | Description                                                                       |
| --------------- | ------ | --------------------------------------------------------------------------------- |
| `stage`         | string | `setop`, `partial_group_agg`, `group_agg`, `window`, `partial_distinct`, `distinct`, `ordered`, or `final` |
| `inputRelRows`  | number | Estimated input rows                                                              |
| `outputRelRows` | number | Estimated output rows                                                             |

### `set_rel_pathlist_hook`

Called after standard paths have been generated for a base relation.

| Property      | Type   | Description                              |
| ------------- | ------ | ---------------------------------------- |
| `relid`       | number | Relation ID in the planner               |
| `rows`        | number | Estimated row count                      |
| `rti`         | number | Range table index                        |
| `relationOid` | number | OID of the relation (for base relations) |

### `set_join_pathlist_hook`

Called after standard join paths are generated.

| Property       | Type   | Description                                       |
| -------------- | ------ | ------------------------------------------------- |
| `joinType`     | string | `inner`, `left`, `right`, `full`, `semi`, or `anti` |
| `joinRelRows`  | number | Estimated join result rows                        |
| `outerRelRows` | number | Estimated outer relation rows                     |
| `innerRelRows` | number | Estimated inner relation rows                     |

### `join_search_hook`

Called during join enumeration. This is an observational hook -- the standard join search always executes afterward.

| Property           | Type   | Description                    |
| ------------------ | ------ | ------------------------------ |
| `levelsNeeded`     | number | Number of relations to join    |
| `initialRelsCount` | number | Number of base relations       |

### `get_relation_info_hook`

Called after basic relation information has been loaded.

| Property      | Type    | Description                         |
| ------------- | ------- | ----------------------------------- |
| `relationOid` | number  | OID of the relation                 |
| `inhparent`   | boolean | Whether this is an inheritance parent |
| `rows`        | number  | Estimated row count                 |
| `pages`       | number  | Estimated page count                |

## Function Manager Hooks

### `needs_fmgr_hook`

Called to determine whether `fmgr_hook` processing should fire for a given function. Unlike other hooks, this takes an `int8` argument (the function OID) and returns a `boolean`.

```
CREATE FUNCTION my_needs_fmgr(int8) RETURNS boolean AS $$
  return true;
$$ LANGUAGE pljs;

SET pljs.needs_fmgr_hook = 'my_needs_fmgr';
```

### `fmgr_hook`

Called on function entry, exit, and abort for functions where `needs_fmgr_hook` returned `true`. Both hooks must be set together for `fmgr_hook` to fire.

| Property | Type   | Description                          |
| -------- | ------ | ------------------------------------ |
| `event`  | string | `start`, `end`, or `abort`           |
| `fnOid`  | number | OID of the function being called     |

```
CREATE FUNCTION my_needs_fmgr(int8) RETURNS boolean AS $$
  return true;
$$ LANGUAGE pljs;

CREATE FUNCTION my_fmgr(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'fmgr: ' + arguments[0].event);
$$ LANGUAGE pljs;

SET pljs.needs_fmgr_hook = 'my_needs_fmgr';
SET pljs.fmgr_hook = 'my_fmgr';
```

## Object Access Hooks

### `object_access_hook`

Called before or after SQL object operations such as CREATE, DROP, ALTER, TRUNCATE, and function execution.

| Property   | Type   | Description                                                                   |
| ---------- | ------ | ----------------------------------------------------------------------------- |
| `access`   | string | `post_create`, `drop`, `post_alter`, `namespace_search`, `function_execute`, or `truncate` |
| `classId`  | number | OID of the system catalog                                                     |
| `objectId` | number | OID of the specific object                                                    |
| `subId`    | number | Sub-object ID (e.g. column number), 0 if N/A                                 |

```
CREATE FUNCTION audit_objects(jsonb) RETURNS void AS $$
  pljs.elog(NOTICE, 'object_access: ' + arguments[0].access);
$$ LANGUAGE pljs;

SET pljs.object_access_hook = 'audit_objects';
CREATE TABLE test_tbl (id int);
-- NOTICE:  object_access: post_create
```

### `object_access_hook_str`

Same as `object_access_hook`, but identifies objects by name instead of OID. This is used for objects that may not yet have an OID at the time of the event.

| Property     | Type   | Description              |
| ------------ | ------ | ------------------------ |
| `access`     | string | Access type              |
| `classId`    | number | OID of the system catalog |
| `objectName` | string | Name of the object       |
| `subId`      | number | Sub-object ID            |

## Log Hook

### `emit_log_hook`

Called before a log message is emitted. This hook runs inside the error reporting system, so it cannot call `pljs.elog()` or `pljs.execute()` -- doing so would cause infinite recursion. Exceptions thrown from this hook are silently discarded.

| Property   | Type   | Description                                            |
| ---------- | ------ | ------------------------------------------------------ |
| `severity` | string | `DEBUG`, `LOG`, `INFO`, `NOTICE`, `WARNING`, `ERROR`, `FATAL`, or `PANIC` |
| `elevel`   | number | Numeric error level                                    |
| `message`  | string | Log message text                                       |
| `detail`   | string | Detail text (if any)                                   |
| `hint`     | string | Hint text (if any)                                     |
| `sqlstate` | number | SQL error code (if any)                                |
| `schema`   | string | Schema name (if any)                                   |
| `table`    | string | Table name (if any)                                    |

```
CREATE FUNCTION observe_logs(jsonb) RETURNS void AS $$
  // Silently process -- cannot elog from here.
$$ LANGUAGE pljs;

SET pljs.emit_log_hook = 'observe_logs';
```

## Recursion Protection

Hooks that call `pljs.execute()` will trigger other hooks, which can lead to recursion. PLJS tracks recursion depth per hook type and stops calling the hook when the depth exceeds `pljs.hooks_max_depth` (default `5`). When the limit is hit, a `WARNING` is emitted (except for `emit_log_hook`, which silently stops).

You can adjust the limit:

```
SET pljs.hooks_max_depth = 2;
```

You can also guard against recursion in your hook function by checking the source text:

```
CREATE FUNCTION my_planner_hook(jsonb) RETURNS void AS $$
  var q = arguments[0].queryString || '';
  if (q.indexOf('my_inner_query') < 0) {
    pljs.execute("SELECT 1 as my_inner_query");
  }
$$ LANGUAGE pljs;
```

## Error Handling

If a hook function throws an exception or encounters an error, PLJS catches it and emits a `WARNING`. The original query still executes normally -- hooks are observational and do not block execution. If the hook function name does not resolve to a valid PLJS function, a `WARNING` is emitted and the query proceeds.

## Security

Only superusers can:

- Set `pljs.hooks_enabled` to `true`
- Set any hook function GUC

Non-superuser attempts to modify these settings will result in an error.
