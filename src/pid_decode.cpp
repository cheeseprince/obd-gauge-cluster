#include "pid_decode.h"

float cToF(float c) { return c * 9.0f / 5.0f + 32.0f; }
float kphToMph(float kph) { return kph * 0.621371f; }
float kpaToPsi(float kpa) { return kpa * 0.1450377f; }

float decodeTempC(uint8_t a) { return static_cast<float>(a) - 40.0f; }

float decodeRpm(uint8_t a, uint8_t b) {
  return ((static_cast<int>(a) * 256) + b) / 4.0f;
}

float decodeSpeedKph(uint8_t a) { return static_cast<float>(a); }
float decodeMapKpa(uint8_t a) { return static_cast<float>(a); }

float decodeModuleVolts(uint8_t a, uint8_t b) {
  return ((static_cast<int>(a) * 256) + b) / 1000.0f;
}

float boostPsi(float mapKpa, float baroKpa) {
  float diff = mapKpa - baroKpa;
  if (diff < 0.0f) diff = 0.0f;
  return kpaToPsi(diff);
}

float decodeFuelRateGph(uint8_t a, uint8_t b) {
  float lph = ((a * 256) + b) / 20.0f;
  return lph / 3.785411784f;            // L/h -> US gallons/hour
}

float decodeEgtMaxF(const uint8_t* d, int n) {
  float maxC = -1000.0f;
  for (int s = 0; s < 3; s++) {         // sensors at d[1..2], d[3..4], d[5..6]
    int hi = 1 + s * 2, lo = 2 + s * 2;
    if (lo >= n) break;
    float c = ((d[hi] * 256) + d[lo]) / 10.0f - 40.0f;
    if (c > maxC) maxC = c;
  }
  if (maxC < -900.0f) return 0.0f;
  return cToF(maxC);
}

float decodeDpfDeltaKpa(uint8_t b, uint8_t c) {
  int16_t raw = (int16_t)(((uint16_t)b << 8) | c);   // signed
  return raw / 100.0f;
}

float decodeDpfTempF(uint8_t b, uint8_t c) {
  return cToF(((b * 256) + c) / 10.0f - 40.0f);
}

float decodeLoadPct(uint8_t a) { return a * 100.0f / 255.0f; }

float decodeRailPsi(uint8_t a, uint8_t b) { return ((a * 256) + b) * 10.0f * 0.1450377f; }
float decodeTorquePct(uint8_t a) { return static_cast<float>(a) - 125.0f; }
float decodeRefTorqueNm(uint8_t a, uint8_t b) { return static_cast<float>((a * 256) + b); }
float decodeDefLevelPct(uint8_t b) { return b / 2.55f; }

float decodeFuelLevelPct(uint8_t a) { return a / 2.55f; }

// Gallons to add to fill a tank: capacity * (1 - level%/100), clamped to [0, capacity].
// OBD sender % is non-linear, so this is an estimate.
float gallonsToFill(float capacityGal, float levelPct) {
  float g = capacityGal * (1.0f - levelPct / 100.0f);
  if (g < 0.0f) return 0.0f;
  if (g > capacityGal) return capacityGal;
  return g;
}

// Engine horsepower from actual-torque %, reference torque (N·m), and RPM.
// hp = (act%/100)*refNm*rpm / 7121. Returns 0 on overrun (negative torque) or
// before the helper PIDs have been read.
float computeHorsepower(float actPct, float refNm, float rpm) {
  if (actPct <= 0.0f || refNm <= 0.0f || rpm <= 0.0f) return 0.0f;
  float hp = actPct / 100.0f * refNm * rpm / 7121.0f;
  return hp < 0.0f ? 0.0f : hp;
}

float decodeMafGps(uint8_t a, uint8_t b) { return ((a * 256) + b) / 100.0f; }
float decodeNoxPpm(uint8_t b, uint8_t c) { return (float)((b * 256) + c); }
