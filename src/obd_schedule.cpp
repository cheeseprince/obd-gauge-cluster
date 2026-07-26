#include "obd_schedule.h"
#include "readouts.h"
#include "vehicle_active.h"

ObdSchedule::ObdSchedule() {
  // Deliberately empty. This object is a member of the file-scope OBD source and
  // is constructed during static initialization, BEFORE setup() selects a profile
  // — so g_activeProfile (which READOUTS dereferences) is still null here. The
  // tier lists are built later by build(), called from the source's begin().
}

void ObdSchedule::build() {
  // Build tier lists from the readout table. Called from the OBD source's begin(),
  // after the active profile is selected. Idempotent — reset all state first.
  nFast_ = nSlow_ = nRare_ = 0;
  tick_ = fi_ = si_ = ri_ = 0;
  // Rows with cmd==nullptr are computed-only (MPG/HP/fill tiles) — never queried.
  for (int i = 0; i < READOUT_COUNT; i++) {
    if (READOUTS[i].cmd == nullptr) continue;
    if (!isActive(i)) continue;   // deactivated stats (not in layout) are never polled
    int t = READOUTS[i].tier;
    // NOTE: tier buckets are capped at 16. A full fast/slow bucket overflows into
    // rare_ (polled rarely, not never); only a full rare_ (>=16, i.e. ~48 queried
    // stats total) drops a stat entirely. Slow is the closest at 15/16 today — if
    // you add many slow PIDs and one stops updating, raise these caps.
    if      (t == 0 && nFast_ < 16) fast_[nFast_++] = i;
    else if (t == 1 && nSlow_ < 16) slow_[nSlow_++] = i;
    else if (nRare_ < 16)           rare_[nRare_++] = i;
  }

  // Header affinity: stable-sort each bucket by ELM header so the rotation
  // visits all same-header PIDs consecutively. Every header change in the
  // query stream costs an extra AT SH round trip (pollQuery re-issues the
  // header whenever it differs from the previous query's) — grouped order
  // cuts that to one swap per header block per rotation.
  sortByHeader(fast_, nFast_);
  sortByHeader(slow_, nSlow_);
  sortByHeader(rare_, nRare_);
}

void ObdSchedule::sortByHeader(int* bucket, int n) {
  for (int i = 1; i < n; i++) {          // insertion sort: tiny n, stable
    int v = bucket[i], j = i - 1;
    while (j >= 0 && READOUTS[bucket[j]].header > READOUTS[v].header) {
      bucket[j + 1] = bucket[j]; j--;
    }
    bucket[j + 1] = v;
  }
}

int ObdSchedule::next() {
  // 5-slot pattern: 4 fast + 1 slow; every 60th tick the slow slot yields a rare
  // (baro) instead. Mirrors the previous cadence: fast ~4x slow, baro ~1/60.
  int slot = tick_ % 5;
  int idx;
  if (slot < 4 && nFast_) { idx = fast_[fi_ % nFast_]; fi_++; }
  else if (tick_ % 60 == 4 && nRare_) { idx = rare_[ri_ % nRare_]; ri_++; }
  else if (nSlow_) { idx = slow_[si_ % nSlow_]; si_++; }
  else if (nFast_) { idx = fast_[fi_ % nFast_]; fi_++; }
  else idx = 0;
  tick_++;
  return idx;
}
