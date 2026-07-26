// test_profile: profile-table invariants (spec 2026-07-20-vehicle-profile-design).
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/readouts.h"
#include "../src/app_types.h"
#include "../src/vehicle_profile.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  // 1. RF_COMPUTED implies cmd==nullptr (computed rows carry no PID). The
  //    converse does NOT hold: an UNSUPPORTED stat is also cmd==nullptr but
  //    unflagged (never scheduled). GM has no unsupported rows; BMW does.
  for (int i = 0; i < READOUT_COUNT; i++)
    if (READOUTS[i].flags & RF_COMPUTED)
      assert(READOUTS[i].cmd == nullptr);
  printf("  ok   computed-implies-cmdless\n");

  // 2. Every row has a decoder (computed rows keep their decNone stub).
  for (int i = 0; i < READOUT_COUNT; i++) assert(READOUTS[i].decode != nullptr);
  printf("  ok   decoder-present\n");

  // 3. fullScale >= critHi wherever critHi is set (bar can't peg while green).
  for (int i = 0; i < READOUT_COUNT; i++)
    if (!std::isnan(READOUTS[i].thr.critHi))
      assert(READOUTS[i].fullScale >= READOUTS[i].thr.critHi);
  printf("  ok   fullscale-covers-crit\n");

  // 4. RF_LOW_NEEDS_ENGINE only on rows that actually have a low bound,
  //    and (today) exactly on OIL P.
  for (int i = 0; i < READOUT_COUNT; i++)
    if (READOUTS[i].flags & RF_LOW_NEEDS_ENGINE) {
      assert(!std::isnan(READOUTS[i].thr.warnLo));
      assert((StatId)i == StatId::OilP);
    }
  assert(READOUTS[(int)StatId::OilP].flags & RF_LOW_NEEDS_ENGINE);
  printf("  ok   low-needs-engine-flag\n");

  // 5. The engine gate follows the FLAG, not a hardcoded StatId.
  {
    GaugeSet gs{};
    gs.g[(int)StatId::OilP].value = 2.0f;    // below critLo 8
    lowArmReset();                            // not armed -> gated to Green
    assert(zoneForStat(gs, (int)StatId::OilP) == Zone::Green);
    lowArmTick(true, 0); lowArmTick(true, LOWARM_MS + 1);   // arm
    assert(zoneForStat(gs, (int)StatId::OilP) == Zone::Red);
    lowArmReset();
  }
  printf("  ok   flag-driven-engine-gate\n");

  // 6. Active layout only references SUPPORTED stats; every page has a name.
  for (int p = 0; p < readoutPageCount(); p++) {
    assert(pageName(p)[0] != '\0');
    for (int c = 0; c < 4; c++) {
      int idx = readoutAt(p, c);
      if (idx < 0) continue;
      bool supported = READOUTS[idx].cmd != nullptr || (READOUTS[idx].flags & RF_COMPUTED);
      assert(supported);
    }
  }
  // Every ACTIVE stat (displayed or helper) must be supported — a helper the
  // vehicle can't read would poll a null cmd forever.
  for (int i = 0; i < READOUT_COUNT; i++)
    if (isActive(i))
      assert(READOUTS[i].cmd != nullptr || (READOUTS[i].flags & RF_COMPUTED));
  printf("  ok   layout-references-supported-stats\n");

  // 7. Every readout's header indexes a real AddressingDef entry.
  for (int i = 0; i < READOUT_COUNT; i++)
    assert(READOUTS[i].header < VEHICLE.addressingCount);
  printf("  ok   header-index-in-range\n");

  printf("test_profile: ALL PASS\n");
  return 0;
}
