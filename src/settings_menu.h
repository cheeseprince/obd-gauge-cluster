#pragma once
#include <cstdint>

// Long-press settings menu — flat list, rotate to move, click to act in place.
enum class MenuItem : uint8_t {
  NightMode, Brightness, Units, FuelTank, SetTime, Logging, ResetTrip, ForgetAdapter,
  PickVehicle, WifiSetup, CheckUpdate, Version, Close, COUNT
};

// What the main loop should do when a row is activated.
enum class MenuAction : uint8_t {
  None, ToggleNight, CycleBrightness, ToggleUnits, OpenTimeSet, ToggleLogging,
  ResetTrip, ForgetAdapter, OpenVehiclePick, OpenWifiSetup, CheckUpdate, ShowVersion,
  OpenTankPick, CloseMenu
};

struct MenuState {
  uint8_t  sel   = 0;                  // highlighted item index [0, MenuItem::COUNT)
  MenuItem armed = MenuItem::COUNT;    // COUNT = nothing armed for confirm
  // Which choice is highlighted while `armed`. Destructive rows (Reset trip,
  // Forget adapter) open an inline Yes/No dialog: the knob toggles this instead
  // of moving the cursor, and a click acts on it. Defaults to No on every arm.
  bool     confirmYes = false;
};

void       menuReset(MenuState& m);            // sel=0, armed=COUNT, confirmYes=false
void       menuMove(MenuState& m, int dir);    // armed: toggle Yes/No. else: ±1, wraps
MenuAction menuActivate(MenuState& m);         // act on the highlighted row / choice

// Per-board row visibility (set once at input begin(); defaults = dash shape).
// SetTime is meaningless without an RTC, Logging without an SD slot, and the
// WiFi-setup/Check-update rows need the OTA stack (S3 boards) — hidden rows
// are skipped by menuMove and never rendered, so they can't be activated.
void menuSetCaps(bool hasRtc, bool hasSdLog, bool hasOta);
bool menuItemVisible(MenuItem it);

// First row of a scrolling window of `vis` rows over a list of `n`, chosen so
// `cursor` is inside it. Both the settings menu and the tank picker outgrew the
// 320 px panel; this is the shared arithmetic that decouples list length from
// screen height. Keeps the cursor near the middle rather than paging, so a turn
// always visibly moves something. Returns 0 when the whole list fits.
int menuWindowTop(int cursor, int n, int vis);

// ── Fuel-tank capacity picker ───────────────────────────────────────────────
// Diesel capacity cannot be derived: a Super Duty is 29/34/48 gal depending on
// WHEELBASE (Ford Owner's Manual, Capacities), the VIN encodes neither cab nor
// bed, and any truck can be refitted with a larger aftermarket tank. So the
// user picks it, from a list that spans pickups, chassis cabs and aftermarket
// tanks. A superset costs one extra line of scroll; a missing size would cost
// a wrong reading, and Custom covers whatever the list misses.
extern const float       TANK_PRESETS[];      // US gallons, ascending
extern const char* const TANK_HINTS[];        // parallel: what each size belongs to
extern const int         TANK_PRESET_COUNT;

// Rows are [0, TANK_PRESET_COUNT) presets, then Custom, then Unset.
int tankRowCustom();     // == TANK_PRESET_COUNT
int tankRowUnset();      // == TANK_PRESET_COUNT + 1
int tankRowCount();      // == TANK_PRESET_COUNT + 2

struct TankPickState {
  uint8_t sel       = 0;        // highlighted row
  bool    editing   = false;    // true while the knob is adjusting customGal
  float   customGal = 30.0f;    // working value for the Custom row
};

// Open the picker on the row matching `curGal` (an exact preset if there is
// one, else Custom seeded to that value, else Unset). This is what makes the
// common case one click: the cursor already sits on the current answer.
void tankPickSeed(TankPickState& t, float curGal);

// One detent. While editing, adjusts customGal by 0.5 gal; otherwise moves the
// row cursor with wrap.
void tankPickMove(TankPickState& t, int dir);

// Short press. Returns true when a choice is COMMITTED, writing the chosen
// capacity to `outGal` (0 = Unset, i.e. clear the override). Returns false when
// the press only entered Custom's edit mode and the picker should stay open.
bool tankPickActivate(TankPickState& t, float& outGal);
