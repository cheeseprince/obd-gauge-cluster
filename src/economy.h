#pragma once
#include <cstdint>

// Trip fuel-economy integrator. instant = mph/gph; average = integrated
// miles / gallons since reset(). Units match the stored readouts (mph, gph).
class Economy {
 public:
  void reset();
  void update(float fuelGph, float speedMph, uint32_t nowMs);
  float instantMpg() const;
  float avgMpg() const;
  bool  valid() const;        // true once any fuel has been integrated
  float avgGalPer100mi() const;   // average gallons per 100 miles (capped at 99)
  float avgLPer100km() const;     // average liters per 100 km (capped at 99)
 private:
  double   miles_ = 0.0, gallons_ = 0.0;
  uint32_t lastMs_ = 0;
  bool     have_ = false;
  float    lastFuel_ = 0.0f, lastSpeed_ = 0.0f;
};
