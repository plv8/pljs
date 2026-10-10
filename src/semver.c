#include "semver.h"

#include <ctype.h>
#include <stdlib.h>

bool semver_parse(const char *str, semver *out) {
  if (!str || !out) {
    return false;
  }

  char *end;
  long major = strtol(str, &end, 10);
  if (end == str || *end != '.') {
    return false;
  }

  str = end + 1;
  long minor = strtol(str, &end, 10);
  if (end == str || *end != '.') {
    return false;
  }

  str = end + 1;
  long patch = strtol(str, &end, 10);
  if (end == str) {
    return false;
  }

  // Anything after the patch ("1.2.3-beta", "1.2.3.4") is not a plain
  // major.minor.patch version; refuse it rather than silently truncating.
  while (isspace((unsigned char)*end)) {
    end++;
  }
  if (*end != '\0') {
    return false;
  }

  out->major = (int)major;
  out->minor = (int)minor;
  out->patch = (int)patch;
  return true;
}

bool semver_range_parse(const char *str, semver_range *out) {
  if (!str || !out) {
    return false;
  }

  // Skip leading whitespace.
  while (isspace((unsigned char)*str)) {
    str++;
  }

  if (*str == '^') {
    out->type = SEMVER_CARET;
    str++;
  } else if (*str == '~') {
    out->type = SEMVER_TILDE;
    str++;
  } else if (str[0] == '>' && str[1] == '=') {
    out->type = SEMVER_GTE;
    str += 2;
  } else if (str[0] == '<' && str[1] == '=') {
    out->type = SEMVER_LTE;
    str += 2;
  } else if (*str == '>') {
    out->type = SEMVER_GT;
    str++;
  } else if (*str == '<') {
    out->type = SEMVER_LT;
    str++;
  } else {
    out->type = SEMVER_EXACT;
  }

  // Skip whitespace between operator and version.
  while (isspace((unsigned char)*str)) {
    str++;
  }

  return semver_parse(str, &out->version);
}

int semver_compare(const semver *a, const semver *b) {
  if (a->major != b->major) {
    return a->major - b->major;
  }
  if (a->minor != b->minor) {
    return a->minor - b->minor;
  }
  return a->patch - b->patch;
}

bool semver_satisfies(const semver *version, const semver_range *range) {
  int cmp = semver_compare(version, &range->version);

  switch (range->type) {
  case SEMVER_EXACT:
    return cmp == 0;

  case SEMVER_CARET:
    // >=range.version, and nothing that changes the left-most non-zero part:
    // ^1.2.3 is <2.0.0, ^0.2.3 is <0.3.0, ^0.0.3 is exactly 0.0.3.
    if (cmp < 0) {
      return false;
    }
    if (range->version.major != 0) {
      return version->major == range->version.major;
    }
    if (range->version.minor != 0) {
      return version->major == 0 && version->minor == range->version.minor;
    }
    return version->major == 0 && version->minor == 0 &&
           version->patch == range->version.patch;

  case SEMVER_TILDE:
    // >=range.version AND <next minor
    if (cmp < 0) {
      return false;
    }
    return version->major == range->version.major &&
           version->minor == range->version.minor;

  case SEMVER_GTE:
    return cmp >= 0;

  case SEMVER_LTE:
    return cmp <= 0;

  case SEMVER_GT:
    return cmp > 0;

  case SEMVER_LT:
    return cmp < 0;
  }

  return false;
}
