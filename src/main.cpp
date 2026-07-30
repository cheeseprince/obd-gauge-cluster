#include <Arduino.h>
#include <cstring>
#include <esp_ota_ops.h>   // esp_ota_mark_app_valid_cancel_rollback()
#include <esp_task_wdt.h>  // esp_task_wdt_reconfigure() — see the TWDT note in setup()
#include <esp_system.h>    // esp_reset_reason()
#include "display.h"
#include "ui.h"
#include "obd_source.h"
#include "mock_obd_source.h"
#include "ble_obd_source.h"
#include "button_input.h"
#include "history.h"
#include "settings.h"
#include "settings_menu.h"
#include "sd_log.h"
#include "wdt_kick.h"
#include "board_caps.h"
#include "ota_portal.h"
#include "ota_update.h"
#include "fw_git.h"
#include "boot_banner.h"
#include "vehicle_active.h"
#include "vehicle_registry.h"
#include "vin.h"
#ifndef FW_DATE
#define FW_DATE __DATE__
#endif
#ifndef OTA_ENV
#define OTA_ENV "n/a"
#endif

// Unknown/empty registry key (fresh device) falls back to the Generic profile.
extern const VehicleProfile GENERIC_PROFILE;

static GaugeSet gauges;
static NavState navState;
#if MOCK_OBD
static MockObdSource g_obd;   // bench: synthetic data (safe-band default)
#else
static BleObdSource g_obd;    // CrowPanel S3: live OBD over BLE (vLinker MS)
#endif
static Settings settings;            // persisted display settings (NVS)
static Theme    theme = Theme::Day;
static HistorySet history;           // rolling 5-min per-stat samples for the Focus graph
static uint32_t g_forgetMsgUntil = 0;// show "Adapter forgotten" until this millis() (0 = off)

#if HAS_KNOB_MENU
namespace buttonInput { MenuAction consumeMenuAction(); bool consumeVehicleCommit(uint8_t& sel); void setVehiclePickSeed(uint8_t s); }
#endif

