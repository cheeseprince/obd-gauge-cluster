#include "settings_menu.h"

static const int N = (int)MenuItem::COUNT;

// Factory sizes are quoted from the Ford Super Duty Owner's Manual, "Capacities
// and Specifications — 6.7L Diesel". "Incomplete vehicle" is Ford's term for a
// cab-and-chassis delivered without a bed, which is why three of the six
// factory sizes never appear on a pickup.
const float TANK_PRESETS[] = {
  24.0f, 26.5f, 29.0f, 34.0f, 40.0f, 48.0f, 55.0f, 58.0f, 60.0f, 65.0f, 66.5f, 68.0f
};
const char* const TANK_HINTS[] = {
  "Sierra 1500",        // 24.0  — gm_sierra_lz0 factory
  "SD chassis cab",     // 26.5  — incomplete vehicle, middle
  "SD 142/148in WB",    // 29.0  — pickup
  "SD 160/164in WB",    // 34.0  — pickup
  "SD chassis, aft",    // 40.0  — incomplete vehicle, aft-axle
  "SD 176in WB",        // 48.0  — pickup
  "aftermarket",        // 55.0
  "aftermarket",        // 58.0
  "aftermarket",        // 60.0
  "aftermarket",        // 65.0
  "SD chassis, both",   // 66.5  — incomplete vehicle, middle + aft-axle
  "aftermarket",        // 68.0
};
const int TANK_PRESET_COUNT = (int)(sizeof(TANK_PRESETS) / sizeof(TANK_PRESETS[0]));

int menuWindowTop(int cursor, int n, int vis) {
  if (vis <= 0 || n <= vis) return 0;          // whole list fits: no scrolling
  if (cursor < 0) cursor = 0;
  if (cursor >= n) cursor = n - 1;
  int top = cursor - vis / 2;
  // The early return above guarantees n > vis, so n - vis >= 1 and these two
  // clamps cannot fight each other -- either order gives the same answer. That
  // is only true BECAUSE of the guard; drop it and the order starts to matter.
  if (top > n - vis) top = n - vis;
  if (top < 0)       top = 0;
  return top;
}

int tankRowCustom() { return TANK_PRESET_COUNT; }
int tankRowUnset()  { return TANK_PRESET_COUNT + 1; }
int tankRowCount()  { return TANK_PRESET_COUNT + 2; }

// Custom is clamped to a range wide enough for anything road-legal without
// letting a spun knob land on a nonsense capacity: 1 gal is below every real
// tank, 200 is above the largest aftermarket transfer tank.
static const float TANK_CUSTOM_MIN = 1.0f, TANK_CUSTOM_MAX = 200.0f;
static const float TANK_CUSTOM_STEP = 0.5f;

void tankPickSeed(TankPickState& t, float curGal) {
  t.editing = false;
  if (curGal <= 0.0f) {                       // nothing set yet -> Unset row
    t.sel = (uint8_t)tankRowUnset();
    t.customGal = 30.0f;
    return;
  }
  for (int i = 0; i < TANK_PRESET_COUNT; i++) {
    // Exact-ish match: presets are 0.5-gal-resolution literals, so a tolerance
    // well under the step size cannot collapse two neighbouring presets.
    float d = TANK_PRESETS[i] - curGal;
    if (d > -0.01f && d < 0.01f) { t.sel = (uint8_t)i; t.customGal = curGal; return; }
  }
  t.sel = (uint8_t)tankRowCustom();           // a value the list does not carry
  t.customGal = curGal;
}

void tankPickMove(TankPickState& t, int dir) {
  int step = dir > 0 ? 1 : dir < 0 ? -1 : 0;
  if (!step) return;
  if (t.editing) {                            // adjusting the Custom value
    float v = t.customGal + step * TANK_CUSTOM_STEP;
    if (v < TANK_CUSTOM_MIN) v = TANK_CUSTOM_MIN;
    if (v > TANK_CUSTOM_MAX) v = TANK_CUSTOM_MAX;
    t.customGal = v;
    return;
  }
  int n = tankRowCount();
  int s = (int)t.sel + step;
  while (s < 0)  s += n;
  while (s >= n) s -= n;
  t.sel = (uint8_t)s;
}

