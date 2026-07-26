#pragma once
#include <cstdint>
#include "gauge_model.h"   // Zone
#include "app_types.h"     // STAT_COUNT

// Gates alarms by persistence: a readout is "confirmed" alarming only after it
// has been continuously non-Green for >= HOLD_MS. Resets on Green. Per-index.
class AlarmHoldoff {
 public:
  static const uint32_t HOLD_MS = 4000;
  bool confirmed(int idx, Zone z, uint32_t nowMs);
 private:
  uint32_t since_[STAT_COUNT] = {0};   // 0 = currently green; else first non-green time
  bool     armed_[STAT_COUNT] = {false};
};
