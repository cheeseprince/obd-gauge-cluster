#include <cstdio>
#include <cmath>
#include "history.h"

static int failures = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL: %s\n", m); failures++; } }

int main() {
  // --- Ring: order is oldest -> newest ---
  {
    HistoryRing r;
    r.push(1); r.push(2); r.push(3);
    check(r.count == 3,  "ring: count after 3 pushes");
    check(r.get(0) == 1, "ring: get(0) oldest");
    check(r.get(2) == 3, "ring: get(2) newest");
  }

  // --- Ring: wraparound keeps the newest HISTORY_LEN samples ---
  {
    HistoryRing r;
    for (int i = 0; i < HISTORY_LEN + 50; i++) r.push((float)i);
    check(r.count == HISTORY_LEN, "ring: count saturates at HISTORY_LEN");
    check(r.get(0) == (float)50, "ring: oldest is sample 50 after overflow");
    check(r.get(HISTORY_LEN - 1) == (float)(HISTORY_LEN + 49), "ring: newest is last pushed");
  }

  // --- Ring: minmax ---
  {
    HistoryRing r;
    float lo, hi;
    check(!r.minmax(&lo, &hi), "minmax: false when empty");
    r.push(5); r.push(-3); r.push(11); r.push(7);
    check(r.minmax(&lo, &hi), "minmax: true when populated");
    check(lo == -3, "minmax: lo");
    check(hi == 11, "minmax: hi");
  }

  // --- historyTick: seeds immediately, then rate-limits to 1 Hz ---
  {
    HistorySet h;
    GaugeSet g;
    g.g[(int)StatId::Trans].value = 100;
    g.g[(int)StatId::Trans].valid = true;
    historyTick(h, g, 0);                 // first call seeds
    check(h.ring[(int)StatId::Trans].count == 1, "tick: seeds one sample at t=0");

    g.g[(int)StatId::Trans].value = 101;
    historyTick(h, g, 500);              // <1s later: no new sample
    check(h.ring[(int)StatId::Trans].count == 1, "tick: no sample before interval");

    historyTick(h, g, 1000);            // 1s: new sample
    check(h.ring[(int)StatId::Trans].count == 2, "tick: sample after interval");
    check(h.ring[(int)StatId::Trans].get(1) == 101, "tick: newest value recorded");

    historyTick(h, g, 1999);           // <1s after last: no sample
    check(h.ring[(int)StatId::Trans].count == 2, "tick: no sample mid-interval");
    historyTick(h, g, 2000);          // next second: sample
    check(h.ring[(int)StatId::Trans].count == 3, "tick: sample on next interval");
  }

  // --- historyTick samples all VALID stats each interval; never-read gauges
  //     (valid=false, value=0.0 boot default) must NOT seed the trend graph
  //     with fake 0.0 samples that wreck the first 5-min auto-fit ---
  {
    HistorySet h;
    GaugeSet g;
    for (int s = 0; s < STAT_COUNT; s++) { g.g[s].valid = true; g.g[s].value = 50; }
    g.g[(int)StatId::Oil].valid = false;               // never read
    historyTick(h, g, 0);
    historyTick(h, g, 1000);
    for (int s = 0; s < STAT_COUNT; s++) {
      if (s == (int)StatId::Oil) continue;
      check(h.ring[s].count == 2, "tick: every valid stat sampled");
    }
    check(h.ring[(int)StatId::Oil].count == 0, "tick: invalid stat not sampled (no 0.0 seed)");

    // Once it turns valid, its ring starts from the first real value.
    g.g[(int)StatId::Oil].valid = true; g.g[(int)StatId::Oil].value = 210;
    historyTick(h, g, 2000);
    check(h.ring[(int)StatId::Oil].count == 1 && h.ring[(int)StatId::Oil].get(0) == 210,
          "tick: ring starts at first real value");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
