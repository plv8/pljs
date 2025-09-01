# Roadmap

The intention of this document is to provide a past, current, and future roadmap to the development of PLJS. The current version is tracked, along with the current status of implementation.

Current Release Version: `1.0.0`

# 0.8

`0.8` was an alpha release of what attempted to be mostly feature compatible with PLV8.

## Major Features

- [x] functions
- [x] inline functions
- [x] triggers
- [x] type conversion and coercion
- [x] SQL executions
- [x] cursors
- [x] memory management
- [x] executions timeouts

# 1.0

`1.0` is the first major release for PLJS. While not completely up to feature parity with PLV8, `1.0` is geared at providing enough basic functionality to install side-by-side with PLV8, and look at migration options.

## Major Features

- [x] caching of contexts and functions
- [x] set returning functions
- [x] windows
- [x] startup functions
- [x] procedures/transactions
- [x] find function
- [x] `BigInt`
- [x] documentation

# 1.1

`1.1` will expand on what exists in PLV8, and attempt to create a new Javascript language plugin universe.

## Major Features

- [ ] initial hook management
- [ ] module imports via `import`
- [ ] resetting of contexts (`pljs_reset()`)

# 1.2

`1.2` will expand on `1.1` by creating `PLJSu`, the untrusted version of PLJS.

- [ ] local access
- [ ] network access
