#include <cstdio>
#include <cmath>
#include "solar.h"
#include "rtc.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

// Reference sunrise/sunset for New York NY (40.71N, 74.01W) — the same neutral
// reference city the setup portal uses for its placeholder examples (see
// ota_portal.cpp: placeholders are never the developer's own location). Local
// wall clock, from published almanac tables. The NOAA-style algorithm is good
// to ~2-3 min; allow +/-15 min so table rounding + recall slack can't false-fail.
static bool near(float got, float wantH, float wantM, const char* m) {
  float want = wantH + wantM / 60.0f;
  bool ok = std::fabs(got - want) <= 0.25f;
  if (!ok) printf("FAIL: %s  got=%.2f want=%.2f\n", m, got, want);
  return ok;
}

int main() {
  // Neutral reference city — New York NY, EST std offset.
  const GeoLocation NY{40.71f, -74.01f, -5};

  // ── US DST rule: starts 2nd Sunday March 02:00, ends 1st Sunday Nov 02:00.
  // 2026: starts Mar 8, ends Nov 1.
  check(!isUsDst(2026, 3, 7, 12), "Mar 7 2026: standard time");
  check(!isUsDst(2026, 3, 8, 1),  "Mar 8 2026 01:00: still standard");
  check( isUsDst(2026, 3, 8, 3),  "Mar 8 2026 03:00: DST");
  check( isUsDst(2026, 7, 1, 12), "July: DST");
  check( isUsDst(2026, 11, 1, 1), "Nov 1 2026 01:00: still DST");
  check(!isUsDst(2026, 11, 1, 3), "Nov 1 2026 03:00: standard");
  check(!isUsDst(2026, 12, 21, 12), "December: standard");
  check(!isUsDst(2026, 1, 15, 12),  "January: standard");

  // ── Sunrise/sunset, New York, local wall clock (handles DST internally).
  float rise, set;
  // Summer solstice 2026 (EDT): sunrise ~05:25, sunset ~20:31.
  check(sunTimesLocal(2026, 6, 21, NY, rise, set), "Jun 21: sun rises and sets");
  failures += !near(rise, 5, 25, "Jun 21 sunrise");
  failures += !near(set, 20, 31, "Jun 21 sunset");
  // Winter solstice 2026 (EST): sunrise ~07:16, sunset ~16:32.
  check(sunTimesLocal(2026, 12, 21, NY, rise, set), "Dec 21: sun rises and sets");
  failures += !near(rise, 7, 16, "Dec 21 sunrise");
  failures += !near(set, 16, 32, "Dec 21 sunset");
  // Sanity: an equinox day is ~12h; summer day longer than winter day.
  check(sunTimesLocal(2026, 3, 20, NY, rise, set) && (set - rise) > 11.5f && (set - rise) < 12.6f,
        "Mar 20 equinox: ~12h daylight");

  // ── isNight(local wall clock) — the single call the firmware uses.
  check(!isNight({2026, 6, 21, 12, 0, 0}, NY),  "summer noon = day");
  check(!isNight({2026, 6, 21, 20, 0, 0}, NY),  "summer 8:00pm = still day (sunset 8:31)");
  check( isNight({2026, 6, 21, 21, 30, 0}, NY), "summer 9:30pm = night");
  check( isNight({2026, 6, 21, 5, 0, 0}, NY),   "summer 5:00am = night (sunrise 5:25)");
  check(!isNight({2026, 12, 21, 12, 0, 0}, NY), "winter noon = day");
  check( isNight({2026, 12, 21, 17, 30, 0}, NY),"winter 5:30pm = night (sunset 4:32)");
  check(!isNight({2026, 12, 21, 8, 0, 0}, NY),  "winter 8:00am = day (sunrise 7:16)");
  check( isNight({2026, 12, 21, 6, 30, 0}, NY), "winter 6:30am = night");

  // ── geoValid: unset sentinel and out-of-range values are rejected.
  check( geoValid(NY),                         "valid location accepted");
  check(!geoValid({200.0f, 0, 0}),             "unset sentinel rejected");
  check(!geoValid({91.0f, 0, 0}),              "lat out of range rejected");
  check(!geoValid({0.0f, 181.0f, 0}),          "lon out of range rejected");

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
