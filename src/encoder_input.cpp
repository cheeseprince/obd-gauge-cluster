// Nav-input HAL for the CrowPanel Advance — Modulino I2C rotary encoder.
// Implements the buttonInput:: interface declared in button_input.h. The
// namespace name is historical — it was a PCF8574 button expander on the retired
// Elecrow board; on this board it is the rotary encoder. This is the only
// implementation that ships.
//
// begin() discovers the knob over I2C and reports it; update() reads it and
// drives the nav cursor. See update() for the three things that are not
// obvious: the ~66 Hz I2C throttle, the 30 ms button debounce (the raw line
// bounces — bring-up log), and the deferred rtcWrite, which is kept OUTSIDE
// the nav spinlock because it is another I2C transaction.
#include <Arduino.h>
#include <Wire.h>
#include <Modulino.h>
#include "button_input.h"     // the buttonInput:: interface this file implements
#include "board_caps.h"       // HAS_RTC / HAS_SD_SLOT for menu-row visibility
#include "encoder_logic.h"
#include "nav_model.h"
#include "settings_menu.h"
#include "rtc.h"               // rtcRead / rtcWrite / adjustField / DateTime
#include "ui.h"               // ui::alarmShown / ui::ackAlarm (press-to-dismiss)
#include "vehicle_registry.h"  // profileCount (Pick Vehicle cursor range)

static ModulinoKnob knob;
static EncoderLogic logic;
static bool g_present = false;
static volatile MenuAction g_menuAction = MenuAction::None;  // set on core 0, consumed by loop() (core 1)
// Vehicle-pick commit queue: this file has no access to main.cpp's `settings`
// global (same reason g_menuAction exists) — core 0 only queues *which* index
// was picked; loop() (core 1, where `settings`/saveSettings live) performs the
// actual settings.vehicleKey write + saveSettings + ESP.restart(). Mirrors the
// g_menuAction/consumeMenuAction pattern exactly (see e.g. ToggleNight/ForgetAdapter).
static volatile bool    g_vehCommitReq = false;
static volatile uint8_t g_vehCommitSel = 0;
// Vehicle-pick cursor seed: pushed from main.cpp (core 1, where `settings`
// lives) every loop so the picker always opens on the CURRENT state — row 0
// (Auto-detect) when settings.vehicleAuto, else the locked profile's row —
// instead of always resetting to Auto-detect. See setVehiclePickSeed().
static volatile uint8_t g_vehPickSeed = 0;
// Fuel-tank commit queue + cursor seed — same core-0/core-1 split as the
// vehicle picker above: core 0 queues the chosen capacity, loop() writes it to
// `settings` and persists. The seed is the CURRENT effective capacity, so the
// picker opens on the row that is already correct (one click to confirm).
static volatile bool  g_tankCommitReq = false;
static volatile float g_tankCommitGal = 0.0f;
static volatile float g_tankPickSeed  = 0.0f;
static portMUX_TYPE navMux = portMUX_INITIALIZER_UNLOCKED;

