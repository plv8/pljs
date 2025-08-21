# PLJS Change Log

## 1.0.0

Released June 25, 2025\_.

- Support of PLV8 functionality
- Documentation created
- First initial release

### 1.0.1

Release _July 1, 2025_.

- Remove extraneous include from `types.c` that stopped PG14/15 compilation
- Add PG14/15 compilation to the CI

### 1.0.2

- Fix function name collision with quickjs
- Add missing `subtransaction` support
- Fix memory context switching before `CopyErrorData()`
