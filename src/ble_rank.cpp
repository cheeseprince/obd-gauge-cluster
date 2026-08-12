#include "ble_rank.h"
#include <cctype>
#include <cstring>
#include <cstdio>
#include <strings.h>   // strcasecmp (POSIX; NOT in <cstring>)

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

// 8 slots: a scan tops out around 26 devices and only non-OBD ones land here.
// Overflow drops the oldest, so that device is probed once more -- degradation,
// not failure.
static const int  REJ_CAP = 8;
static const int  REJ_LEN = 18;            // "aa:bb:cc:dd:ee:ff" + NUL
static char       s_rej[REJ_CAP][REJ_LEN];
static int        s_rejCount = 0;          // entries in use, <= REJ_CAP
static int        s_rejNext  = 0;          // ring write cursor

static bool rejValid(const char* a) { return a && a[0]; }

void bleRejectClear() { s_rejCount = 0; s_rejNext = 0; }

bool bleRejectContains(const char* addr) {
  if (!rejValid(addr)) return false;       // no address is not a match
  for (int i = 0; i < s_rejCount; i++)
    if (strcasecmp(s_rej[i], addr) == 0) return true;   // BlueZ and NimBLE differ on hex case
  return false;
}

void bleRejectRecord(const char* addr) {
  if (!rejValid(addr)) return;
  // Refuse rather than truncate. A truncated address breaks its own lookup AND can
  // prefix-collide with a DIFFERENT device, which would skip a peer that was never
  // rejected -- potentially the user's real adapter. Not storing it merely means that
  // device is probed again, which is the safe direction to fail.
  if (strlen(addr) >= REJ_LEN) return;
  if (bleRejectContains(addr)) return;     // duplicates must not consume slots
  snprintf(s_rej[s_rejNext], REJ_LEN, "%s", addr);
  s_rejNext = (s_rejNext + 1) % REJ_CAP;
  if (s_rejCount < REJ_CAP) s_rejCount++;
}

// Sort key: OBD-named devices sort ahead of everything else, and within each
// group stronger RSSI sorts first. RSSI is ~[-100, 0] dBm; bias it positive and
// keep it well below the OBD flag's weight so the flag always dominates.
bool bleShouldSkip(const BleCand& c) {
  return c.svc == SvcHint::Other || bleRejectContains(c.addr);
}

static long rankKey(const BleCand& c) {
  // Weights are decades apart so each tier strictly dominates the one below:
  // a service match beats any name, and any name beats any signal. Advertising
  // our service UUID is stronger evidence than a name -- names are chosen by
  // whoever made the clone, service UUIDs are what the firmware must actually
  // find after connecting.
  long svc = (c.svc == SvcHint::Obd) ? 100000000L : 0L;
  long obd = bleNameLooksLikeObd(c.name) ? 1000000L : 0L;
  long sig = (long)c.rssi + 200;   // -100..0 dBm -> 100..200, always positive
  return svc + obd + sig;
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
