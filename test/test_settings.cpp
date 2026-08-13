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

  // --- Tank capacity override, scoped to the vehicle it was set for ---------
  {
    Settings s;
    check(tankOverrideFor(s, "ford_sd_67") == 0.0f, "fresh Settings has no tank override");

    setTankOverride(s, 48.0f, "ford_sd_67");
    check(tankOverrideFor(s, "ford_sd_67") == 48.0f, "override applies to the profile it was set for");
    // THE POINT of the scoping: it must NOT leak onto another truck, whose own
    // profile already knows its capacity.
    check(tankOverrideFor(s, "gm_sierra_lz0") == 0.0f, "override does not follow to another profile");

    // Re-setting on a different profile re-homes it rather than stacking.
    setTankOverride(s, 60.0f, "gm_sierra_lz0");
    check(tankOverrideFor(s, "gm_sierra_lz0") == 60.0f, "re-set applies to the new profile");
    check(tankOverrideFor(s, "ford_sd_67") == 0.0f, "re-set releases the old profile");

    // Clearing forgets the owner too, so it cannot resurrect on a key match.
    setTankOverride(s, 0.0f, "gm_sierra_lz0");
    check(tankOverrideFor(s, "gm_sierra_lz0") == 0.0f, "clearing removes the override");
    check(s.tankVeh[0] == '\0', "clearing also forgets which profile owned it");

    // The default/Generic profile has an empty key; an override must still bind.
    setTankOverride(s, 29.0f, "");
    check(tankOverrideFor(s, "") == 29.0f, "override binds to the empty (default) profile key");
    check(tankOverrideFor(s, nullptr) == 29.0f, "null active key is treated as the empty key");
    check(tankOverrideFor(s, "ford_sd_67") == 0.0f, "default-profile override does not leak to Ford");

    // A key longer than the field must truncate safely, not overflow.
    setTankOverride(s, 34.0f, "a_very_long_vehicle_registry_key_beyond_the_field");
    check(s.tankVeh[sizeof s.tankVeh - 1] == '\0', "long profile key stays NUL-terminated");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
