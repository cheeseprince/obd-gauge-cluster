#pragma once
#include <cstdint>
#include "rtc.h"    // DateTime (auto-night resolver)
#include "solar.h"  // GeoLocation

// Night theme tri-state (Settings::nightMode). Auto follows the solar calc
// from the RTC; Day/Night are manual pins.
enum : uint8_t { NIGHT_AUTO = 0, NIGHT_DAY = 1, NIGHT_NIGHT = 2 };

// User-adjustable display settings (set via the long-press menu, persisted to NVS).
struct Settings {
  bool        night         = false;   // ACTIVE night theme state (resolved; boot theme)
  uint8_t     nightMode     = NIGHT_AUTO;  // Auto (RTC solar calc) / Day / Night
  uint8_t     brightnessPct = 100;     // backlight level (one of the presets)
  bool        metric        = false;   // false = mph/°F, true = km/h/°C
  bool        logging       = true;    // SD CSV logging enabled (auto every drive)
  GeoLocation geo;                     // provisioned location (unset sentinel by default)
  char vehicleKey[24] = "";            // registry key of the selected profile ("" = default/Generic)
  bool        vehicleAuto   = true;    // NVS "vehauto". true = VIN auto-detect governs; false = manually locked.
};

// Next brightness preset, cycling 25->50->75->100->25. A non-preset value
// snaps up to the next-higher preset.
uint8_t nextBrightness(uint8_t cur);

// Flip night mode and apply the convenience brightness (night on -> 25,
// night off -> 100). The Brightness item can override afterward.
void toggleNight(Settings& s);

// Cycle the Night menu row: Auto -> Day -> Night -> Auto. Boards without an
// RTC (hasRtc=false) skip Auto — there is no clock to drive it.
void cycleNightMode(Settings& s, bool hasRtc);

// Menu label for the Night row: "AUTO" / "OFF" (day) / "ON" (night).
const char* nightModeLabel(const Settings& s);

// Resolve the ACTIVE night flag for this tick. Day/Night modes are fixed;
// Auto uses the solar sunrise/sunset calc when the RTC is valid, and keeps
// curNight (no flapping) when it isn't.
bool resolveNight(const Settings& s, bool rtcValid, const DateTime& now, bool curNight);

// NVS persistence (device only; no-op declarations on host — never linked there).
void loadSettings(Settings& s);
void saveSettings(const Settings& s);
