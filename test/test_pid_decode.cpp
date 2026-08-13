// Host unit tests for pid_decode. Values cross-checked against the design spec
// §4 (e.g. trans 185°F = 85°C = byte 0x7D; RPM 0x1AF8 = 1726).
#include "pid_decode.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void checkf(const char* name, float got, float want) {
  if (std::fabs(got - want) < 0.05f) {
    std::printf("  ok   %s\n", name);
  } else {
    std::printf("  FAIL %s (got %.3f, want %.3f)\n", name, got, want);
    ++g_failures;
  }
}
static void check(bool cond, const char* msg) {
  if (cond) {
    std::printf("  ok   %s\n", msg);
  } else {
    std::printf("  FAIL %s\n", msg);
    ++g_failures;
  }
}

int main() {
  std::printf("conversions:\n");
  checkf("cToF-0", cToF(0.0f), 32.0f);
  checkf("cToF-100", cToF(100.0f), 212.0f);
  checkf("kphToMph-100", kphToMph(100.0f), 62.137f);
  checkf("kpaToPsi-100", kpaToPsi(100.0f), 14.504f);

  std::printf("decoders:\n");
  // Temp: A - 40 (deg C). 0x7D=125 -> 85C -> 185F (validated trans-temp value).
  checkf("trans-185F", cToF(decodeTempC(125)), 185.0f);
  checkf("coolant-205F", cToF(decodeTempC(136)), 204.8f);
  // RPM: ((A*256)+B)/4. 0x1A,0xF8 -> 1726.
  checkf("rpm", decodeRpm(0x1A, 0xF8), 1726.0f);
  // Speed: A km/h -> mph.
  checkf("speed-100kph", kphToMph(decodeSpeedKph(100)), 62.137f);
  // Volts: ((A*256)+B)/1000. 0x37,0x78 = 14200 -> 14.2 V.
  checkf("volts", decodeModuleVolts(0x37, 0x78), 14.2f);
  // Boost: (MAP - baro) kPa -> psi, clamped at 0.
  checkf("boost-pos", boostPsi(180.0f, 100.0f), 11.603f);
  checkf("boost-clamp", boostPsi(95.0f, 100.0f), 0.0f);

  // --- Sub-project B diesel decoders ---
  // Fuel rate: 0x001F = 31 -> 1.55 L/h -> 0.4095 gph
  check(fabsf(decodeFuelRateGph(0x00, 0x1F) - 0.4095f) < 0.01f, "fuel rate 0x001F -> ~0.41 gph");
  // EGT: sensors 183/150/122 C (max 183 -> 361.4 F). d[0]=mask, then 3 sensor pairs.
  { uint8_t d[7] = {0x07, 0x08,0xB6, 0x07,0x6C, 0x06,0x54};
    check(fabsf(decodeEgtMaxF(d, 7) - 361.4f) < 1.0f, "EGT max 183C -> ~361F"); }
  // EGT with only 1 sensor present (n=3): 183C -> 361.4F
  { uint8_t d[3] = {0x01, 0x08,0xB6};
    check(fabsf(decodeEgtMaxF(d, 3) - 361.4f) < 1.0f, "EGT 1 sensor -> 361F"); }
  // DPF dP: 0x0064 -> 1.00 kPa; signed negative 0xFFFF -> -0.01
  check(fabsf(decodeDpfDeltaKpa(0x00, 0x64) - 1.00f) < 0.01f, "DPF dP 0x0064 -> 1.0 kPa");
  check(decodeDpfDeltaKpa(0xFF, 0xFF) < 0.0f, "DPF dP 0xFFFF -> negative");
  // DPF temp: 0x0960 = 2400 -> 200 C -> 392 F
  check(fabsf(decodeDpfTempF(0x09, 0x60) - 392.0f) < 1.0f, "DPF temp 200C -> 392F");

  // Engine load (PID 0104): A*100/255. 0->0, 255->100, 128->50.196.
  checkf("load-0", decodeLoadPct(0), 0.0f);
  checkf("load-255", decodeLoadPct(255), 100.0f);
  checkf("load-128", decodeLoadPct(128), 50.196f);

  // Fuel rail pressure (PID 23): ((A*256)+B)*10 kPa -> psi. 0x0E4C -> ~5308 psi.
  check(fabsf(decodeRailPsi(0x0E, 0x4C) - 5308.4f) < 2.0f, "rail 0x0E4C -> ~5308 psi");
  // Torque % (PID 62/61): A-125. 0xAF -> 50, 0x7D -> 0, 0x73 -> -10 (overrun).
  checkf("torque-50", decodeTorquePct(0xAF), 50.0f);
  checkf("torque-0", decodeTorquePct(0x7D), 0.0f);
  // Reference torque (PID 63): (A*256)+B N·m. 0x0262 -> 610.
  checkf("reftq-610", decodeRefTorqueNm(0x02, 0x62), 610.0f);
  // DEF level (PID 9B byte B): B/2.55 %. 0x84 -> ~51.76%.
  check(fabsf(decodeDefLevelPct(0x84) - 51.76f) < 0.1f, "DEF 0x84 -> ~52%");
  // Computed HP: actPct/100*refNm*rpm/7121, clamped >=0.
  check(fabsf(computeHorsepower(50.0f, 610.0f, 905.0f) - 38.76f) < 1.0f, "HP 50%/610/905 -> ~39");
  check(fabsf(computeHorsepower(13.0f, 610.0f, 864.0f) - 9.62f) < 1.0f, "HP idle ~10");
  check(computeHorsepower(-10.0f, 610.0f, 905.0f) == 0.0f, "HP overrun (neg torque) -> 0");
  check(computeHorsepower(50.0f, 0.0f, 905.0f) == 0.0f, "HP no refTq -> 0");

  // Fuel level (PID 2F): A/2.55 %. 0xC4 -> ~77%, 0xFF -> 100%.
  check(fabsf(decodeFuelLevelPct(0xC4) - 76.86f) < 0.2f, "fuel level 0xC4 -> ~77%");
  checkf("fuel-100", decodeFuelLevelPct(0xFF), 100.0f);
  // gallons to fill = capacity*(1-level%/100), clamped to [0,capacity].
  check(fabsf(gallonsToFill(24.0f, 77.0f) - 5.52f) < 0.1f, "diesel 24gal@77% -> ~5.5 to fill");
  check(fabsf(gallonsToFill(5.4f, 52.0f) - 2.59f) < 0.1f, "DEF 5.4gal@52% -> ~2.6 to fill");
  check(gallonsToFill(24.0f, 100.0f) == 0.0f, "full tank -> 0 to fill");
  check(fabsf(gallonsToFill(24.0f, 0.0f) - 24.0f) < 0.01f, "empty tank -> full capacity");
  check(gallonsToFill(24.0f, 130.0f) == 0.0f, "over-100% clamps to 0");

  // THE TRAP the capacity gate exists to catch: an unknown capacity does not
  // produce a NaN or an error, it produces a confident "0.0 gal to fill" --
  // i.e. "tank is full" -- at ANY fuel level. Callers must gate on capacity.
  check(gallonsToFill(0.0f, 10.0f) == 0.0f, "0 capacity @10% still returns 0.0 (why the gate exists)");

  // effectiveTankGal: override wins, else the profile figure, else 0 = unknown.
  checkf("eff-override-wins",   effectiveTankGal(48.0f, 24.0f), 48.0f);
  checkf("eff-falls-to-profile",effectiveTankGal(0.0f,  24.0f), 24.0f);
  checkf("eff-both-unset",      effectiveTankGal(0.0f,   0.0f),  0.0f);
  checkf("eff-override-on-unknown-profile", effectiveTankGal(34.0f, 0.0f), 34.0f);
  // Negative/NaN overrides must not poison the result — they fall through.
  checkf("eff-negative-override", effectiveTankGal(-5.0f, 24.0f), 24.0f);
  checkf("eff-nan-override",      effectiveTankGal(NAN,   24.0f), 24.0f);
  checkf("eff-nan-profile",       effectiveTankGal(0.0f,  NAN),    0.0f);

  // MAF (PID 10): ((A*256)+B)/100 g/s. 0x07D2 -> 20.02.
  check(fabsf(decodeMafGps(0x07,0xD2) - 20.02f) < 0.05f, "MAF 0x07D2 -> 20.02 g/s");
  // NOx (PID 83): ((B*256)+C) ppm. 0x00,0x93 -> 147.
  check(decodeNoxPpm(0x00,0x93) == 147.0f, "NOx 0x0093 -> 147 ppm");

  if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
  std::printf("\n%d FAILURE(S)\n", g_failures);
  return 1;
}
