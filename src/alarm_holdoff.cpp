#include "alarm_holdoff.h"

bool AlarmHoldoff::confirmed(int idx, Zone z, uint32_t nowMs) {
  if (z == Zone::Green) { armed_[idx] = false; return false; }
  if (!armed_[idx]) { armed_[idx] = true; since_[idx] = nowMs; }
  return (nowMs - since_[idx]) >= HOLD_MS;
}
