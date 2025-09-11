# PLJS Change Log

## 1.0.0

Released _June 25, 2025_.

- Support of PLV8 functionality
- Documentation created
- First initial release

### 1.0.1

Released _July 1, 2025_.

- Remove extraneous include from `types.c` that stopped PG14/15 compilation
- Add PG14/15 compilation to the CI

### 1.0.2

Released _August 20, 2025_.

- Fix function name collision with QuickJS
- Add missing `subtransaction` support
- Fix memory context switching before `CopyErrorData()`

### 1.0.3

Released _August 20, 2025_.

- PGXN release went out with an incorrect control file

### 1.0.4

Released _January 11, 2026_.

- Up memory default limit to 512MB
- Remove unnecessary include from modules.c
- Remove extra running of GC after each execution
- Better handling of SRF's, including freeing resources
- Increased test timeouts to accommodate slower CI machines
- Alter memory limit minimum to 64MB to facilitate testing
- Alter some tests for s390x testing
- Clean up parameter orders for function calls
- Fixed potential memory leak in `pljs_jsvalue_to_datums`

### 1.1.0
