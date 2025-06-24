# PLJS

PLJS is a Javascript Language Extension for _"modern"_ PostgreSQL.

It is compact, lightweight, and decently fast.

## Technology

Javascript: [QuickJS](https://bellard.org/quickjs/quickjs.html)

PostgreSQL: 14+

### Current Status

Currently 1.0.0 alpha release.

Missing:

- Windows

Also, WASM will likely never be added to this extension.

## Building

Building is meant to be easy, but not all platforms have been worked out as far as the build instructions. Please use this as an example of how to build in the meantime.

## MacOS

### Requirements

- XCode
- git

### Building

```
$ make install
```
