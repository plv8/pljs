# PLJS Configuration Options

PLJS provides configuration options that are available either with the `SET` command in Postgres, or by adding them to the `postgresql.conf` file.

| Setting                  | Description                                                                   | Default                       |
| ------------------------ | ----------------------------------------------------------------------------- | ----------------------------- |
| `pljs.execution_timeout` | Execution timeout in seconds before the JavaScript runtime is interrupted     | `300` (Range of `1`-`65536`)  |
| `pljs.memory_limit`      | Memory limit of the JavaScript engone in `MB`                                 | `256` (Range of `256`-`3096`) |
| `pljs.start_proc`        | A Function Name to be called when the JavaScript engine is first initialized` |                               |
