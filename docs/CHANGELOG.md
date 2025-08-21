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
