#pragma once
#include <cstdint>

// Parse a "vX.Y.Z" release string into one monotonically-comparable value.
//
// THIS FUNCTION DECIDES WHETHER AN UPDATE INSTALLS. The dash compares the
// manifest's version against its own with it, so a bug here either refuses a
// real release or accepts an older one. It was `static` inside ota_update.cpp
// and therefore untestable; it is here so the host suite can pin the edge
// cases, all of which are load-bearing:
//
//   * A NON-RELEASE STRING PARSES TO 0. "local" and "dev-<hash>" are what a
//     USB or bench build stamps, and 0 means "older than every release", so
//     such a build always updates rather than stranding itself. Relied on
//     directly on 2026-08-15: a build stamped "v0.4.4-heapfix" was expected to
//     read as 0.4.4 and report "Up to date", and it did.
//   * PARSING STOPS AT THE FIRST NON-VERSION CHARACTER, so "v0.4.4-heapfix"
//     and "v1.2.3-rc1" compare as their release part rather than as 0. A
//     pre-release therefore does NOT out-rank the release it precedes.
//   * EACH FIELD IS CLAMPED TO 10 BITS (1023). Three fields are packed into one
//     uint32 at 20/10/0, so an unclamped field would carry into the one above
//     it and make a large patch number outrank a minor bump.
//
// Comparison is by the packed value only — never by string order, where
// "v0.10.0" sorts below "v0.9.0".
inline uint32_t parseSemver(const char* v) {
  if (!v) return 0;
  if (*v == 'v' || *v == 'V') v++;
  unsigned f[3] = {0, 0, 0};
  int idx = 0;
  bool sawDigit = false;
  for (; *v && idx < 3; v++) {
    if (*v >= '0' && *v <= '9') {
      f[idx] = f[idx] * 10 + (unsigned)(*v - '0');
      if (f[idx] > 1023) f[idx] = 1023;
      sawDigit = true;
    } else if (*v == '.') {
      idx++;
    } else {
      break;   // stop at any non-version char ("-rc1", etc.)
    }
  }
  return sawDigit ? ((f[0] << 20) | (f[1] << 10) | f[2]) : 0;
}
