#pragma once
#include <cstddef>   // size_t (clang/libc++ doesn't provide it transitively like libstdc++)
#include <cstdint>
#include <cmath>

// Alarm zone of a value against thresholds. Higher-is-worse limits use
// warnHi/critHi; two-sided signals (e.g. volts) also set warnLo/critLo.
// Leave any bound as NAN to disable it.
enum class Zone { Green, Amber, Red };

struct Thresholds {
  float warnHi = NAN;
  float critHi = NAN;
  float warnLo = NAN;
  float critLo = NAN;
};

Zone zoneFor(float value, const Thresholds& t);

// Low-alarm arming for RF_LOW_NEEDS_ENGINE stats (OIL P on the GM profile):
// armed after LOWARM_MS of sustained engine-running
// (RPM-based; see gauge_model.cpp). Ticked once per frame by applyReadings().
inline constexpr uint32_t LOWARM_MS       = 20000;
inline constexpr float    LOWARM_RPM      = 400.0f;
// RPM must be FRESH, not just held: at key-off the ECU stops answering and
// hold-last-good freezes RPM at its final value (698 rpm forever) while oil
// pressure gets its last fresh reads dropping to the zero floor — an armed-on-
// stale-RPM alarm fired in exactly that window on the 2026-07-17 log replay.
// 4s: RPM revisits every ~2-3s in the fast rotation, and the stamp goes stale
// within ~2s of key-off — disarm beats the 4s alarm holdoff with margin.
inline constexpr uint32_t LOWARM_RPM_FRESH_MS = 4000;
void lowArmTick(bool engineRunning, uint32_t now);
bool lowAlarmArmed();
void lowArmReset();   // tests

// One displayed signal with peak-hold.
struct Gauge {
  float value = 0.0f;
  float peak = 0.0f;
  bool valid = false;
  bool stale = false;   // valid but no fresh read within the tier window while linked
                        // (dead PID): render grey + exclude from alarm evaluation
  bool needsSetup = false;  // !valid because a SETTING is missing, not because the
                            // vehicle never answered: render "SET UP", not "--"
};

void gaugeUpdate(Gauge& g, float v);  // store value, mark valid, raise peak
void gaugeResetPeak(Gauge& g);        // peak <- current value (or 0 if invalid)
