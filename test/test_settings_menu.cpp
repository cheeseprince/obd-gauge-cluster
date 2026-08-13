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
  check((int)MenuItem::COUNT == 13, "13 menu items");   // +1: Fuel tank
  menuMove(m, +1);
  check(m.sel == 0, "wrap down to NightMode");

  // Non-destructive items act immediately.
  m.sel = (uint8_t)MenuItem::NightMode;
  check(menuActivate(m) == MenuAction::ToggleNight, "NightMode -> ToggleNight");
  m.sel = (uint8_t)MenuItem::Brightness;
  check(menuActivate(m) == MenuAction::CycleBrightness, "Brightness -> CycleBrightness");
  m.sel = (uint8_t)MenuItem::Units;
  check(menuActivate(m) == MenuAction::ToggleUnits, "Units -> ToggleUnits");
  m.sel = (uint8_t)MenuItem::FuelTank;
  check(menuActivate(m) == MenuAction::OpenTankPick, "FuelTank -> OpenTankPick");
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
  // Absolute mapping, not a toggle: another +1 move must STAY on Yes.
  menuMove(m, +1);
  check(m.confirmYes == true, "another +1 move stays on Yes (absolute, not a toggle)");
  menuMove(m, -1);
  check(m.confirmYes == false, "-1 move selects No");

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
  // Start from FuelTank, the row immediately above the gated pair: it is
  // always visible, so this isolates the hidden-row skip from row ordering.
  t.sel = (uint8_t)MenuItem::FuelTank;
  menuMove(t, +1);                       // skips SetTime AND Logging
  check(t.sel == (uint8_t)MenuItem::ResetTrip, "move down skips hidden rows");
  menuMove(t, -1);                       // and back up
  check(t.sel == (uint8_t)MenuItem::FuelTank, "move up skips hidden rows");
  t.sel = 0;                             // NightMode
  menuMove(t, -1);
  check(t.sel == (uint8_t)MenuItem::Close, "wrap up still lands on visible last row");

  menuSetCaps(true, true, true);         // restore dash shape for any later checks
  MenuState u; u.sel = (uint8_t)MenuItem::Units;
  menuMove(u, +1);
  // Pins the row ORDER: Fuel tank sits with Units (both units-of-measure kin)
  // and well above the destructive rows, not next to Forget adapter.
  check(u.sel == (uint8_t)MenuItem::FuelTank, "full caps: Fuel tank follows Units");
  menuMove(u, +1);
  check(u.sel == (uint8_t)MenuItem::SetTime, "full caps: SetTime reachable again");
  // Fuel tank is never capability-gated: no RTC, SD or OTA hardware involved.
  menuSetCaps(false, false, false);
  check(menuItemVisible(MenuItem::FuelTank), "Fuel tank visible on a minimal board");
  menuSetCaps(true, true, true);

  // --- Yes/No confirm dialog on destructive rows ----------------------
  // Old behaviour was click-to-arm, click-again-to-confirm, and ANY knob turn
  // silently cancelled. Users reported "it says Forget adapter? and then
  // nothing happens". Now: click arms with No selected, turn selects No/Yes
  // ABSOLUTELY (dir > 0 = Yes, dir < 0 = No -- not a toggle, see menuMove()),
  // click acts on the highlighted choice.
  {
    MenuState d; menuReset(d);
    d.sel = (uint8_t)MenuItem::ForgetAdapter;

    // First click arms, defaulting to No -- the safe choice.
    check(menuActivate(d) == MenuAction::None, "confirm: first click does not act");
    check(d.armed == MenuItem::ForgetAdapter, "confirm: first click arms");
    check(d.confirmYes == false, "confirm: defaults to No");

    // A turn selects the CHOICE (absolute, not relative). It must not move the
    // cursor and must not disarm -- disarming on turn is exactly the old bug.
    menuMove(d, +1);
    check(d.confirmYes == true, "confirm: turn selects Yes");
    check(d.sel == (uint8_t)MenuItem::ForgetAdapter, "confirm: turn does not move the cursor");
    check(d.armed == MenuItem::ForgetAdapter, "confirm: turn does not disarm");

    menuMove(d, -1);
    check(d.confirmYes == false, "confirm: turn back selects No");
    menuMove(d, +1);
    check(d.confirmYes == true, "confirm: +1 selects Yes");

    // Click on Yes fires and disarms.
    check(menuActivate(d) == MenuAction::ForgetAdapter, "confirm: Yes fires the action");
    check(d.armed == MenuItem::COUNT, "confirm: firing disarms");
  }
  {
    // Absolute, not a toggle: three detents in the same direction must land on Yes,
    // not parity-flip back to No. The old toggle made a destructive choice depend on
    // how fast the knob was spun.
    MenuState p; menuReset(p); p.sel = (uint8_t)MenuItem::ForgetAdapter;
    menuActivate(p);
    menuMove(p, +1); menuMove(p, +1); menuMove(p, +1);
    check(p.confirmYes == true, "confirm: 3 detents up = Yes, not parity");
    menuMove(p, -1); menuMove(p, -1);
    check(p.confirmYes == false, "confirm: 2 detents down = No, not parity");
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

  // ── Scrolling window (settings menu + tank picker share this) ─────────────
  {
    // THE INVARIANT: whatever the list length and wherever the cursor is, the
    // cursor must land inside the rendered window. If it ever falls outside,
    // the knob moves a selection the user cannot see.
    for (int n = 1; n <= 40; n++) {
      for (int vis = 1; vis <= 20; vis++) {
        for (int c = 0; c < n; c++) {
          int top = menuWindowTop(c, n, vis);
          bool inWindow = (c >= top) && (c < top + vis);
          check(inWindow, "window always contains the cursor");
          check(top >= 0, "window top is never negative");
          // Never scroll past the end: the last window must end at the list end.
          if (n > vis) check(top <= n - vis, "window never runs past the list end");
          else         check(top == 0, "no scrolling when the whole list fits");
        }
      }
    }
    // Concrete cases for the two real callers.
    check(menuWindowTop(0, 13, 10) == 0,  "menu: cursor at top -> window at top");
    check(menuWindowTop(12, 13, 10) == 3, "menu: cursor at end -> window pinned to end");
    check(menuWindowTop(6, 13, 10) == 1,  "menu: mid cursor centres the window");
    check(menuWindowTop(13, 14, 8) == 6,  "tank: last row pins the window to the end");
    // Out-of-range cursors are clamped, not used to index anything.
    check(menuWindowTop(-5, 13, 10) == 0,  "negative cursor clamps to the top");
    check(menuWindowTop(99, 13, 10) == 3,  "over-range cursor clamps to the end");
  }

  // ── Fuel-tank picker ──────────────────────────────────────────────────────
  {
    // Presets are ascending and unique — a duplicate or an out-of-order entry
    // would make tankPickSeed's match ambiguous and the list confusing.
    for (int i = 1; i < TANK_PRESET_COUNT; i++)
      check(TANK_PRESETS[i] > TANK_PRESETS[i-1], "tank presets strictly ascending");
    check(tankRowCount() == TANK_PRESET_COUNT + 2, "rows = presets + Custom + Unset");
  }
  {
    // SEEDING is what makes the common case one click: the picker opens on the
    // row that is already correct.
    TankPickState t;
    tankPickSeed(t, 24.0f);
    check(t.sel == 0 && !t.editing, "seed: 24 gal lands on the first preset");
    float out = -1.0f;
    check(tankPickActivate(t, out) && out == 24.0f, "seed+click commits 24 gal unchanged");

    tankPickSeed(t, 48.0f);
    check(TANK_PRESETS[t.sel] == 48.0f, "seed: 48 gal lands on the 48 preset");

    // An unknown capacity opens on Unset, so nothing is pre-committed.
    tankPickSeed(t, 0.0f);
    check((int)t.sel == tankRowUnset(), "seed: unset capacity opens on Unset");
    check(tankPickActivate(t, out) && out == 0.0f, "Unset commits 0 (clear the override)");

    // A value the list does not carry opens on Custom, seeded to that value —
    // so an aftermarket tank is not silently snapped to a nearby preset.
    tankPickSeed(t, 37.5f);
    check((int)t.sel == tankRowCustom(), "seed: off-list capacity opens on Custom");
    check(t.customGal == 37.5f, "seed: Custom carries the current value");
  }
  {
    // Row cursor wraps; Custom's first click only ENTERS edit mode.
    TankPickState t; tankPickSeed(t, 0.0f);
    t.sel = 0;
    tankPickMove(t, -1);
    check((int)t.sel == tankRowCount() - 1, "picker wraps backwards to the last row");
    tankPickMove(t, +1);
    check(t.sel == 0, "picker wraps forwards to the first row");

    t.sel = (uint8_t)tankRowCustom();
    t.customGal = 30.0f;
    float out = -1.0f;
    check(!tankPickActivate(t, out), "Custom: first click does not commit");
    check(t.editing, "Custom: first click enters edit mode");
    // While editing, the knob adjusts the VALUE and must not move the cursor.
    uint8_t rowBefore = t.sel;
    tankPickMove(t, +1); tankPickMove(t, +1);
    check(t.customGal == 31.0f, "editing: two detents = +1.0 gal");
    check(t.sel == rowBefore, "editing: the knob does not move the row cursor");
    tankPickMove(t, -1);
    check(t.customGal == 30.5f, "editing: reverse detent steps back down");
    check(tankPickActivate(t, out) && out == 30.5f, "Custom: second click commits the value");
    check(!t.editing, "Custom: committing leaves edit mode");
  }
  {
    // Custom clamps rather than wrapping — a spun knob must not land on a
    // negative or absurd capacity.
    TankPickState t;
    t.sel = (uint8_t)tankRowCustom(); t.editing = true; t.customGal = 1.0f;
    for (int i = 0; i < 20; i++) tankPickMove(t, -1);
    check(t.customGal == 1.0f, "Custom clamps at the low end");
    t.customGal = 199.5f;
    for (int i = 0; i < 20; i++) tankPickMove(t, +1);
    check(t.customGal == 200.0f, "Custom clamps at the high end");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
