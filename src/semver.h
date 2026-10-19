#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct semver {
  int major;
  int minor;
  int patch;
} semver;

typedef enum semver_range_type {
  SEMVER_EXACT,  // 1.2.3
  SEMVER_CARET,  // ^1.2.3 (>=1.2.3 <2.0.0), ^0.2.3 (<0.3.0), ^0.0.3 (=0.0.3)
  SEMVER_TILDE,  // ~1.2.3 (>=1.2.3 <1.3.0)
  SEMVER_GTE,    // >=1.2.3
  SEMVER_LTE,    // <=1.2.3
  SEMVER_GT,     // >1.2.3
  SEMVER_LT,     // <1.2.3
} semver_range_type;

typedef struct semver_range {
  semver_range_type type;
  semver version;
} semver_range;

// Parse a version string "1.2.3" into a semver struct.
// Returns true on success, false on parse error.
bool semver_parse(const char *str, semver *out);

// Parse a range string like "^1.2.3", "~1.0.0", ">=2.0.0", "1.2.3".
// Returns true on success, false on parse error.
bool semver_range_parse(const char *str, semver_range *out);

// Compare two semver values. Returns <0, 0, or >0.
int semver_compare(const semver *a, const semver *b);

// Check if a version satisfies a range constraint.
bool semver_satisfies(const semver *version, const semver_range *range);