namespace buttonInput {

void begin() {
  Wire.setPins(15, 16);          // I2C-OUT port; Modulino calls Wire.begin() internally
  Modulino.begin(Wire);
  g_present = knob.begin();      // auto-discovers the knob (0x3A / 0x74)
  menuSetCaps(HAS_RTC, HAS_SD_SLOT, HAS_OTA);   // dash: all true — full menu
  Serial.printf("[encoder] %s\n", g_present ? "found" : "MISSING");
}

// Read the encoder and drive nav: each detent moves the cursor (CW = next),
// a short press enters Focus / backs out. Runs on the core-0 I/O task.
void update(NavState& s) {
  if (!g_present) return;
  const uint32_t now = millis();
  static uint32_t lastRead = 0;
  if (now - lastRead < 15) return;          // throttle I2C reads to ~66 Hz
  lastRead = now;

  const int16_t pos = knob.get();           // refreshes the pressed flag
  const bool rawPressed = knob.isPressed();

  // Debounce the button (the raw line bounces — see bring-up log).
  static bool dbState = false, lastRaw = false;
  static uint32_t lastChange = 0;
  if (rawPressed != lastRaw) { lastRaw = rawPressed; lastChange = now; }
  if (now - lastChange >= 30) dbState = rawPressed;

  const int32_t d = logic.rotation(pos);    // signed detents since last read
  const EncEvent ev = logic.button(dbState, now);

  // Refresh the cached clock ~1 Hz (I2C, OUTSIDE the nav critical section).
  static uint32_t lastRtc = 0;
  DateTime dtNow; bool haveRtc = false;
  if (now - lastRtc >= 1000) { lastRtc = now; haveRtc = rtcRead(dtNow); }

  bool saveReq = false; DateTime toSave{};   // rtcWrite deferred until after unlock (I2C)

  portENTER_CRITICAL(&navMux);
  if (haveRtc) { s.rtcNow = dtNow; s.rtcValid = true; }   // sticky: a later glitch keeps the cached clock
  if (s.view == View::Menu) {
    for (int32_t i = 0; i < d; i++) menuMove(s.menu, +1);   // CW = down
    for (int32_t i = 0; i > d; i--) menuMove(s.menu, -1);   // CCW = up
    if (ev == EncEvent::PressShort) {
      MenuAction a = menuActivate(s.menu);
      if (a == MenuAction::CloseMenu)        s.view = View::Quad;
      else if (a == MenuAction::OpenTimeSet) { s.editDt = s.rtcNow; s.editField = 0; s.view = View::TimeSet; }
      else if (a == MenuAction::OpenVehiclePick) { s.vehSel = g_vehPickSeed; s.view = View::VehiclePick; }  // seed to Auto-detect row or the current lock (pushed by main.cpp; no `settings` access here — see g_vehCommit*/g_vehPickSeed above)
      else if (a == MenuAction::OpenTankPick) { tankPickSeed(s.tank, g_tankPickSeed); s.view = View::TankPick; }
      // ForgetAdapter/ResetTrip must ALSO close the menu, same as CloseMenu:
      // their feedback lives OUTSIDE the menu overlay (the status overlay for
      // Forget; the gauges for Reset trip), and statusLabel/menuLabel are both
      // full-screen LV_OPA_COVER children of lv_layer_top() with the menu built
      // after (and therefore drawn on top of) the status overlay — see ui.cpp
      // buildStatus()/buildMenu(). Leaving the menu open paints the ack
      // underneath it, invisibly.
      else if (a == MenuAction::ForgetAdapter || a == MenuAction::ResetTrip) {
        g_menuAction = a;        // applied by loop()
        s.view = View::Quad;     // close the menu so the feedback is visible
      }
      else if (a != MenuAction::None)        g_menuAction = a;   // applied by loop()
    }
    if (ev == EncEvent::PressLong) s.view = View::Quad;        // hold closes
  } else if (s.view == View::TimeSet) {
    for (int32_t i = 0; i < d; i++) adjustField(s.editDt, s.editField, +1);
    for (int32_t i = 0; i > d; i--) adjustField(s.editDt, s.editField, -1);
    if (ev == EncEvent::PressShort) s.editField = (uint8_t)((s.editField + 1) % 5);  // next field, Min->Y
    if (ev == EncEvent::PressLong)  { toSave = s.editDt; saveReq = true; s.view = View::Menu; }  // save + back
  } else if (s.view == View::VehiclePick) {
    int n = profileCount() + 1;                       // +1 for the Auto-detect row
    for (int32_t i = 0; i < d; i++) s.vehSel = (uint8_t)((s.vehSel + 1) % n);
    for (int32_t i = 0; i > d; i--) s.vehSel = (uint8_t)((s.vehSel + n - 1) % n);
    if (ev == EncEvent::PressLong)  s.view = View::Menu;                 // cancel, no change
    if (ev == EncEvent::PressShort) {                                    // select + reboot
      g_vehCommitSel = s.vehSel;
      g_vehCommitReq = true;
      s.view = View::Menu;
    }
  } else if (s.view == View::TankPick) {
    for (int32_t i = 0; i < d; i++) tankPickMove(s.tank, +1);
    for (int32_t i = 0; i > d; i--) tankPickMove(s.tank, -1);
    // Long press always backs out, including out of Custom's edit mode, so
    // there is a way to abandon a half-typed value without committing it.
    if (ev == EncEvent::PressLong)  { s.tank.editing = false; s.view = View::Menu; }
    if (ev == EncEvent::PressShort) {
      float gal = 0.0f;
      // Custom's first press only opens the value editor — the picker stays up
      // until tankPickActivate reports a real commit.
      if (tankPickActivate(s.tank, gal)) {
        g_tankCommitGal = gal;
        g_tankCommitReq = true;
        s.view = View::Menu;
      }
    }
  } else {
    for (int32_t i = 0; i < d; i++) nav::cursorNext(s);   // CW / right = next stat
    for (int32_t i = 0; i > d; i--) nav::cursorPrev(s);   // CCW / left = prev stat
    if (ev == EncEvent::PressShort) {
      if (ui::alarmShown()) ui::ackAlarm();               // dismiss the alarm overlay
      else                  nav::press(s);                // else normal Focus / back nav
    }
    if (ev == EncEvent::PressLong) { menuReset(s.menu); s.view = View::Menu; }  // open menu
  }
  portEXIT_CRITICAL(&navMux);

  if (saveReq) rtcWrite(toSave);   // I2C write outside the critical section (core 0)
}

// One-shot: returns the latest menu action queued by the core-0 task, then clears.
MenuAction consumeMenuAction() {
  portENTER_CRITICAL(&navMux);
  MenuAction a = g_menuAction;
  g_menuAction = MenuAction::None;
  portEXIT_CRITICAL(&navMux);
  return a;
}

// One-shot: true (with the chosen registry index in `sel`) if the vehicle
// picker was just committed on core 0; clears the request. loop() (core 1)
// uses this to write settings.vehicleKey + saveSettings + ESP.restart().
bool consumeVehicleCommit(uint8_t& sel) {
  portENTER_CRITICAL(&navMux);
  bool req = g_vehCommitReq;
  sel = g_vehCommitSel;
  g_vehCommitReq = false;
  portEXIT_CRITICAL(&navMux);
  return req;
}

// One-shot: true (with the chosen capacity in `gal`, 0 = clear the override) if
// the fuel-tank picker was just committed on core 0. loop() (core 1) performs
// the setTankOverride + saveSettings. NO reboot, unlike the vehicle picker:
// the capacity feeds a derived readout that is recomputed every poll, so it
// takes effect on the next tick.
bool consumeTankCommit(float& gal) {
  portENTER_CRITICAL(&navMux);
  bool req = g_tankCommitReq;
  gal = g_tankCommitGal;
  g_tankCommitReq = false;
  portEXIT_CRITICAL(&navMux);
  return req;
}

// Push from main.cpp (core 1): the effective capacity the tank picker should
// seed its cursor to. Same no-locking rationale as setVehiclePickSeed below.
void setTankPickSeed(float gal) { g_tankPickSeed = gal; }

// One-shot-per-loop push from main.cpp (core 1): the row the Pick Vehicle
// cursor should seed to next time it opens (0 = Auto-detect, i>0 = registry
// profile i-1, matching s.vehSel's row numbering). No locking needed — a
// single volatile byte written by one core and read by the other; a torn
// read/write race just costs one stale-seed tick, never a wrong commit.
void setVehiclePickSeed(uint8_t s) { g_vehPickSeed = s; }

bool present() { return g_present; }

void lockNav()   { portENTER_CRITICAL(&navMux); }
void unlockNav() { portEXIT_CRITICAL(&navMux); }

}  // namespace buttonInput
