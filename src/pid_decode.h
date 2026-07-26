#pragma once
#include <cstdint>

// Pure OBD-II signal math: raw data bytes -> engineering values, plus unit
// conversions. No Arduino/ELM headers so it unit-tests on the host with g++.
// "A"/"B" are the first/second data bytes of a PID response.

// --- unit conversions ---
float cToF(float celsius);
float kphToMph(float kph);
float kpaToPsi(float kpa);

// --- raw PID decoders (engineering value in native OBD units) ---
float decodeTempC(uint8_t a);                  // 05/5C/0F and Mode22 1940: A - 40 (C)
float decodeRpm(uint8_t a, uint8_t b);         // 0C: ((A*256)+B)/4
float decodeSpeedKph(uint8_t a);               // 0D: A (km/h)
float decodeMapKpa(uint8_t a);                 // 0B: A (kPa absolute)
float decodeModuleVolts(uint8_t a, uint8_t b); // 42: ((A*256)+B)/1000 (V)

// Boost = manifold absolute pressure minus barometric, clamped >= 0, in psi.
float boostPsi(float mapKpa, float baroKpa);

// --- Sub-project B diesel decoders ---
float decodeFuelRateGph(uint8_t a, uint8_t b);   // 5E: ((A*256)+B)/20 L/h -> US gph
float decodeEgtMaxF(const uint8_t* d, int n);    // 78: d[0]=support mask; max of 3 EGT sensors (C) -> F
float decodeDpfDeltaKpa(uint8_t b, uint8_t c);   // 7A: signed ((B*256)+C)/100 kPa (d[0]=mask)
float decodeDpfTempF(uint8_t b, uint8_t c);      // 7C: DPF inlet ((B*256)+C)/10-40 C -> F (d[0]=mask)
float decodeLoadPct(uint8_t a);                  // 04: A*100/255 (% calculated engine load)
float decodeRailPsi(uint8_t a, uint8_t b);       // 23: ((A*256)+B)*10 kPa -> psi (fuel rail)
float decodeTorquePct(uint8_t a);                // 62/61: A-125 (% of reference torque)
float decodeRefTorqueNm(uint8_t a, uint8_t b);   // 63: (A*256)+B (N·m reference torque)
float decodeDefLevelPct(uint8_t b);              // x/2.55 (%); apply to 9B byte[3]=tank level
float computeHorsepower(float actPct, float refNm, float rpm);  // actPct/100*refNm*rpm/7121, >=0
float decodeFuelLevelPct(uint8_t a);                          // 2F: A/2.55 (% tank)
float gallonsToFill(float capacityGal, float levelPct);      // capacity*(1-level/100), [0,cap]
float decodeMafGps(uint8_t a, uint8_t b);        // 10: ((A*256)+B)/100 (g/s mass air flow)
float decodeNoxPpm(uint8_t b, uint8_t c);        // 83: ((B*256)+C) ppm (NOx; d[0]=status)
