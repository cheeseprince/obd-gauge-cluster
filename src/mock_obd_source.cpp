#include <cmath>
#include <cstdint>
#include "mock_obd_source.h"
#include "readouts.h"   // READOUTS, READOUT_COUNT
#include "vehicle_active.h"

// A triangle/sine sweep helper: returns a value oscillating in [lo,hi] with the
// given period (ms), phase-shifted so the stats don't all move in lockstep.
static float sweep(uint32_t t, float lo, float hi, uint32_t periodMs, float phase) {
  float u = std::sin(2.0f * 3.14159265f * ((float)t / periodMs + phase));  // -1..1
  return lo + (hi - lo) * (u * 0.5f + 0.5f);
}

// Sweep specs keyed by readout index (matches READOUTS[] order: Trans, Oil,
// Boost, Coolant, Volts, Intake, Rpm, Speed, Egt, DpfDp, FuelRate, Load, MpgInst, MpgAvg, Gal100mi, L100km, Rail, Hp, Def, FuelLevel, DslFill, DefFill, ActTq, RefTq, Baro,
// Maf, Ambient, Egr, Pedal, Cac, Nox).
struct SweepSpec { float lo, hi; uint32_t period; float phase; };

// Full-sweep: thresholded rows cross warn/crit zones — exercises alarm overlay.
static const SweepSpec SWEEP[] = {
  /*Trans  */ {160, 270, 60000, 0.00f},
  /*Oil    */ {180, 285, 75000, 0.20f},
  /*Boost  */ {  0,  22, 12000, 0.10f},
  /*Coolant*/ {170, 240, 90000, 0.40f},
  /*Volts  */ { 9.8f,15.2f, 40000, 0.60f},   // must dip under critLo 10.2 to exercise the alarm
  /*Intake */ { 70, 140, 50000, 0.80f},
  /*Rpm    */ {700,3200,  8000, 0.30f},
  /*Speed  */ {  0,  75, 30000, 0.50f},
  /*Egt    */ {300,1350, 70000, 0.15f},
  /*DpfDp  */ {  0,  16, 55000, 0.35f},
  /*FuelRt */ {0.4f, 12, 20000, 0.45f},
  /*Load   */ { 10,  95, 18000, 0.85f},
  /*MpgInst*/ {  5,  40, 25000, 0.65f},
  /*MpgAvg */ { 18,  28,100000, 0.05f},
  /*Gal100 */ {  2,  10, 45000, 0.70f},
  /*L100km */ {  6,  14, 48000, 0.25f},
  /*Rail   */ {4000,28000, 16000, 0.90f},
  /*Hp     */ {  5, 280, 22000, 0.12f},
  /*Def    */ { 40,  90, 130000, 0.33f},
  /*FuelLvl*/ { 40,  90, 110000, 0.40f},
  /*DslFill*/ {  2,  20,  60000, 0.18f},
  /*DefFill*/ {0.3f, 4,  90000, 0.62f},
  /*ActTq  */ {  0, 100, 14000, 0.55f},
  /*RefTq  */ {610, 610, 120000, 0.00f},
  /*Baro   */ {100, 101,120000, 0.00f},
  /*Maf    */ { 10, 200, 14000, 0.20f},
  /*Ambient*/ { 30, 110,120000, 0.40f},
  /*Egr    */ {  0,  60, 18000, 0.50f},
  /*Pedal  */ {  0, 100, 10000, 0.70f},
  /*Cac    */ { 80, 200, 60000, 0.30f},
  /*Nox    */ {  0, 400, 22000, 0.60f},
  /*OilP   */ { 18,  55, 26000, 0.44f},
  /*Gear   */ {  1,  10, 40000, 0.20f},
};
// The old hand-maintained tables silently fell behind the StatId enum (31 rows
// vs 33 stats after OilP/Gear landed) -> OOB reads on every bench poll.
static_assert(sizeof(SWEEP)/sizeof(SWEEP[0]) == (size_t)STAT_COUNT,
              "SWEEP must cover every StatId");

// Safe mode: clamp the sweep into the profile's green band (works for ANY
// vehicle profile — no hand-maintained parallel table, no OOB when StatId
// grows). Margins keep clear of the warn bounds so holdoff can't be nicked.
static float clampSafe(int i, float v) {
  const Thresholds& t = READOUTS[i].thr;
  if (!std::isnan(t.warnHi)) v = std::fmin(v, t.warnHi * 0.92f);
  if (!std::isnan(t.warnLo)) v = std::fmax(v, t.warnLo * 1.15f);
  return v;
}

void MockObdSource::begin() { cur_.linkUp = true; }

void MockObdSource::poll(uint32_t t) {
  // Build the tick into a local, then publish under the lock — keeps the
  // critical section to just the struct copy (poll runs on core 0; latest()
  // copies from core 1).
  ObdReadings next;
  next.linkUp = true;
  // Safe mode keeps thresholded rows in the green band (clamped from the
  // profile's thresholds); sweep mode crosses warn/crit zones to exercise
  // the alarm overlay.
  for (int i = 0; i < READOUT_COUNT; i++) {
    float v = sweep(t, SWEEP[i].lo, SWEEP[i].hi, SWEEP[i].period, SWEEP[i].phase);
    next.v[i] = safe_ ? clampSafe(i, v) : v;
    next.valid[i] = true;
    next.ms[i] = t;   // every value regenerates each poll -> genuinely fresh
                      // (without this the dead-PID stale marker greys the whole
                      // bench build once t exceeds the fast-tier window)
  }
#if defined(ARDUINO)
  portENTER_CRITICAL(&mux_);
  cur_ = next;
  portEXIT_CRITICAL(&mux_);
#else
  cur_ = next;        // host tests: single-threaded, no lock needed
#endif
}

ObdReadings MockObdSource::latest() const {
#if defined(ARDUINO)
  ObdReadings snap;
  portENTER_CRITICAL(&mux_);
  snap = cur_;
  portEXIT_CRITICAL(&mux_);
  return snap;
#else
  return cur_;        // host tests: single-threaded, no lock needed
#endif
}
