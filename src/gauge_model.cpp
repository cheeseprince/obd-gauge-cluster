#include "gauge_model.h"

Zone zoneFor(float value, const Thresholds& t) {
  if (!std::isnan(t.critHi) && value >= t.critHi) return Zone::Red;
  if (!std::isnan(t.critLo) && value <= t.critLo) return Zone::Red;
  if (!std::isnan(t.warnHi) && value >= t.warnHi) return Zone::Amber;
  if (!std::isnan(t.warnLo) && value <= t.warnLo) return Zone::Amber;
  return Zone::Green;
}

void gaugeUpdate(Gauge& g, float v) {
  g.value = v;
  if (!g.valid || v > g.peak) g.peak = v;
  g.valid = true;
}

void gaugeResetPeak(Gauge& g) {
  g.peak = g.valid ? g.value : 0.0f;
}

// --- OIL P alarm arming -----------------------------------------------------
// Low oil pressure only means something once the engine has been RUNNING long
// enough for pressure to establish. Gating on fuel flow failed on the truck:
// FUEL is a slow-tier PID (~35s poll cadence at 33 stats), so its last-good
// value is stale through exactly the transitions that matter (2026-07-17 log
// replay: 11 false fires at auto-stop shutdowns and key-on windows). RPM is
// fast-tier (fresh ~2s): arm the alarm only after RPM >= LOWARM_RPM sustained
// LOWARM_MS; disarm the instant RPM drops/goes invalid/link drops. Auto-stop
// spin-down's fake 500-700 rpm readings can't survive the sustain window, and
// restarts never nuisance (pressure builds in ~3s, arming waits 20s). A real
// pressure loss with the engine still turning fires normally.
static bool     s_lowArmed  = false;
static bool     s_running   = false;   // currently in a confirmed-running streak
static uint32_t s_runSince  = 0;       // when the streak started

void lowArmTick(bool engineRunning, uint32_t now) {
  if (!engineRunning) { s_lowArmed = false; s_running = false; return; }
  if (!s_running) { s_running = true; s_runSince = now; }
  if (!s_lowArmed && (int32_t)(now - s_runSince) >= (int32_t)LOWARM_MS)
    s_lowArmed = true;
}
bool lowAlarmArmed() { return s_lowArmed; }
void lowArmReset()   { s_lowArmed = false; s_running = false; }