bool tankPickActivate(TankPickState& t, float& outGal) {
  if (t.sel < (uint8_t)TANK_PRESET_COUNT) {   // a preset: commit straight away
    outGal = TANK_PRESETS[t.sel];
    return true;
  }
  if ((int)t.sel == tankRowUnset()) {         // clear the override
    outGal = 0.0f;
    return true;
  }
  // Custom: first press starts editing, second commits the adjusted value.
  if (!t.editing) { t.editing = true; return false; }
  t.editing = false;
  outGal = t.customGal;
  return true;
}

static bool g_hasRtc = true, g_hasSd = true, g_hasOta = true;   // default = dash shape

void menuSetCaps(bool hasRtc, bool hasSdLog, bool hasOta) {
  g_hasRtc = hasRtc; g_hasSd = hasSdLog; g_hasOta = hasOta;
}

bool menuItemVisible(MenuItem it) {
  if (it == MenuItem::SetTime) return g_hasRtc;
  if (it == MenuItem::Logging) return g_hasSd;
  if (it == MenuItem::WifiSetup || it == MenuItem::CheckUpdate) return g_hasOta;
  return true;
}

void menuReset(MenuState& m) { m.sel = 0; m.armed = MenuItem::COUNT; m.confirmYes = false; }

void menuMove(MenuState& m, int dir) {
  int step = dir > 0 ? 1 : dir < 0 ? -1 : 0;
  // While a confirm dialog is open the knob picks the CHOICE, not the row.
  // It deliberately does NOT disarm: the old behaviour cancelled a pending
  // confirm on any turn, so the dialog vanished silently and the action
  // appeared to do nothing.
  //
  // The mapping is ABSOLUTE (dir > 0 = Yes, dir < 0 = No), not a toggle. A
  // toggle makes the result a parity function of how many detents the knob
  // saw: this is called once per detent from encoder_input.cpp, so a 3-detent
  // spin toggles three times and the outcome depends on how fast the knob was
  // turned — a destructive choice must not depend on that. Absolute selection
  // is deterministic regardless of detent count, and matches the spatial
  // `< No >  < Yes >` idiom (right = Yes, left = No).
  if (m.armed != MenuItem::COUNT) { if (step) m.confirmYes = (step > 0); return; }
  if (!step) return;
  int s = (int)m.sel;
  for (int i = 0; i < N; i++) {          // at most one full lap
    s += step;
    while (s < 0)  s += N;
    while (s >= N) s -= N;
    if (menuItemVisible((MenuItem)s)) break;
  }
  m.sel = (uint8_t)s;
}

MenuAction menuActivate(MenuState& m) {
  MenuItem it = (MenuItem)m.sel;
  switch (it) {
    case MenuItem::NightMode:  m.armed = MenuItem::COUNT; return MenuAction::ToggleNight;
    case MenuItem::Brightness: m.armed = MenuItem::COUNT; return MenuAction::CycleBrightness;
    case MenuItem::Units:      m.armed = MenuItem::COUNT; return MenuAction::ToggleUnits;
    case MenuItem::FuelTank:   m.armed = MenuItem::COUNT; return MenuAction::OpenTankPick;
    case MenuItem::SetTime:    m.armed = MenuItem::COUNT; return MenuAction::OpenTimeSet;
    case MenuItem::Logging:    m.armed = MenuItem::COUNT; return MenuAction::ToggleLogging;
    case MenuItem::PickVehicle:m.armed = MenuItem::COUNT; return MenuAction::OpenVehiclePick;
    case MenuItem::WifiSetup:  m.armed = MenuItem::COUNT; return MenuAction::OpenWifiSetup;
    case MenuItem::CheckUpdate:m.armed = MenuItem::COUNT; return MenuAction::CheckUpdate;
    case MenuItem::Version:    m.armed = MenuItem::COUNT; return MenuAction::ShowVersion;
    case MenuItem::Close:      m.armed = MenuItem::COUNT; return MenuAction::CloseMenu;
    case MenuItem::ResetTrip:
    case MenuItem::ForgetAdapter:
      if (m.armed == it) {                       // dialog open: act on the choice
        bool yes = m.confirmYes;
        m.armed = MenuItem::COUNT;
        m.confirmYes = false;
        if (!yes) return MenuAction::None;       // No = cancel
        return it == MenuItem::ResetTrip ? MenuAction::ResetTrip
                                         : MenuAction::ForgetAdapter;
      }
      m.armed = it;                              // open the dialog, defaulting to No
      m.confirmYes = false;
      return MenuAction::None;
    default:
      return MenuAction::None;
  }
}