// Two core-0 tasks, split so a blocking OBD connect can't freeze the encoder.
// inputTask runs at HIGHER priority than obdTask: while obdTask is parked in the
// 6 s BLE scan (blocked on a semaphore) the scheduler runs inputTask freely, and
// the higher-priority inputTask still preempts on its 3 ms tick. So the knob
// (and the "hold for settings" menu) stay
// responsive whether disconnected, scanning, or reconnecting. Both stay on core 0
// to keep BT/ELM work off the LVGL render core (core 1). navState is shared with
// core 1 under navMux; applyReadings()/latest() stay safe as before.
static void inputTaskCore0(void* /*arg*/) {
  for (;;) {
    buttonInput::update(navState);   // encoder / buttons -> navState (under navMux)
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

// Heartbeat stamped each obd loop. The core-1 render loop (always alive) watches it
// and reboots if it freezes — a hang backstop for the blocking BLE connect, since the
// core-0 task WDT is disabled. Written core 0, read core 1.
static volatile uint32_t g_obdHeartbeat = 0;

// True while the OTA portal / update check owns the WiFi + screen (core 1).
// The core-0 OBD task idles (but keeps its heartbeat alive) so nothing else
// touches the radio; both flows end in ESP.restart(), so there is no resume path.
static volatile bool g_suspendObd = false;

// Kick from long synchronous core-0 diagnostics (the 'x' scan / 'g' probe run
// hundreds of probes inside ONE poll(); see wdt_kick.h) so a slow-but-healthy
// sweep against a quiet ECU isn't mistaken for a hang and rebooted mid-scan.
void obdWatchdogKick() { g_obdHeartbeat = millis(); }

static void obdTaskCore0(void* /*arg*/) {
  for (;;) {
    g_obdHeartbeat = millis();
    if (g_suspendObd) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // OTA owns WiFi
    g_obd.poll(millis());   // drives BT connect + ELM queries (may block while connecting)
#if HAS_SD_LOG
    // CSV logging (core 0). Wall-clock from the input task's cached RTC, under navMux.
    DateTime rtc; bool rtcOk;
    buttonInput::lockNav(); rtc = navState.rtcNow; rtcOk = navState.rtcValid; buttonInput::unlockNav();
    ObdReadings r = g_obd.latest();   // by-value snapshot (safe vs core-1 latest() calls)
    logTick(r.linkUp, settings.logging, rtc, rtcOk, millis(), r.v, r.valid);
#endif
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

void setup() {
  Serial.begin(115200);
  display::begin();
  // Select the profile BEFORE g_obd.begin(): the source's begin() builds its
  // query schedule from the active profile, so g_activeProfile must be set first.
  loadSettings(settings);
  g_activeProfile = profileForKey(settings.vehicleKey);
  if (!g_activeProfile) {
    // An unresolvable key (renamed/removed profile, or corrupted NVS) falls back
    // to Generic. Clear the stale key rather than leaving it persisted: it no
    // longer describes the running profile, it makes the Pick-Vehicle cursor seed
    // to a vehicle that isn't active, and leaving vehicleAuto's lock semantics
    // hanging off a dead key is worse than re-detecting on the next connect.
    if (settings.vehicleKey[0]) {
      Serial.printf("[PROFILE] key '%s' did not resolve — falling back to Generic\n",
                    settings.vehicleKey);
      settings.vehicleKey[0] = '\0';
      saveSettings(settings);
    }
    g_activeProfile = &GENERIC_PROFILE;
  }
  g_obd.begin();
#if !HAS_RTC
  // No clock -> Auto can't resolve; pin the mode to the persisted theme so the
  // menu shows ON/OFF (cycleNightMode also never offers Auto on this board).
  if (settings.nightMode == NIGHT_AUTO)
    settings.nightMode = settings.night ? NIGHT_NIGHT : NIGHT_DAY;
#endif
  theme = settings.night ? Theme::Night : Theme::Day;
  display::setBacklight(settings.brightnessPct);
  ui::begin();
  buttonInput::begin();  // bring up the Modulino encoder on I2C; no-op if absent
#if HAS_SD_LOG
  sdBegin();             // init microSD (HSPI) for CSV logging
#endif

#if !MOCK_OBD
  // A real connect() can block core 0 for several seconds; that starves the
  // IDLE0 task so it can't feed the task watchdog → panic/reboot loop. So core 0's
  // idle task must stop being watched.
  //
  // DO NOT use disableCore0WDT() here. It calls esp_task_wdt_delete(IDLE0), which
  // on IDF 5.x unsubscribes the task but leaves the TWDT's idle HOOK installed.
  // The hook then calls esp_task_wdt_reset() for a task that is no longer
  // subscribed, on every idle tick, and IDF logs an error each time:
  //   E (nnnn) task_wdt: esp_task_wdt_reset(705): task not found
  // At idle-tick rates that saturates the 115200 UART and burns enough CPU in the
  // logging path to push OBD replies past their 400 ms window — the link comes up
  // and every gauge stays blank. That is exactly how v0.1.1 failed in the field.
  // On IDF 4.4 (Arduino core 2.x) the same call was quiet, which is why this only
  // appeared after the core 3.1.3 migration.
  //
  // The supported IDF 5 way is to reconfigure the TWDT with an empty
  // idle_core_mask, which removes the hook as well as the subscription.
  {
    esp_task_wdt_config_t twdt = {
      .timeout_ms     = 60000,   // generous: our own 240 s heartbeat watchdog in
                                 // loop() is the real hang backstop (see below)
      .idle_core_mask = 0,       // watch NEITHER idle task -> no hook, no spam
      .trigger_panic  = false,
    };
    esp_err_t rc = esp_task_wdt_reconfigure(&twdt);
    if (rc != ESP_OK) Serial.printf("[WDT] reconfigure failed: %d\n", (int)rc);
  }
#endif

  // Input + OBD as separate core-0 tasks; input at higher priority (2 vs 1) so a
  // blocking connect can't starve the encoder. LVGL render stays on core 1 (loop).
  xTaskCreatePinnedToCore(inputTaskCore0, "input_core0", 3072, nullptr, 2, nullptr, 0);
  // 8192 (was 4096): the VIN read on connect (readVinOverIo -> parseVinReply,
  // std::string/vector parsing with exception-cleanup frames) runs on top of the
  // already-deep BLE-connect path and overflowed a 4096-byte stack, crashing
  // obd_core0 in a boot loop (v1.3.0). Bytes on esp32-arduino.
  xTaskCreatePinnedToCore(obdTaskCore0,   "obd_core0",   8192, nullptr, 1, nullptr, 0);

  // Identity banner — the only thing setup() prints on the success path.
  // Emitted last so `heap` reflects the real post-allocation figure, which is
  // what makes it useful as a static-bloat tripwire.
  //
  // On this board Serial is the native USB CDC (ARDUINO_USB_CDC_ON_BOOT=1), so
  // this lands on /dev/ttyACM*, NOT on the CH340 UART bridge.
  {
    BootInfo bi;
    bi.env         = OTA_ENV;
    bi.version     = FW_VERSION;
    bi.git         = FW_GIT;
    bi.profileKey  = settings.vehicleKey;   // "" when Generic is the fallback
    bi.psramBytes  = ESP.getPsramSize();
    bi.flashBytes  = ESP.getFlashChipSize();
    bi.resetReason = (int)esp_reset_reason();
    bi.freeHeap    = ESP.getFreeHeap();
    char line[192];
    formatBootBanner(bi, line, sizeof line);
    Serial.println(line);
  }
}

// Cycle the night tri-state (Auto -> Day -> Night; no Auto without an RTC) —
// shared by the serial 'n' key and the settings-menu action (suffix labels the
// source in the log line). The theme itself is applied by the per-tick
// resolver in loop(), which is the SINGLE place the active theme changes.
static void applyNightCycle(const char* suffix) {
  cycleNightMode(settings, HAS_RTC && geoValid(settings.geo));
  saveSettings(settings);
  Serial.printf("[THEME] mode=%s%s\n", nightModeLabel(settings), suffix);
}

void loop() {
  uint32_t now = millis();

#if !MOCK_OBD
  // OTA rollback confirm: once the firmware has run stably for 20 s (booted and
  // looping without a crash-reboot), mark the running image valid so the
  // bootloader keeps it. A freshly-OTA'd image that crashes before this is rolled
  // back to the previous partition on the next boot (needs the IDF bootloader
  // rollback config; harmless no-op otherwise, and on a non-pending image).
  static bool otaConfirmed = false;
  if (!otaConfirmed && now > 20000) {
    otaConfirmed = true;
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[OTA] running image marked valid (rollback cancelled)");
  }
#endif

#if !MOCK_OBD
  // Software watchdog: core 1 reboots if the core-0 OBD task freezes (e.g. a wedged
  // NimBLE call that never returns) — the backstop for the disabled core-0 task WDT.
  // Window = 240s. A single connectAndSetup round is NOT bounded by 6s scan + 12x4s:
  // in a busy RF environment it GATT-connects to random devices and attempts bonding
  // (each can take many seconds), so a legitimate round can exceed 90s (bench-observed
  // false restarts at 90s with no adapter present). 240s still catches a true hang —
  // a call that never returns — while clearing the real worst-case connect round.
  if (g_obdHeartbeat != 0 && (now - g_obdHeartbeat) > 240000) {
    Serial.println("[WDT] OBD task stalled >240s — restarting");
    delay(50);
    ESP.restart();
  }
#endif

  // Serial key handler:
  //   'n' → toggle Day/Night theme + backlight
  //   's' → toggle mock safe-bands (no alarms) vs alarm-sweep  [MOCK_OBD only]
  //   'e' 'x' 'd' 'g' → vehicle bring-up probes    [OBD_DEV_CONSOLE builds only]
  //
  // NOTHING HERE MAY BE DESTRUCTIVE OR LONG-RUNNING ON A SHIPPED BUILD (F-11).
  // This console is unauthenticated: anything reachable from it is reachable by
  // anyone who can touch the USB port. Two categories were removed from the
  // stock image rather than gated behind a prompt, because a prompt stops an
  // accident, not an attacker:
  //
  //   * 'f'/'p' (forget adapter / re-pair) are GONE. Wiping the stored bond
  //     reopens the BLE trust-on-first-use window, which is the one thing here
  //     with a real security consequence — a clone present during that window
  //     can bond and then feed fabricated readings. Settings → Forget adapter
  //     already does this from the knob, which needs physical presence at the
  //     dash and shows an on-screen confirmation.
  //   * the probe/scan keys are compiled out unless OBD_DEV_CONSOLE is set.
  //     'x' alone sweeps 544 PIDs and monopolises the OBD link for minutes, so
  //     on a moving vehicle it is a denial of the gauges. They exist for
  //     bringing up a new vehicle, which is a bench activity — build
  //     `crowpanel_obd_dev` for that.
  //
  // During pairing, core 0 (via poll()/doPairing()) owns the console.  Stand
  // down here so we don't race on Serial.read() while doPairing() blocks on
  // user input.
#if !MOCK_OBD
  if (g_obd.pairing()) {
    // doPairing() is running on this core and owns the console — skip.
  } else
#endif
  if (Serial.available()) {
    char k = Serial.read();
    if (k == 'n') {
      applyNightCycle("");
    }
#if MOCK_OBD
    else if (k == 's') {
      g_obd.setSafeMode(!g_obd.safeMode());
      Serial.printf("[MOCK] %s\n", g_obd.safeMode() ? "safe bands (no alarms)" : "alarm sweep");
    }
#else
    // No 'f'/'p' here — see the F-11 note above. Forget adapter lives in the
    // settings menu, where it needs the knob and confirms on screen.
#if defined(BLE_OBD) && defined(OBD_DEV_CONSOLE)
    else if (k == 'e') {
      Serial.println("[OBD] EGT/DPF raw-reply dump requested");
      g_obd.requestDiag();
    }
    else if (k == 'x') {
      Serial.println("[OBD] enhanced-PID range scan requested");
      g_obd.requestScan();
    }
    else if (k == 'd') {
      Serial.println("[OBD] DEF candidate PID dump requested");
      g_obd.requestDefProbe();
    }
    else if (k == 'g') {
      Serial.println("[OBD] gear/oil live probe requested");
      g_obd.requestGearOilProbe();
    }
#endif
#endif
  }

#if HAS_KNOB_MENU
  // Encoder menu actions (core 0 queues; apply here on core 1).
  MenuAction menuAct = buttonInput::consumeMenuAction();
  switch (menuAct) {
    case MenuAction::ToggleNight:
      applyNightCycle(" (knob)");
      break;
    case MenuAction::CycleBrightness:
      settings.brightnessPct = nextBrightness(settings.brightnessPct);
      display::setBacklight(settings.brightnessPct);
      saveSettings(settings);
      Serial.printf("[BRIGHT] %u%%\n", (unsigned)settings.brightnessPct);
      break;
    case MenuAction::ToggleUnits:
      settings.metric = !settings.metric;
      saveSettings(settings);
      Serial.printf("[UNITS] %s\n", settings.metric ? "metric" : "imperial");
      break;
    case MenuAction::ToggleLogging:
      settings.logging = !settings.logging;
      saveSettings(settings);
      Serial.printf("[LOG] %s\n", settings.logging ? "on" : "off");
      break;
    case MenuAction::ResetTrip:
#if !MOCK_OBD
      g_obd.resetTrip();   // all live sources (classic/BLE/WiFi) marshal to core 0
#endif
      Serial.println("[TRIP] reset");
      break;
    case MenuAction::ForgetAdapter:
#if !MOCK_OBD
      g_obd.forget();
#endif
      g_forgetMsgUntil = millis() + 4000;   // on-screen confirmation while it rescans
      Serial.println("[OBD] forget (menu)");
      break;
#if HAS_OTA
    case MenuAction::OpenWifiSetup:
    case MenuAction::CheckUpdate: {
      // Both flows run BLOCKING here on core 1 (they own the screen anyway)
      // with the OBD task idled so nothing else touches the WiFi radio. The
      // pump keeps LVGL served so status text actually paints. Both flows end
      // in a restart — the cleanest way back to a normal radio/OBD state
      // (the knob's WiFi must rejoin the adapter's AP from scratch anyway).
      g_suspendObd = true;
      delay(150);                          // let an in-flight poll() finish
      ui::hideMenu();
      auto pump = [](const char* msg) { ui::showStatus(msg, theme); display::tick(); };
      if (menuAct == MenuAction::OpenWifiSetup) otaPortalRun(pump);
      else                                      otaCheckUpdate(pump, settings.geo);   // reboots itself on success
      Serial.println("[OTA] flow done — restarting");
      delay(100);
      ESP.restart();
      break;                               // unreachable
    }
    case MenuAction::ShowVersion: {
      // Blocking ~4s info card (freed the menu footer line for row space).
      char v[128];
      snprintf(v, sizeof v, "VERSION\n\nbuild %s\n%s\nenv %s\nOTA: obd-gauge-cluster",
               FW_DATE, FW_VERSION, OTA_ENV);
      ui::hideMenu();
      uint32_t t0 = millis();
      while ((int32_t)(millis() - (t0 + 4000)) < 0) {
        ui::showStatus(v, theme);
        display::tick();
        delay(30);
      }
      ui::showStatus(nullptr, theme);
      break;
    }
#endif
    default: break;
  }

  // Keep the Pick Vehicle cursor seed current EVERY loop (not just when the
  // menu is opened) — encoder_input.cpp's OpenVehiclePick handler runs on
  // core 0 the instant the user short-presses "Pick Vehicle", so the seed
  // must already be fresh by then. Auto mode -> row 0 (Auto-detect); locked
  // mode -> the locked profile's row (registry index + 1, since row 0 is the
  // synthetic Auto-detect entry); unknown/stale key falls back to row 0.
  {
    int lockIdx = profileIndexForKey(settings.vehicleKey);
    buttonInput::setVehiclePickSeed(settings.vehicleAuto ? 0
                                    : (uint8_t)(lockIdx < 0 ? 0 : lockIdx + 1));
  }

  // Vehicle-profile pick: core 0 (encoder_input.cpp) only queues the chosen
  // picker index — it has no access to this file's `settings` global. The
  // actual persist + reboot happen here, same as the settings-touching cases
  // in the switch above. Index 0 = Auto-detect row; index i>0 = registry
  // profile i-1 (the picker's row 0 is synthetic, not in PROFILE_REGISTRY).
  uint8_t vehSel;
  if (buttonInput::consumeVehicleCommit(vehSel)) {
    if (vehSel == 0) {                                    // Auto-detect
      settings.vehicleAuto = true;
      settings.vehicleKey[0] = '\0';                       // clear lock; re-detect on next connect
      Serial.println("[VEHICLE] auto-detect enabled — restarting");
    } else {                                              // lock a specific profile
      const char* key = profileKeyAt(vehSel - 1);
      settings.vehicleAuto = false;
      strncpy(settings.vehicleKey, key, sizeof settings.vehicleKey - 1);
      settings.vehicleKey[sizeof settings.vehicleKey - 1] = '\0';
      Serial.printf("[VEHICLE] locked to %s (%s) — restarting\n", key, profileLabelAt(vehSel - 1));
    }
    saveSettings(settings);
    delay(100);
    ESP.restart();
  }
#endif

  display::tick();                         // LVGL service + flush + cache touch (core 1)

  // NavState is shared with the core-0 button task. Take a consistent snapshot
  // under the lock; render from the snapshot. (Navigation is buttons-only.)
  NavState snap;
  buttonInput::lockNav();
  snap = navState;
  buttonInput::unlockNav();

  // Resolve the active night theme each tick (tri-state: Auto follows the RTC
  // solar calc; Day/Night are pins; Auto with an unset RTC holds steady). This
  // is the ONLY place the theme changes — manual cycles above just set the mode.
  {
    bool curNight  = (theme == Theme::Night);
    bool wantNight = resolveNight(settings, snap.rtcValid, snap.rtcNow, curNight);
    if (wantNight != curNight) {
      theme = wantNight ? Theme::Night : Theme::Day;
      settings.night = wantNight;                       // persisted -> boot theme
      settings.brightnessPct = wantNight ? 25 : 100;    // convenience preset; Brightness overrides
      display::setBacklight(settings.brightnessPct);
      saveSettings(settings);                           // Auto writes at most ~2x/day
      Serial.printf("[THEME] %s (resolved, mode=%s)\n",
                    wantNight ? "night" : "day", nightModeLabel(settings));
    }
  }

  // Heavy work (sampling + full re-render with SPI flush) throttled to ~12 Hz;
  // also render immediately on a nav change so input response feels instant.
  static uint32_t lastRender = 0;
  static View     lastView   = snap.view;
  static int      lastPage   = snap.quadPage;
  static StatId   lastFocus  = snap.focus;
  static uint8_t  lastMenuSel = snap.menu.sel;
  static uint8_t  lastEditField = snap.editField;
  static uint8_t  lastVehSel = snap.vehSel;
  bool navChanged = snap.view != lastView || snap.quadPage != lastPage ||
                    snap.focus != lastFocus || snap.menu.sel != lastMenuSel ||
                    snap.editField != lastEditField || snap.vehSel != lastVehSel;
  static const uint32_t SPLASH_MS = 6000;   // boot splash duration, then hand off to connect/gauges
  if (now < SPLASH_MS) {
    ui::showSplash(snap.rtcNow, snap.rtcValid);
  } else if (now - lastRender >= 80 || navChanged) {
    ui::hideSplash();
    lastRender = now;
    lastView = snap.view; lastPage = snap.quadPage; lastFocus = snap.focus;
    lastMenuSel = snap.menu.sel;
    lastEditField = snap.editField;
    lastVehSel = snap.vehSel;
    // poll() runs on core-0 obdTaskCore0 — only applyReadings + render here.
    // latest() is safe from core 1: it returns a by-value snapshot taken under
    // the source's spinlock (no shared static — see ObdSource::latest()).
    // ONE snapshot per frame: applyReadings and the link-state branch below must
    // agree — two latest() calls could straddle a link transition (one frame of
    // gauges rendered from the blank link-down path), and each call is a spinlock
    // hold + ~380 B copy against the core-0 poll task.
    ObdReadings readings = g_obd.latest();
    applyReadings(gauges, readings, now);
    historyTick(history, gauges, now);    // record 5-min trend (internally 1 Hz)

    // VIN auto-detect: map the connected vehicle's WMI to a registry profile and
    // switch to it automatically (save + reboot), unless locked by a manual Pick
    // Vehicle (settings.vehicleAuto == false). One-shot by construction: after the
    // restart, the saved key resolves on boot and the same VIN maps to the same
    // key, so vinAutoTarget() returns nullptr on the next check — no reboot loop.
    // Reuses `readings` (the one ObdReadings snapshot this frame already took)
    // rather than calling g_obd.latest() again.
    // Never reboot out from under an open overlay. VehiclePick is the case that
    // matters: the user is mid-decision about which profile to lock, and
    // vehicleAuto is still true until they commit, so an arriving VIN could save
    // + restart while their cursor is on the list. Menu/TimeSet are included for
    // the same reason (a restart mid-edit discards the edit). Deferring is free —
    // the VIN persists in `readings`, so this fires on a later frame once they
    // are back on the gauges.
    const bool overlayOpen = snap.view == View::VehiclePick ||
                             snap.view == View::Menu ||
                             snap.view == View::TimeSet;
    if (settings.vehicleAuto && readings.vin[0] && !overlayOpen) {
      const char* target = vinAutoTarget(readings.vin, settings.vehicleAuto, settings.vehicleKey);
      if (target) {
        strncpy(settings.vehicleKey, target, sizeof settings.vehicleKey - 1);
        settings.vehicleKey[sizeof settings.vehicleKey - 1] = '\0';
        saveSettings(settings);
        Serial.printf("[VIN] auto-detected %s — restarting\n", target);
        delay(100);
        ESP.restart();
      }
    }

#if HAS_KNOB_MENU
    // Settings / time-set overlays (top layer; independent of link state).
    if (snap.view == View::Menu) {
      ui::showMenu(snap.menu, settings, theme, snap.rtcNow);
      ui::hideTimeSet();
      ui::hideVehiclePick();
    } else if (snap.view == View::TimeSet) {
      ui::showTimeSet(snap.editDt, snap.editField, theme);
      ui::hideMenu();
      ui::hideVehiclePick();
    } else if (snap.view == View::VehiclePick) {
      ui::showVehiclePick(snap.vehSel, settings.vehicleAuto, theme);
      ui::hideMenu();
      ui::hideTimeSet();
    } else {
      ui::hideMenu();
      ui::hideTimeSet();
      ui::hideVehiclePick();
    }
#endif

    // Choose screen by OBD link state.
    // Mock build: always show gauges (MockObdSource linkUp is always true, but
    //   the #if keeps the logic simple and avoids the real-only pairing() call).
    // Real build: show gauges once linked; status screen while connecting/pairing.
#if MOCK_OBD
    ui::suppressAlarms(false);
    ui::showStatus(nullptr);
    ui::render(gauges, snap, theme, history, settings.metric);
#else
    bool linked = readings.linkUp;   // same snapshot applyReadings consumed
    if (linked) {
      // Startup grace: suppress alarms for 10s after the link FIRST comes up, so
      // the key-on/crank voltage dip can't trip a warning. First link-up only;
      // later reconnects rely on the normal 4s hold-off.
      static bool     linkSeen    = false;
      static uint32_t linkUpSince = 0;
      if (!linkSeen) { linkSeen = true; linkUpSince = now; }
      ui::suppressAlarms(now - linkUpSince < 10000);
      ui::showStatus(nullptr);
      ui::render(gauges, snap, theme, history, settings.metric);
    } else if (g_forgetMsgUntil && (int32_t)(g_forgetMsgUntil - now) > 0) {
      // Just did Forget-adapter — a clear on-screen confirmation while it rescans,
      // so the action is obviously acknowledged (not just a silently-closed menu).
      ui::showStatus("Adapter forgotten.\nScanning for a new one...", theme);
    } else {
#if defined(BLE_OBD)
      char status[256];
      formatConnStatus(g_obd.connStatus(), now, status, sizeof status);
      { extern volatile char g_bleStep[48]; extern char g_bleScan[240];  // BRING-UP: step + devices seen
        size_t l = strlen(status);
        snprintf(status + l, sizeof status - l, "\n[%s]\n%s", (const char*)g_bleStep, g_bleScan); }
      ui::showStatus(status, theme);
#else
      ui::showStatus(g_obd.pairing() ? "Scanning... see console"
                                     : "Connecting to OBD...", theme);
#endif
    }
#endif
  }
  delay(5);
}
