#include "ble_rank.h"
#include <cctype>

// Case-insensitive substring test: true if `needle` occurs anywhere in `hay`.
static bool ciContains(const char* hay, const char* needle) {
  if (!hay || !needle || !*needle) return false;
  for (const char* p = hay; *p; ++p) {
    const char* a = p;
    const char* b = needle;
    while (*a && *b &&
           std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) { ++a; ++b; }
    if (!*b) return true;   // matched all of needle
  }
  return false;
}

bool bleNameLooksLikeObd(const char* name) {
  if (!name || !*name) return false;
  // Common tokens seen in BLE OBD adapter advertised names. Case-insensitive.
  static const char* const HINTS[] = {
    "obd", "vlink", "elm", "icar", "veepeak", "konnwei", "carista", "obdlink"
  };
  for (const char* h : HINTS)
    if (ciContains(name, h)) return true;
  return false;
}

// Sort key: OBD-named devices sort ahead of everything else, and within each
// group stronger RSSI sorts first. RSSI is ~[-100, 0] dBm; bias it positive and
// keep it well below the OBD flag's weight so the flag always dominates.
static long rankKey(const BleCand& c) {
  long obd = bleNameLooksLikeObd(c.name) ? 1000000L : 0L;
  long sig = (long)c.rssi + 200;   // -100..0 dBm -> 100..200, always positive
  return obd + sig;
}

void bleRankCandidates(const BleCand* cands, int n, int* order) {
  for (int i = 0; i < n; ++i) order[i] = i;
  // Insertion sort by descending rankKey. It is stable — the `<` (strict) test
  // means equal-key devices never shift past each other, so ties keep their
  // input order. n is small (scan count, capped by the caller at ~64).
  for (int i = 1; i < n; ++i) {
    int  cur = order[i];
    long ck  = rankKey(cands[cur]);
    int  j   = i - 1;
    while (j >= 0 && rankKey(cands[order[j]]) < ck) { order[j + 1] = order[j]; --j; }
    order[j + 1] = cur;
  }
}
