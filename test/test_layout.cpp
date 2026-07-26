#include <cstdio>
#include "readouts.h"
#include "app_types.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  // Layout arrays live in the vehicle profile now — validate through the facade.
  int seen[STAT_COUNT] = {0};
  for (int p = 0; p < readoutPageCount(); p++)
    for (int c = 0; c < 4; c++) {
      int idx = readoutAt(p, c);
      check(idx >= -1 && idx < STAT_COUNT, "cell valid StatId or empty");
      if (idx >= 0) seen[idx]++;
    }
  for (int i = 0; i < STAT_COUNT; i++) check(seen[i] <= 1, "no stat in two cells");
  // Helpers = active but not displayed; they must not also occupy a cell.
  int helpers = 0;
  for (int i = 0; i < STAT_COUNT; i++)
    if (isActive(i) && !isDisplayed(i)) { helpers++; check(seen[i] == 0, "HELPERS not in PAGES"); }

  check(readoutPageCount() == 7, "7 pages");
  check(readoutAt(0,0) == (int)StatId::Trans, "p0c0 TRANS (TOW)");
  check(readoutAt(0,3) == (int)StatId::Egt,   "p0c3 EGT (TOW)");
  check(readoutAt(1,0) == (int)StatId::Boost, "p1c0 BOOST (POWER)");
  check(readoutAt(2,0) == (int)StatId::DpfDp, "p2c0 DPF dP (REGEN)");
  check(readoutAt(3,2) == (int)StatId::Def,   "p3c2 DEF (RANGE)");
  check(readoutAt(4,3) == (int)StatId::L100km,"p4c3 L/100km (TRIP)");
  check(readoutAt(5,1) == (int)StatId::Egr,   "p5c1 EGR (DIAG)");
  check(readoutAt(6,1) == (int)StatId::Volts, "p6c1 VOLTS (MISC)");
  check(readoutAt(0,2) == (int)StatId::OilP,  "p0c2 OIL P (TOW)");
  check(readoutAt(6,2) == (int)StatId::Oil,   "p6c2 OIL temp (MISC)");
  check(readoutAt(6,3) == -1,                 "p6c3 empty");

  int disp = 0; for (int i = 0; i < STAT_COUNT; i++) if (isDisplayed(i)) disp++;
  check(disp == 27, "27 displayed");
  // Baro + ActTq dropped from display but STILL POLLED (boost needs baro, HP
  // needs torque); Pedal + Ambient fully deactivated (not displayed, not polled).
  check(!isDisplayed((int)StatId::RefTq) && isActive((int)StatId::RefTq), "RefTq hidden helper");
  check(!isDisplayed((int)StatId::Baro)  && isActive((int)StatId::Baro),  "Baro hidden helper");
  check(!isDisplayed((int)StatId::ActTq) && isActive((int)StatId::ActTq), "ActTq hidden helper");
  check(!isDisplayed((int)StatId::Pedal)   && !isActive((int)StatId::Pedal),   "Pedal deactivated");
  check(!isDisplayed((int)StatId::Ambient) && !isActive((int)StatId::Ambient), "Ambient deactivated");
  check(helpers == 4, "four helpers (RefTq/Baro/ActTq/Gear)");
  check(!isDisplayed((int)StatId::Gear) && isActive((int)StatId::Gear), "Gear hidden helper (logged only)");
  check(isDisplayed((int)StatId::OilP), "OIL P displayed (TOW p0c2)");

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
