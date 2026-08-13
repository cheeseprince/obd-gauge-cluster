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
  // Identity of a vehicle we can NAME but have no profile for (e.g. a Ford
  // Super Duty). Empty when the vehicle IS profiled -- the profile supplies its
  // own name -- or when nothing has been identified yet.
  //
  // These hold the RESOLVED STRINGS, not the VIN. Persisting the VIN and
  // re-running vinIdentify() at boot would be the DRY choice and would let a
  // firmware update improve the name for free, but it writes a full VIN to
  // flash -- a privacy surface this project does not otherwise have. The cost
  // of storing strings instead is one boot of staleness: the next OBD connect
  // re-identifies and overwrites.
  char detectedName[24]   = "";        // NVS "detname"
  char detectedEngine[24] = "";        // NVS "deteng"
  // Diesel tank capacity override, in US gallons. 0 = unset (use the profile's
  // factory figure, or show SET UP when the profile does not know either).
  //
  // Scoped by `tankVeh`, the vehicle-registry key it was set for: capacity is a
  // property of the TRUCK, but Settings is global. Without the key, a 48 gal
  // override entered on a Super Duty would follow the user to the Sierra and
  // silently displace its sourced 24.0.
  //
  // There is no DEF equivalent: DEF capacity does not vary by cab or bed, so it
  // stays a per-profile constant (VehicleProfile::defTankGal).
  float tankGal = 0.0f;                // NVS "tankgal"
  char  tankVeh[24] = "";              // NVS "tankveh"
};

// The tank override IF it belongs to the active profile, else 0. `activeKey` is
// the vehicle-registry key of the running profile (vehicleKeyActive()).
// Pure — no NVS, no platform types — so the scoping rule is host-testable.
float tankOverrideFor(const Settings& s, const char* activeKey);

// Record an override against the active profile. gal <= 0 clears it.
void  setTankOverride(Settings& s, float gal, const char* activeKey);

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
