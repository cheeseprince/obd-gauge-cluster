#pragma once
#include <cstdint>
#include "app_types.h"

// Rolling per-stat sample history for the Focus-view trend graph.
// Pure logic (no Arduino/LVGL) so it is host-unit-tested.

// Overridable via -D HISTORY_LEN=N. No environment currently sets it — the one
// board has PSRAM and takes the default — but the seam is kept because the ring is
// 33 stats x N x 4B of contiguous memory, which is the first thing to shrink if a
// future board is tighter on RAM.
#ifndef HISTORY_LEN
#define HISTORY_LEN 300   // default: 5 min @ 1 Hz
#endif
static constexpr uint32_t HISTORY_INTERVAL_MS = 1000;  // one sample per second

// Ring buffer of the most recent HISTORY_LEN samples for one stat.
struct HistoryRing {
  float samples[HISTORY_LEN] = {0};
  int   count = 0;   // valid samples so far (saturates at HISTORY_LEN)
  int   head  = 0;   // index the next sample will be written to

  void push(float v);
  // Oldest-to-newest access: i in [0, count); i==0 is the oldest retained sample.
  float get(int i) const;
  // Min/max across the retained samples. Returns false (and leaves *lo/*hi) if empty.
  bool minmax(float* lo, float* hi) const;
};

// One ring per stat, plus the 1 Hz sampling clock.
struct HistorySet {
  HistoryRing ring[STAT_COUNT];
  uint32_t    lastSampleMs = 0;
  bool        started = false;
};

// Call every loop with the current gauge values. Pushes one sample per stat
// each time HISTORY_INTERVAL_MS has elapsed (and once immediately on first call).
void historyTick(HistorySet& h, const GaugeSet& g, uint32_t nowMs);
