#include <cstdio>
#include <cstring>
#include "obd_source.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }
static bool has(const char* hay, const char* needle){ return std::strstr(hay, needle) != nullptr; }

int main() {
  char buf[256];

  // Connecting, with a saved adapter and 12s elapsed on attempt 4.
  ConnStatus c; c.phase = ConnPhase::Connecting; c.attempts = 4;
  c.sinceMs = 1000; c.addr = "AA:BB:CC:DD:EE:FF";
  formatConnStatus(c, 13000, buf, sizeof buf);
  check(has(buf, "Connecting to OBD"), "title = connecting");
  check(has(buf, "AA:BB:CC:DD:EE:FF"), "shows adapter addr");
  check(has(buf, "Connecting"),        "shows phase");
  check(has(buf, "Attempt 4"),         "shows attempt count");
  check(has(buf, "12s"),               "elapsed (13000-1000)/1000 = 12s");
  check(has(buf, "settings"),          "shows settings hint");

  // Scanning + no saved adapter -> different title + fallback identity.
  ConnStatus s; s.phase = ConnPhase::Scanning; s.attempts = 1; s.sinceMs = 0; s.addr = "";
  formatConnStatus(s, 3000, buf, sizeof buf);
  check(has(buf, "Scanning"),          "scanning title");
  check(has(buf, "no saved adapter"),  "fallback identity");
  check(has(buf, "3s"),                "elapsed 3s");

  // Clock guard: now < sinceMs must not underflow to a huge number.
  ConnStatus g; g.phase = ConnPhase::Connecting; g.sinceMs = 5000;
  formatConnStatus(g, 1000, buf, sizeof buf);
  check(has(buf, "0s"), "now<since clamps elapsed to 0");

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
