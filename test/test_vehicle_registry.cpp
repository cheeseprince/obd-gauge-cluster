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

    // NO STAT MAY APPEAR ON TWO PAGES. nav_model's readoutPageOf() returns the
    // FIRST page holding a stat, and cursorStep() locates its FIRST slot in the
    // reading order -- so a duplicate silently teleports the knob backwards
    // instead of advancing. Shipped once (Ford RANGE re-listed FuelLevel and
    // Def, and page 8 jumped to page 4 on the bench); caught here from now on.
    // StatId::COUNT is the deliberate empty-cell placeholder and is skipped.
    const LayoutDef& L = p->defaultLayout;
    bool seen[(int)StatId::COUNT] = {false};
    for (int pg = 0; pg < L.pageCount; pg++) {
      for (int c = 0; c < 4; c++) {
        StatId id = L.pages[pg][c];
        if (id == StatId::COUNT) continue;            // intentional blank cell
        check(!seen[(int)id], "no stat appears on two pages of one profile");
        seen[(int)id] = true;
      }
    }
    // Helpers are the stats polled/logged WITHOUT a tile, so they must be
    // disjoint from the pages -- a stat in both is a contradiction about
    // whether it is displayed.
    for (int h = 0; h < L.helperCount; h++) {
      StatId id = L.helpers[h];
      if (id == StatId::COUNT) continue;
      check(!seen[(int)id], "a helper stat must not also occupy a page cell");
    }
  }
  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("test_vehicle_registry: ALL PASS\n"); return 0;
}
