#include <cstdio>
#include <cstring>
#include <cstddef>
#include "../src/vehicle_active.h"
#include "../src/vehicle_registry.h"
#include "../src/readouts.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  check(profileCount() >= 1, "registry has at least one entry");
  check(strcmp(profileKeyAt(0), "generic") == 0, "generic is index 0 (default)");
  // Known keys round-trip; unknown -> nullptr / -1.
  check(profileForKey("generic") != nullptr, "generic resolves");
  check(profileForKey("gm_sierra_lz0") != nullptr, "gm resolves");
  check(profileForKey("bmw_f10_535i") != nullptr, "bmw resolves");
  check(profileForKey("nope") == nullptr, "unknown key -> nullptr");
  check(profileForKey(nullptr) == nullptr, "null key -> nullptr");
  check(profileIndexForKey("gm_sierra_lz0") >= 0, "gm has an index");
  check(profileIndexForKey("nope") == -1, "unknown -> -1");
  // Every registered profile is well-formed.
  for (int i = 0; i < profileCount(); i++) {
    const VehicleProfile* p = profileForKey(profileKeyAt(i));
    check(p != nullptr, "keyAt round-trips through profileForKey");
    g_activeProfile = p;
    check(READOUT_COUNT == (int)StatId::COUNT, "profile readout count == STAT_COUNT");
    check(p->defaultLayout.pageCount >= 1, "profile has >= 1 page");
  }
  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("test_vehicle_registry: ALL PASS\n"); return 0;
}
