#include <cstdio>
#include <initializer_list>
#include "obd_schedule.h"
#include "readouts.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  // Regression (whole-branch review CRITICAL): an ObdSchedule is a member of the
  // file-scope OBD source and is constructed during static init, when
  // g_activeProfile is still null. The ctor must NOT read the profile —
  // constructing with a null profile must not crash. Tier-building is deferred
  // to build(), called from the source's begin() after profile selection.
  { g_activeProfile = nullptr; ObdSchedule guard; (void)guard; }   // must not dereference null

  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  ObdSchedule s;
  s.build();
  // Tally next() over a long run — indices 0..READOUT_COUNT-1.
  int count[STAT_COUNT] = {0};   // STAT_COUNT is constexpr; buckets built from the active profile
  for (int i = 0; i < 1000; i++) count[s.next()]++;

  // Every ACTIVE queried row must appear at least once. Deactivated stats
  // (cmd set but dropped from PAGES/HELPERS, e.g. Pedal/Ambient) must NOT be
  // polled — that is the whole point of deactivation.
  for (int i = 0; i < READOUT_COUNT; i++) {
    if (READOUTS[i].cmd && isActive(i)) check(count[i] > 0, "every active queried row scheduled");
    if (!isActive(i))                   check(count[i] == 0, "deactivated row never scheduled");
  }

  // Fast-tier rows (Boost=2, Rpm=6, Speed=7) each polled clearly more often
  // than the slow-tier temps (Trans=0, Oil=1, Coolant=3, Volts=4, Intake=5).
  int fastMin = count[2];  // Boost
  for (int f : {6, 7})     // Rpm, Speed
    if (count[f] < fastMin) fastMin = count[f];
  int slowMax = 0;
  for (int sl : {0, 1, 3, 4, 5})  // Trans, Oil, Coolant, Volts, Intake
    if (count[sl] > slowMax) slowMax = count[sl];
  check(fastMin > slowMax, "fast > slow");

  // Baro (IDX_BARO == 16) is the rarest query.
  check(count[IDX_BARO] <= slowMax, "baro rarest");

  // Header affinity: each tier bucket is sorted by READOUTS[].header, so the
  // rotation visits all same-header PIDs consecutively — each header change in
  // the query stream costs an extra AT SH round trip. One full fast rotation
  // must therefore contain at most (distinct headers - 1) upward transitions;
  // sorted order means header is simply non-decreasing across the rotation.
  {
    ObdSchedule s2;
    s2.build();
    // slots 0-3 of every 5 are fast picks; collect one full fast rotation.
    int fastPicks[16]; int nf = 0;
    for (int t = 0; nf < 16 && t < 100; t++) {
      int idx = s2.next();
      if ((t % 5) >= 4) continue;                  // slow/rare slot — not a fast pick
      if (nf > 0 && idx == fastPicks[0]) break;    // rotation wrapped
      fastPicks[nf++] = idx;
    }
    bool sorted = true;
    for (int i = 1; i < nf; i++)
      if (READOUTS[fastPicks[i]].header < READOUTS[fastPicks[i-1]].header) sorted = false;
    check(nf > 0 && sorted, "fast rotation grouped by header (minimal AT SH swaps)");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
