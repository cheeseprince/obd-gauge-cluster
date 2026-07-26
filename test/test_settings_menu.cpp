#include <cstdio>
#include "settings_menu.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  MenuState m;
  check(m.sel == 0 && m.armed == MenuItem::COUNT, "init: sel 0, nothing armed");

  // Move wraps both directions and clears any armed confirm.
  menuMove(m, -1);
  check(m.sel == (uint8_t)MenuItem::Close, "wrap up to Close");   // last item
  check((int)MenuItem::COUNT == 12, "12 menu items");
  menuMove(m, +1);
  check(m.sel == 0, "wrap down to NightMode");

  // Non-destructive items act immediately.
  m.sel = (uint8_t)MenuItem::NightMode;
  check(menuActivate(m) == MenuAction::ToggleNight, "NightMode -> ToggleNight");
  m.sel = (uint8_t)MenuItem::Brightness;
  check(menuActivate(m) == MenuAction::CycleBrightness, "Brightness -> CycleBrightness");
  m.sel = (uint8_t)MenuItem::Units;
  check(menuActivate(m) == MenuAction::ToggleUnits, "Units -> ToggleUnits");
  m.sel = (uint8_t)MenuItem::SetTime;
  check(menuActivate(m) == MenuAction::OpenTimeSet, "SetTime -> OpenTimeSet");
  m.sel = (uint8_t)MenuItem::Logging;
  check(menuActivate(m) == MenuAction::ToggleLogging, "Logging -> ToggleLogging");
  m.sel = (uint8_t)MenuItem::PickVehicle;
  check(menuActivate(m) == MenuAction::OpenVehiclePick, "PickVehicle -> OpenVehiclePick");
  m.sel = (uint8_t)MenuItem::WifiSetup;
  check(menuActivate(m) == MenuAction::OpenWifiSetup, "WifiSetup -> OpenWifiSetup");
  m.sel = (uint8_t)MenuItem::CheckUpdate;
  check(menuActivate(m) == MenuAction::CheckUpdate, "CheckUpdate -> CheckUpdate");
  m.sel = (uint8_t)MenuItem::Version;
  check(menuActivate(m) == MenuAction::ShowVersion, "Version -> ShowVersion");
  m.sel = (uint8_t)MenuItem::Close;
  check(menuActivate(m) == MenuAction::CloseMenu, "Close -> CloseMenu");

  // Destructive items need two activates (arm then fire).
  m.sel = (uint8_t)MenuItem::ResetTrip;
  check(menuActivate(m) == MenuAction::None, "ResetTrip first click arms");
  check(m.armed == MenuItem::ResetTrip, "armed = ResetTrip");
  check(menuActivate(m) == MenuAction::ResetTrip, "ResetTrip second click fires");
  check(m.armed == MenuItem::COUNT, "disarmed after fire");

  // Moving cancels a pending confirm.
  m.sel = (uint8_t)MenuItem::ForgetAdapter;
  check(menuActivate(m) == MenuAction::None, "ForgetAdapter arms");
  menuMove(m, +1);
  check(m.armed == MenuItem::COUNT, "move cancels arm");
  m.sel = (uint8_t)MenuItem::ForgetAdapter;
  check(menuActivate(m) == MenuAction::None, "re-arm after move");
  check(menuActivate(m) == MenuAction::ForgetAdapter, "ForgetAdapter fires on 2nd");

  // menuReset clears everything.
  menuReset(m);
  check(m.sel == 0 && m.armed == MenuItem::COUNT, "menuReset clears");

  // --- Capability-trimmed shape (no RTC, no SD, has OTA) ---
  menuSetCaps(false, false, true);
  check(!menuItemVisible(MenuItem::SetTime), "no rtc -> SetTime hidden");
  check(!menuItemVisible(MenuItem::Logging), "no sd -> Logging hidden");
  check(menuItemVisible(MenuItem::WifiSetup) && menuItemVisible(MenuItem::CheckUpdate),
        "OTA rows visible with hasOta");
  check(menuItemVisible(MenuItem::NightMode) && menuItemVisible(MenuItem::Close),
        "other rows stay visible");

  // No-OTA board (retired elecrow shape): both OTA rows hidden + skipped.
  // PickVehicle has no capability gate, so it stays visible regardless.
  menuSetCaps(true, true, false);
  check(!menuItemVisible(MenuItem::WifiSetup) && !menuItemVisible(MenuItem::CheckUpdate),
        "no ota -> OTA rows hidden");
  check(menuItemVisible(MenuItem::PickVehicle), "PickVehicle always visible (no cap gate)");
  MenuState o; menuReset(o);
  o.sel = (uint8_t)MenuItem::ForgetAdapter;
  menuMove(o, +1);                       // lands on PickVehicle (ungated, next row)
  check(o.sel == (uint8_t)MenuItem::PickVehicle, "move down reaches PickVehicle first");
  menuMove(o, +1);                       // skips WifiSetup AND CheckUpdate
  check(o.sel == (uint8_t)MenuItem::Version, "move down skips hidden OTA rows");
  menuSetCaps(false, false, true);       // back to the trimmed shape for the block below

  MenuState t; menuReset(t);
  t.sel = (uint8_t)MenuItem::Units;
  menuMove(t, +1);                       // skips SetTime AND Logging
  check(t.sel == (uint8_t)MenuItem::ResetTrip, "move down skips hidden rows");
  menuMove(t, -1);                       // and back up
  check(t.sel == (uint8_t)MenuItem::Units, "move up skips hidden rows");
  t.sel = 0;                             // NightMode
  menuMove(t, -1);
  check(t.sel == (uint8_t)MenuItem::Close, "wrap up still lands on visible last row");

  menuSetCaps(true, true, true);         // restore dash shape for any later checks
  MenuState u; u.sel = (uint8_t)MenuItem::Units;
  menuMove(u, +1);
  check(u.sel == (uint8_t)MenuItem::SetTime, "full caps: SetTime reachable again");

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
