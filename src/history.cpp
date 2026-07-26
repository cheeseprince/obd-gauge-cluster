#include "history.h"

void HistoryRing::push(float v) {
  samples[head] = v;
  head = (head + 1) % HISTORY_LEN;
  if (count < HISTORY_LEN) count++;
}

float HistoryRing::get(int i) const {
  // Oldest retained sample sits `count` slots behind head.
  int start = (head - count + HISTORY_LEN) % HISTORY_LEN;
  return samples[(start + i) % HISTORY_LEN];
}

bool HistoryRing::minmax(float* lo, float* hi) const {
  if (count == 0) return false;
  float mn = get(0), mx = get(0);
  for (int i = 1; i < count; i++) {
    float v = get(i);
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  *lo = mn;
  *hi = mx;
  return true;
}

// Sample only gauges that have a real reading. A never-read gauge holds the
// 0.0 boot default with valid=false — pushing that would seed the Focus trend
// graph with fake zeros and wreck its auto-fit until they age out of the ring.
static void pushValid(HistorySet& h, const GaugeSet& g) {
  for (int s = 0; s < STAT_COUNT; s++)
    if (g.g[s].valid) h.ring[s].push(g.g[s].value);
}

void historyTick(HistorySet& h, const GaugeSet& g, uint32_t nowMs) {
  // First call: seed one sample immediately and start the clock.
  if (!h.started) {
    h.started = true;
    h.lastSampleMs = nowMs;
    pushValid(h, g);
    return;
  }
  // Rate-limit to one sample per stat per HISTORY_INTERVAL_MS.
  if (nowMs - h.lastSampleMs < HISTORY_INTERVAL_MS) return;
  h.lastSampleMs = nowMs;
  pushValid(h, g);
}
