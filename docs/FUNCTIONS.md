# PLJS Built-in Functions

PLJS includes a number of built-in functions bound to the `pljs` object for you to use.

## Utility Functions

### `pljs.elog`

`pljs.elog` emits a message to the client or the PostgreSQL log file. The
emit level is one of:

- `DEBUG5`
- `DEBUG4`
- `DEBUG3`
- `DEBUG2`
- `DEBUG1`
- `LOG`
- `INFO`
- `NOTICE`
- `WARNING`
- `ERROR`

```js
var msg = 'world';

pljs.elog(DEBUG1, 'Hello', `${msg}!`);
```

See the [Postgres manual](https://www.postgresql.org/docs/current/static/runtime-config-logging.html#RUNTIME-CONFIG-SEVERITY-LEVELS) for information on each error level.

### `pljs.find_function`

PLJS provides a function to access other functions defined as `pljs` functions that have been created as part of the extension.

```
CREATE FUNCTION callee(a int) RETURNS int AS $$ return a * a $$ LANGUAGE pljs;
CREATE FUNCTION caller(a int, t int) RETURNS int AS $$
  var func = pljs.find_function("callee");
  return func(a);
$$ LANGUAGE pljs;
```

With `pljs.find_function()`, you can look up other PLJS functions. If a function is found and it is not a PLJS function, an error is thrown. The function signature parameter to `pljs.find_function()` is either of `regproc` (function name only) or `regprocedure` (function name with argument types). You can make use of the internal type for arguments and void type for return type for the pure JavaScript function to make sure any invocation from SQL statements should not occur.

### `pljs.version`

The `pljs` object provides a version string as `pljs.version`. This string corresponds to the `pljs` module version.

## Database Access via SPI

PLJS provides functions for database access, including prepared statements, and cursors.

### `pljs.execute`

`pljs.execute(sql [, args])`

Executes SQL statements and retrieves the results. The `sql` argument is required, and the `args` argument is an optional `array` containing any arguments passed in the SQL query. For `SELECT` queries, the returned value is an `array` of `objects`. Each `object` represents one row, with the `object` properties mapped as column names. For non-`SELECT` queries, the return result is the number of rows affected.

```
var json_result = pljs.execute('SELECT * FROM tbl');
var num_affected = pljs.execute('DELETE FROM tbl WHERE price > $1', [ 1000 ]);
```

### `pljs.return_next`

`pljs.return_next(arg)`

Returns a value in the context of a [Set Returning Function](../INTEGRATION.md). Called anywhere else — a `DO` block, a trigger, or a function that does not return a set — it throws.

A row is an object naming each column, as `{a: 1, b: 'x'}`, or for a set of one column its bare value. A column is read from the row by its name as reading the property reads it: its own property, or one it inherits — a getter of its class, say — but not one every object inherits, and a `Proxy`'s through its `get` trap. A function as a row's column is an error, since no column takes one, except a `json` or `jsonb` column, where it is SQL NULL, as it is anywhere in a document. For a set of one column the name is the one the function gives it — `a` for `RETURNS TABLE (a int)`, or an `OUT` parameter's — and a `json` or `jsonb` column, or a domain over one, takes the whole object as its value instead. An object without the column that has a `toString()`, `valueOf()` or `[Symbol.toPrimitive]()` other than every object's — its own, or its class's — is a value that converts itself. Otherwise an object with exactly one property gives that property's value. A row that gives no value for its column, or is not an object at all, raises `datatype_mismatch`.

A value that has to satisfy a domain with a `CHECK` constraint — a set of such a domain, or of a composite type with a column of one — is checked in a subtransaction, as `pljs.execute()` runs its query, so a constraint that fails leaves nothing behind when the exception is caught. So is the rest of converting the row: an error converting it, one JavaScript raised as much as PostgreSQL's, rolls back what the row's getters did as well. A set with no such domain has no subtransaction, and keeps what they did.

Before PostgreSQL 17, no subtransaction can be begun in parallel mode, where a `PARALLEL SAFE` function can run. There such a value is checked without one, and any error converting it ends the call rather than being thrown to JavaScript, since nothing can roll back what a constraint left — a row that does not name its column, or a getter that throws, as much as a constraint that fails: a `try`/`catch` around `pljs.return_next()` does not catch it, as a PL/pgSQL block cannot catch an error there either. So does an error while working out whether a set needs a subtransaction — reading the catalogs, or loading a domain's constraints — and one in a row that ran a `CHECK` constraint without one all the same, because a getter gave its domain one as the row was converted, on any version.

Changing the types a set's rows are converted to from a getter while a row is converted — a domain's constraints, a composite type's columns, dropping a domain — is not supported. pljs keeps the session sound when it happens, but an error in that row can end the call rather than be thrown to JavaScript.

### `pljs.gc`

` pljs.gc()`

If at compile time, garbage collection exposure is enabled, then this function is exposed to allow a request to the Javascript engine to run its garbage collection.

### `pljs.prepare`

`pljs.prepare(sql [, typenames])`

Opens or creates a prepared statement. The `typenames` parameter is an `array` with the type of each `bind` parameter, and values passed to the plan are converted to those types. Without `typenames`, the parameters' types are inferred from the query, as they are for `pljs.execute()`. Returned value is an object of the `PreparedPlan` type. This object must be freed by `plan.free()` before leaving the function.

```
var plan = pljs.prepare('SELECT * FROM tbl WHERE col = $1', [ 'int' ]);
var rows = plan.execute([ 1 ]);
var sum = 0;
for (var i = 0; i < rows.length; i++) {
  sum += rows[i].num;
}
plan.free();

return sum;
```

### `PreparedPlan.execute`

`PreparedPlan.execute([ args ])`

Executes the prepared statement. The `args` parameter is the same as what would be required for `pljs.execute()`, and can be omitted if the statement does not have any parameters. The result of this method is also the same as `pljs.execute()`.

### `PreparedPlan.cursor`

`PreparedPlan.cursor([ args ])`

Opens a cursor form the prepared statement. The `args` parameter is the same as what would be required for `pljs.execute()` and `PreparedPlan.execute()`. The returned object is of type `Cursor`. This must be closed by `Cursor.close()` before leaving the function.

```
var plan = pljs.prepare('SELECT * FROM tbl WHERE col = $1', [ 'int' ]);
var cursor = plan.cursor([ 1 ]);
var sum = 0, row;
while (row = cursor.fetch()) {
    sum += row.num;
}
cursor.close();
plan.free();

return sum;
```

### `PreparedPlan.free`

Frees the prepared statement.

### `Cursor.fetch`

`Cursor.fetch([ nrows ])`

When the `nrows` parameter is omitted, fetches a row from the cursor and returns it as an `object` (note: not as an `array`). If specified, fetches as many rows as the `nrows` parameter, up to the number of rows available, and returns an `array` of `objects`. A negative value will fetch backward.

### `Cursor.move`

`Cursor.move(nrows)`

Moves the cursor `nrows`. A negative value will move backward.

### `Cursor.close`

Closes the `Cursor`.

### `pljs.subtransaction`

`pljs.subtransaction(func)`

`pljs.execute()` creates a subtransaction each time it executes. If you need an atomic operation, you will need to call `pljs.subtransaction()` to create a subtransaction block.

```
try{
  pljs.subtransaction(function(){
    pljs.execute("INSERT INTO tbl VALUES(1)"); // should be rolled back!
    pljs.execute("INSERT INTO tbl VALUES(1/0)"); // occurs an exception
  });
} catch(e) {
  ... execute fall back plan ...
}
```

If one of the SQL execution in the subtransaction block fails, all of operations within the block are rolled back. If the process in the block throws a JavaScript exception, it is carried forward. So use a `try ... catch` block to capture it and do alternative operations if it occurs.

## Window Function API

You can define user-defined window functions with PLJS. It wraps the C-level window function API to support full functionality. To create one, first obtain a window object by calling `pljs.get_window_object()`, which provides the following interfaces. They act on the window function call that is running, and throw outside of one.

### `WindowObject.get_current_position`

Returns the current position in the partition, starting from `0`.

### `WindowObject.get_partition_row_count`

Returns the number of rows in the partition.

### `WindowObject.set_mark_position`

`WindowObject.set_mark_position(pos)`

Sets the mark at the specified row. Rows above this position will be gone and no longer accessible later.

### `WindowObject.rows_are_peers`

`WindowObject.rows_are_peers(pos1, pos1)`

Returns `true` if the rows at `pos1` and `pos2` are peers.

### `WindowObject.get_func_arg_in_partition`

`WindowObject.get_func_arg_in_partition(argno, relpos, seektype, mark_pos)`

### `WindowObject.get_func_arg_in_frame`

`WindowObject.get_func_arg_in_frame(argno, relpos, seektype, mark_pos)`

Returns the value of the argument in `argno` (starting from 0) to this function at the `relpos` row from `seektype` in the current partition or frame. `seektype` can be either of `WindowObject.SEEK_HEAD`, `WindowObject.SEEK_CURRENT`, or `WindowObject.SEEK_TAIL`. If `mark_pos` is `true`, the row the argument is fetched from is marked. If the specified row is out of the partition/frame, the returned value will be undefined.

### `WindowObject.get_func_arg_in_current`

`WindowObject.get_func_arg_in_current(argno)`

Returns the value of the argument in `argno` (starting from 0) to this function at the current row. Note that the returned value will be the same as the argument variable of the function.

### `WindowObject.get_partition_local`

`WindowObject.get_partition_local([ size ])`

Returns partition-local value, which is released at the end of the current partition. If nothing is stored, undefined is returned. size argument (default 1000) is the byte size of the allocated memory in the first call. Once the memory is allocated, the size will not change.

### `WindowObject.set_partition_local`

`WindowObject.set_partition_local(obj)`

Stores the partition-local value, which you can retrieve later with `get_partition_local()`. This function internally uses `JSON.stringify()` to serialize the object, so if you pass a value that is not able to be serialized it may end up being an unexpected value. If the size of a serialized value is more than the allocated memory, it will throw an exception.

You can also learn more on how to use these API in the `sql/window.sql` regression test, which implements most of the native window functions. For general information on the user-defined window function, see the [CREATE FUNCTION page of the PostgreSQL manual](https://www.postgresql.org/docs/current/static/sql-createfunction.html).

## Stored Procedures

PLJS provides a small number of stored procedures.

### `pljs_info`

`SELECT pljs_info();`

Provides a view of memory usage and stack usage inside the JavaScript runtime. This is returned as a `JSON` object:

```json
{
  "malloc_count": 228,
  "malloc_size": 14192,
  "malloc_limit": 268435456,
  "stack_size": 1048576,
  "stack_limit": 6120837488
}
```

### `pljs_version`

`SELECT pljs_version();`

Returns the version number of the currently compiled PLJS extension.
