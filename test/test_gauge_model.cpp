// Host unit tests for gauge_model. Thresholds match design spec §5.
#include "gauge_model.h"
#include <cstdio>
#include <cmath>

static int g_failures = 0;
static void check(const char* name, Zone got, Zone want) {
  if (got == want) { std::printf("  ok   %s\n", name); }
  else { std::printf("  FAIL %s (got %d, want %d)\n", name,
                     static_cast<int>(got), static_cast<int>(want)); ++g_failures; }
}
static void checkf(const char* name, float got, float want) {
  if (std::fabs(got - want) < 0.05f) { std::printf("  ok   %s\n", name); }
  else { std::printf("  FAIL %s (got %.3f, want %.3f)\n", name, got, want); ++g_failures; }
}
static void checkb(const char* name, bool got, bool want) {
  if (got == want) { std::printf("  ok   %s\n", name); }
  else { std::printf("  FAIL %s (got %d, want %d)\n", name, got, want); ++g_failures; }
}

int main() {
  // Trans temp: higher-is-worse, warnHi 235, critHi 255.
  Thresholds trans; trans.warnHi = 235.0f; trans.critHi = 255.0f;
  std::printf("zones (trans):\n");
  check("green", zoneFor(185.0f, trans), Zone::Green);
  check("amber", zoneFor(240.0f, trans), Zone::Amber);
  check("red",   zoneFor(256.0f, trans), Zone::Red);

  // Volts: two-sided, warnHi 15, warnLo 13, critLo 12.
  Thresholds volts; volts.warnHi = 15.0f; volts.warnLo = 13.0f; volts.critLo = 12.0f;
  std::printf("zones (volts):\n");
  check("v-green", zoneFor(14.2f, volts), Zone::Green);
  check("v-amber-hi", zoneFor(15.5f, volts), Zone::Amber);
  check("v-amber-lo", zoneFor(12.5f, volts), Zone::Amber);
  check("v-red-lo", zoneFor(11.9f, volts), Zone::Red);

  std::printf("peak-hold:\n");
  Gauge g;
  checkb("invalid-start", g.valid, false);
  gaugeUpdate(g, 185.0f);
  checkf("v1", g.value, 185.0f);
  checkf("pk1", g.peak, 185.0f);
  checkb("valid-now", g.valid, true);
  gaugeUpdate(g, 192.0f);
  checkf("pk-rises", g.peak, 192.0f);
  gaugeUpdate(g, 180.0f);
  checkf("v-falls", g.value, 180.0f);
  checkf("pk-holds", g.peak, 192.0f);
  gaugeResetPeak(g);
  checkf("pk-reset", g.peak, 180.0f);

  if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
  std::printf("\n%d FAILURE(S)\n", g_failures);
  return 1;
}
