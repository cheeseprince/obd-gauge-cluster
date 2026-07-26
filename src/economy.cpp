#include "economy.h"

void Economy::reset() {
  miles_ = gallons_ = 0.0; lastMs_ = 0; have_ = false;
  lastFuel_ = lastSpeed_ = 0.0f;
}

void Economy::update(float fuelGph, float speedMph, uint32_t nowMs) {
  lastFuel_ = fuelGph; lastSpeed_ = speedMph;
  if (!have_) { have_ = true; lastMs_ = nowMs; return; }   // first sample seeds the clock
  uint32_t dt = nowMs - lastMs_;
  lastMs_ = nowMs;
  if (dt == 0 || dt > 10000) return;     // skip zero + abnormal gaps (e.g. reconnect)
  double hrs = dt / 3600000.0;
  miles_   += (double)speedMph * hrs;
  gallons_ += (double)fuelGph  * hrs;
}

// Display cap: economy numbers above 99 aren't useful (coast/idle spikes).
static float cap99(float v) { return v > 99.0f ? 99.0f : v; }

float Economy::instantMpg() const {
  if (lastFuel_ > 0.02f && lastSpeed_ > 1.0f) return cap99(lastSpeed_ / lastFuel_);
  return 0.0f;
}

float Economy::avgMpg() const {
  return gallons_ > 0.0001 ? cap99((float)(miles_ / gallons_)) : 0.0f;
}

float Economy::avgGalPer100mi() const {
  return miles_ > 0.0001 ? cap99((float)(gallons_ / miles_ * 100.0)) : 0.0f;
}

float Economy::avgLPer100km() const {
  return miles_ > 0.0001
           ? cap99((float)(gallons_ * 3.785411784 / (miles_ * 1.609344) * 100.0))
           : 0.0f;
}

bool Economy::valid() const { return gallons_ > 0.0001; }
