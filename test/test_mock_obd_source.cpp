#include <cstdio>
#include <cmath>
#include "mock_obd_source.h"
#include "readouts.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  MockObdSource src;
  src.begin();

  // At t=0 link is up and values sit in a plausible resting band.
  src.poll(0);
  const ObdReadings& r0 = src.latest();
  check(r0.linkUp, "link up");
  check(r0.v[(int)StatId::Trans] > 80 && r0.v[(int)StatId::Trans] < 300, "trans in range");
  check(r0.v[(int)StatId::Volts] > 11 && r0.v[(int)StatId::Volts] < 16, "volts in range");

  // Deterministic: same time -> same value.
  src.poll(5000); float a = src.latest().v[(int)StatId::Trans];
  src.poll(5000); float b = src.latest().v[(int)StatId::Trans];
  check(a == b, "deterministic for equal time");

  // --- Safe mode (default): thresholded stats stay GREEN over a full cycle ---
  {
    bool allGreen = true;
    for (uint32_t t = 0; t < 120000; t += 1000) {
      src.poll(t);
      const ObdReadings& r = src.latest();
      const StatId thr[] = {StatId::Trans, StatId::Oil, StatId::Coolant, StatId::Volts};
      for (StatId id : thr)
        if (zoneFor(r.v[(int)id], READOUTS[(int)id].thr) != Zone::Green) allGreen = false;
    }
    check(src.safeMode(), "default is safe mode");
    check(allGreen, "safe mode: all thresholded stats stay green over a cycle");
  }

  // --- Safe mode: EVERY stat (all STAT_COUNT rows, thresholded or not) stays
  // Green across a full sweep period — bands come from the profile thresholds,
  // and the sweep table must cover the whole StatId range (the old 31-row
  // hand tables read OOB for OilP/Gear). Raw zoneFor: no engine-gate involved.
  {
    bool allGreen = true;
    for (uint32_t t = 0; t <= 130000; t += 500) {
      src.poll(t);
      const ObdReadings& r = src.latest();
      for (int i = 0; i < READOUT_COUNT; i++)
        if (zoneFor(r.v[i], READOUTS[i].thr) != Zone::Green) allGreen = false;
    }
    check(allGreen, "safe mode: ALL stats green from profile thresholds");
  }

  // --- Sweep mode: trans and volts cross their alarm zones over a cycle ---
  src.setSafeMode(false);
  float lo = 1e9f, hi = -1e9f;
  float vlo = 1e9f, vhi = -1e9f;
  for (uint32_t t = 0; t < 120000; t += 1000) {
    src.poll(t);
    float x = src.latest().v[(int)StatId::Trans];
    lo = std::fmin(lo, x); hi = std::fmax(hi, x);
    float v = src.latest().v[(int)StatId::Volts];
    vlo = std::fmin(vlo, v); vhi = std::fmax(vhi, v);
  }
  check(hi >= 255, "trans reaches red zone (>=255) within a cycle");
  check(lo <= 200, "trans returns to safe band (<=200) within a cycle");
  // Volts sweep must cross the battery thresholds: critLo=10.2, warnLo=11.0
  check(vhi >= 15.0f, "volts reaches over-volt zone (>=15.0) within a cycle");
  check(vlo <= 10.2f, "volts reaches under-volt critical zone (<=10.2) within a cycle");

  // --- Staleness: with the link DOWN, a stat with no fresh read within its tier window
  //     falls back to "--". (ObdReadings defaults linkUp=false, so these exercise that path.) ---
  {
    GaugeSet gg; ObdReadings r;
    // Trans = slow tier (30min = 1,800,000ms window); fresh read stamped at t=1000.
    r.valid[(int)StatId::Trans] = true; r.v[(int)StatId::Trans] = 200.0f; r.ms[(int)StatId::Trans] = 1000;
    applyReadings(gg, r, 5000);       // 4s old < 30min -> fresh
    check(gg.g[(int)StatId::Trans].valid && gg.g[(int)StatId::Trans].value == 200.0f, "fresh slow stat applied");
    applyReadings(gg, r, 1802000);   // 1801s old > 1800s -> stale
    check(!gg.g[(int)StatId::Trans].valid, "stale slow stat -> invalid (shows --)");

    GaugeSet gf; ObdReadings rf;
    // Speed = fast tier (120s window).
    rf.valid[(int)StatId::Speed] = true; rf.v[(int)StatId::Speed] = 55.0f; rf.ms[(int)StatId::Speed] = 1000;
    applyReadings(gf, rf, 6000);     // 5s < 120s -> fresh
    check(gf.g[(int)StatId::Speed].valid, "fresh fast stat applied");
    applyReadings(gf, rf, 122000);   // 121s > 120s -> stale
    check(!gf.g[(int)StatId::Speed].valid, "stale fast stat -> invalid");

    check(!gf.g[(int)StatId::Oil].valid, "never-read stat stays invalid");

    // HOLD-WHILE-LINKED (the flicker fix): with the link UP, even a very old reading is
    // held — the pane never blanks. Past the tier window the gauge is MARKED STALE
    // instead (rendered grey, excluded from alarms) so a dead PID can't masquerade
    // as a live reading (dead-PID guard, restored after 16417f9 removed blanking).
    GaugeSet gh; ObdReadings rh; rh.linkUp = true;
    rh.valid[(int)StatId::Trans] = true; rh.v[(int)StatId::Trans] = 210.0f; rh.ms[(int)StatId::Trans] = 1000;
    applyReadings(gh, rh, 5000);      // 4s old, linked -> fresh, not stale
    check(gh.g[(int)StatId::Trans].valid && !gh.g[(int)StatId::Trans].stale,
          "linked + fresh: valid, not stale");
    applyReadings(gh, rh, 5000000);   // ~83 min old, but linkUp -> held, not blanked
    check(gh.g[(int)StatId::Trans].valid && gh.g[(int)StatId::Trans].value == 210.0f,
          "linked: stale stat held (no flicker)");
    check(gh.g[(int)StatId::Trans].stale, "linked + past window: marked stale (dead PID)");
    rh.ms[(int)StatId::Trans] = 4999000;   // fresh read arrives -> stale clears
    applyReadings(gh, rh, 5000000);
    check(gh.g[(int)StatId::Trans].valid && !gh.g[(int)StatId::Trans].stale,
          "linked: fresh read clears stale");
    rh.ms[(int)StatId::Trans] = 1000;
    rh.linkUp = false;                // link drops -> the window applies again
    applyReadings(gh, rh, 5000000);
    check(!gh.g[(int)StatId::Trans].valid, "link down + stale -> invalid");

    // A never-read stat stays invalid even while linked.
    GaugeSet gn; ObdReadings rn; rn.linkUp = true;
    applyReadings(gn, rn, 5000);
    check(!gn.g[(int)StatId::Oil].valid, "linked: never-read stat stays invalid");

    // The mock source stamps ms[] each poll, so a long bench run never goes
    // stale-grey (and sweep mode keeps exercising alarms).
    GaugeSet gm; MockObdSource ms; ms.begin();
    ms.poll(10000000);                       // ~2.8h uptime, way past every window
    applyReadings(gm, ms.latest(), 10000000);
    check(gm.g[(int)StatId::Trans].valid && !gm.g[(int)StatId::Trans].stale,
          "mock: fresh every poll, never stale");

    // CROSS-CORE SKEW (the grey-flicker bug): core-1 captures `now` at the top of
    // loop(), then core-0's poll stamps cur_.ms[i] with a LATER millis() before
    // core-1 reads the snapshot — so r.ms[i] can be a few ms AHEAD of `now`. The
    // staleness check must treat that as fresh. An unsigned (now - ms) underflows
    // to ~4.29e9 there and falsely marks the PID stale, flickering it grey every
    // other frame (worst on MPG + fast-tier stats, which are stamped every poll).
    GaugeSet gsk; ObdReadings rsk; rsk.linkUp = true;
    rsk.valid[(int)StatId::Speed] = true; rsk.v[(int)StatId::Speed] = 60.0f;
    rsk.ms[(int)StatId::Speed] = 100005;          // stamped 5 ms in the "future"
    applyReadings(gsk, rsk, 100000);              // now is 5 ms behind the stamp
    check(gsk.g[(int)StatId::Speed].valid && !gsk.g[(int)StatId::Speed].stale,
          "cross-core skew: ms just ahead of now is fresh, not stale (no grey flicker)");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
