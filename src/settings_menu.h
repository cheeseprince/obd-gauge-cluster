#pragma once
#include <cstdint>

// Long-press settings menu — flat list, rotate to move, click to act in place.
enum class MenuItem : uint8_t {
  NightMode, Brightness, Units, SetTime, Logging, ResetTrip, ForgetAdapter,
  PickVehicle, WifiSetup, CheckUpdate, Version, Close, COUNT
};

// What the main loop should do when a row is activated.
enum class MenuAction : uint8_t {
  None, ToggleNight, CycleBrightness, ToggleUnits, OpenTimeSet, ToggleLogging,
  ResetTrip, ForgetAdapter, OpenVehiclePick, OpenWifiSetup, CheckUpdate, ShowVersion, CloseMenu
};

struct MenuState {
  uint8_t  sel   = 0;                  // highlighted item index [0, MenuItem::COUNT)
  MenuItem armed = MenuItem::COUNT;    // COUNT = nothing armed for confirm
};

void       menuReset(MenuState& m);            // sel=0, armed=COUNT
void       menuMove(MenuState& m, int dir);    // ±1, wraps; clears armed
MenuAction menuActivate(MenuState& m);         // act on the highlighted row

// Per-board row visibility (set once at input begin(); defaults = dash shape).
// SetTime is meaningless without an RTC, Logging without an SD slot, and the
// WiFi-setup/Check-update rows need the OTA stack (S3 boards) — hidden rows
// are skipped by menuMove and never rendered, so they can't be activated.
void menuSetCaps(bool hasRtc, bool hasSdLog, bool hasOta);
bool menuItemVisible(MenuItem it);
