#include <cstdio>
#include "settings.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  // Defaults.
  Settings s;
  check(!s.night && s.brightnessPct == 100 && !s.metric, "defaults: day/100/imperial");
  check(s.logging == true, "logging defaults on");
  // vehicleKey defaults empty ("" = Generic fallback). loadSettings()'s NVS body
  // is compiled out on host (guarded by #ifdef ARDUINO; this Makefile has no
  // -DARDUINO), so this asserts the struct's in-memory default directly rather
  // than calling loadSettings().
  check(s.vehicleKey[0] == '\0', "vehicleKey defaults empty");
  check(s.vehicleAuto, "vehicleAuto defaults true");

  // Brightness presets cycle 25->50->75->100->25.
  check(nextBrightness(25)  == 50,  "25 -> 50");
  check(nextBrightness(50)  == 75,  "50 -> 75");
  check(nextBrightness(75)  == 100, "75 -> 100");
  check(nextBrightness(100) == 25,  "100 -> 25 (wrap)");
  // Non-preset snaps up to the next preset.
  check(nextBrightness(10)  == 25,  "10 -> 25");
  check(nextBrightness(60)  == 75,  "60 -> 75");

  // Night toggle sets the convenience brightness.
  Settings n;                       // day, 100
  toggleNight(n);
  check(n.night && n.brightnessPct == 25, "night ON -> night + 25%");
  toggleNight(n);
  check(!n.night && n.brightnessPct == 100, "night OFF -> day + 100%");

  // Override survives: after toggling night on, brightness can be raised
  // independently and is NOT reset by anything but another night toggle.
  Settings o; toggleNight(o);        // night, 25
  o.brightnessPct = nextBrightness(o.brightnessPct);  // 50
  check(o.night && o.brightnessPct == 50, "brightness overrides night default, night flag intact");

  // ── Night tri-state (Auto/Day/Night) ──────────────────────────────────────
  Settings t;
  check(t.nightMode == NIGHT_AUTO, "nightMode defaults to Auto");
  cycleNightMode(t, /*hasRtc=*/true);
  check(t.nightMode == NIGHT_DAY, "Auto -> Day");
  cycleNightMode(t, true);
  check(t.nightMode == NIGHT_NIGHT, "Day -> Night");
  cycleNightMode(t, true);
  check(t.nightMode == NIGHT_AUTO, "Night -> Auto (wraps, RTC board)");
  // No-RTC board (knob): Auto is skipped.
  t.nightMode = NIGHT_NIGHT;
  cycleNightMode(t, /*hasRtc=*/false);
  check(t.nightMode == NIGHT_DAY, "Night -> Day when no RTC (Auto skipped)");

  // Labels.
  t.nightMode = NIGHT_AUTO;  check(nightModeLabel(t)[0] == 'A', "label AUTO");
  t.nightMode = NIGHT_NIGHT; check(nightModeLabel(t)[1] == 'N', "label ON");
  t.nightMode = NIGHT_DAY;   check(nightModeLabel(t)[1] == 'F', "label OFF");

  // ── resolveNight ──────────────────────────────────────────────────────────
  DateTime noonSummer{2026, 6, 21, 12, 0, 0};
  DateTime lateSummer{2026, 6, 21, 22, 0, 0};
  Settings r;
  r.nightMode = NIGHT_DAY;
  check(!resolveNight(r, true, lateSummer, true), "Day mode pins day even at 10pm");
  r.nightMode = NIGHT_NIGHT;
  check(resolveNight(r, true, noonSummer, false), "Night mode pins night even at noon");
  r.nightMode = NIGHT_AUTO;
  r.geo = GeoLocation{40.71f, -74.01f, -5};   // New York (neutral reference) — a set location is required for Auto to consult solar
  check(!resolveNight(r, true, noonSummer, true),  "Auto + valid RTC: noon = day");
  check( resolveNight(r, true, lateSummer, false), "Auto + valid RTC: 10pm = night");
  // Auto with no valid RTC holds the current state (no flapping).
  check( resolveNight(r, false, noonSummer, true),  "Auto + invalid RTC keeps night");
  check(!resolveNight(r, false, lateSummer, false), "Auto + invalid RTC keeps day");

  // resolveNight in AUTO mode must NOT consult the solar calc when no location
  // is set — it falls through to the current theme, exactly like a no-RTC board.
  {
    Settings s;                       // default geo is the unset sentinel
    s.nightMode = NIGHT_AUTO;
    check(!geoValid(s.geo), "default Settings has no location");
    // rtcValid true, but location unset -> returns curNight unchanged:
    check(resolveNight(s, /*rtcValid=*/true, {2026,6,21,21,30,0}, /*curNight=*/false) == false,
          "AUTO with unset location holds curNight (day)");
    check(resolveNight(s, /*rtcValid=*/true, {2026,6,21,21,30,0}, /*curNight=*/true) == true,
          "AUTO with unset location holds curNight (night)");
    // With a location set, AUTO consults the solar calc (9:30pm summer = night):
    s.geo = GeoLocation{40.71f, -74.01f, -5};
    check(resolveNight(s, /*rtcValid=*/true, {2026,6,21,21,30,0}, /*curNight=*/false) == true,
          "AUTO with location resolves night from solar calc");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
