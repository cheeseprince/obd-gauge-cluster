#pragma once
#include "app_types.h"
#include "nav_model.h"
#include "history.h"
#include "settings.h"        // Settings
#include "rtc.h"             // DateTime

enum class Theme { Day, Night };

namespace ui {
void begin(const Settings& s);                           // build static objects once (splash needs the detected identity)
// Renders the active screen. In Focus view the 5-min trend graph is drawn from `hist`.
void render(const GaugeSet& gs, const NavState& nav, Theme theme, const HistorySet& hist, bool metric);
bool anyAlarm(const GaugeSet& gs);                       // true if any thresholded stat is Amber or Red
// Full-screen status overlay (OBD connecting / scanning / OTA). nullptr = hide,
// show gauges. Theme-aware: night renders amber.
void showStatus(const char* msg, Theme t = Theme::Day);
void showMenu(const MenuState& m, const Settings& s, Theme t, const DateTime& now);   // settings overlay (clock in header)
void hideMenu();
void showTimeSet(const DateTime& dt, uint8_t field, Theme t);   // date/time editor overlay
void hideTimeSet();
void showVehiclePick(uint8_t sel, bool autoOn, Theme t);   // vehicle-profile picker overlay (Pick Vehicle); row 0 = Auto-detect
void hideVehiclePick();
// Boot identity splash. rtcValid gates the clock line: valid -> date/time,
// invalid on an RTC board -> "--:--" (hints at a dead coin cell), invalid on a
// no-RTC board (knob) -> no clock line at all.
void showSplash(const DateTime& now, bool rtcValid);
void hideSplash();
// Press-to-dismiss + startup grace for the full-screen alarm overlay.
void suppressAlarms(bool suppress);   // true during startup grace: hide the overlay
void ackAlarm();                       // dismiss the currently-shown alarm overlay
bool alarmShown();                     // true if the overlay is currently up
}  // namespace ui
