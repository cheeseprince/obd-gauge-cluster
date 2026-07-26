// test_profile_generic: GENERIC_PROFILE invariants — full StatId table, a
// non-empty default layout, and every laid-out stat backed by a standard
// Mode-01 PID (spec 2026-07-24-vehicle-profile-framework, Task 3).
#include <cstdio>
#include <cstddef>
#include "../src/vehicle_active.h"
#include "../src/readouts.h"

extern const VehicleProfile GENERIC_PROFILE;
static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  g_activeProfile = &GENERIC_PROFILE;
  // Table sized to the union.
  check(READOUT_COUNT == (int)StatId::COUNT, "generic readout count == STAT_COUNT");
  // Layout is non-empty and references only stats that carry a standard cmd.
  const auto& L = GENERIC_PROFILE.defaultLayout;
  check(L.pageCount >= 1, "generic has >= 1 page");
  for (int p = 0; p < L.pageCount; p++)
    for (int c = 0; c < 4; c++) {
      StatId s = L.pages[p][c];
      if (s == StatId::COUNT) continue;
      check(READOUTS[(int)s].cmd != nullptr, "every laid-out generic stat has a standard PID");
      // Standard Mode-01 PIDs only: cmd starts with "01".
      check(READOUTS[(int)s].cmd[0]=='0' && READOUTS[(int)s].cmd[1]=='1', "generic uses only Mode-01 PIDs");
    }
  // MAF is g/s, NOT the gph->L/h Flow conversion -> Quantity::None (so Metric
  // mode doesn't multiply it by 3.785).
  check(READOUTS[(int)StatId::Maf].quantity == Quantity::None, "generic MAF is Quantity::None");
  // FUEL% (012F) carries a cmd, so it must be scheduled or it never gets polled.
  // Generic schedules it as a helper (the two pages are full).
  bool fuelScheduled = false;
  for (int h = 0; h < L.helperCount; h++)
    if (L.helpers[h] == StatId::FuelLevel) fuelScheduled = true;
  check(fuelScheduled, "generic FUEL% is scheduled (helper)");
  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("test_profile_generic: ALL PASS\n"); return 0;
}
