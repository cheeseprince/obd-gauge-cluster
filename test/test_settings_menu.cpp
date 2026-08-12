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

  // Destructive items need: first click arms with No selected, toggle Yes, then click fires.
  m.sel = (uint8_t)MenuItem::ResetTrip;
  check(menuActivate(m) == MenuAction::None, "ResetTrip first click arms");
  check(m.armed == MenuItem::ResetTrip, "armed = ResetTrip");
  check(m.confirmYes == false, "default to No (safe)");
  menuMove(m, +1);
  check(m.confirmYes == true, "toggle to Yes");
  check(menuActivate(m) == MenuAction::ResetTrip, "ResetTrip fires after toggle+click");
  check(m.armed == MenuItem::COUNT, "disarmed after fire");

  // Moving no longer cancels: while armed the knob picks Yes/No. Cancelling a
  // pending confirm silently on any turn is the bug this replaces.
  m.sel = (uint8_t)MenuItem::ForgetAdapter;
  check(menuActivate(m) == MenuAction::None, "ForgetAdapter arms");
  menuMove(m, +1);
  check(m.armed == MenuItem::ForgetAdapter, "move keeps the dialog open");
  check(m.confirmYes == true, "move selects Yes");
  menuMove(m, +1);
  check(m.confirmYes == false, "move toggles back to No");

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

  // A board without OTA/RTC/SD caps: both OTA rows hidden + skipped. No such board
  // ships today, but menuSetCaps() takes the flags, so the path stays covered.
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

  // --- Yes/No confirm dialog on destructive rows ----------------------
  // Old behaviour was click-to-arm, click-again-to-confirm, and ANY knob turn
  // silently cancelled. Users reported "it says Forget adapter? and then
  // nothing happens". Now: click arms with No selected, turn toggles No/Yes,
  // click acts on the highlighted choice.
  {
    MenuState d; menuReset(d);
    d.sel = (uint8_t)MenuItem::ForgetAdapter;

    // First click arms, defaulting to No -- the safe choice.
    check(menuActivate(d) == MenuAction::None, "confirm: first click does not act");
    check(d.armed == MenuItem::ForgetAdapter, "confirm: first click arms");
    check(d.confirmYes == false, "confirm: defaults to No");

    // A turn toggles the CHOICE. It must not move the cursor and must not disarm
    // -- disarming on turn is exactly the old bug.
    menuMove(d, +1);
    check(d.confirmYes == true, "confirm: turn selects Yes");
    check(d.sel == (uint8_t)MenuItem::ForgetAdapter, "confirm: turn does not move the cursor");
    check(d.armed == MenuItem::ForgetAdapter, "confirm: turn does not disarm");

    menuMove(d, -1);
    check(d.confirmYes == false, "confirm: turn back selects No");
    menuMove(d, +1);
    check(d.confirmYes == true, "confirm: toggles regardless of direction");

    // Click on Yes fires and disarms.
    check(menuActivate(d) == MenuAction::ForgetAdapter, "confirm: Yes fires the action");
    check(d.armed == MenuItem::COUNT, "confirm: firing disarms");
  }
  {
    // Click on No cancels: no action, disarmed, cursor unmoved.
    MenuState d; menuReset(d);
    d.sel = (uint8_t)MenuItem::ForgetAdapter;
    menuActivate(d);                                  // arm (No)
    check(menuActivate(d) == MenuAction::None, "confirm: No cancels");
    check(d.armed == MenuItem::COUNT, "confirm: cancel disarms");
    check(d.sel == (uint8_t)MenuItem::ForgetAdapter, "confirm: cancel keeps the cursor");
  }
  {
    // Reset trip uses the same dialog -- leaving one row on double-click while
    // the other has a dialog is how mis-clicks happen.
    MenuState d; menuReset(d);
    d.sel = (uint8_t)MenuItem::ResetTrip;
    check(menuActivate(d) == MenuAction::None, "confirm: reset trip arms too");
    menuMove(d, +1);
    check(menuActivate(d) == MenuAction::ResetTrip, "confirm: reset trip fires on Yes");
  }
  {
    // Non-destructive rows are untouched: one click, acts immediately, and a
    // turn still moves the cursor.
    MenuState d; menuReset(d);
    d.sel = (uint8_t)MenuItem::NightMode;
    check(menuActivate(d) == MenuAction::ToggleNight, "confirm: normal row acts on one click");
    check(d.armed == MenuItem::COUNT, "confirm: normal row never arms");
    uint8_t before = d.sel;
    menuMove(d, +1);
    check(d.sel != before, "confirm: turn still moves the cursor when not armed");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
