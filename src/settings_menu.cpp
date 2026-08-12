#include "settings_menu.h"

static const int N = (int)MenuItem::COUNT;

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
  if (m.armed != MenuItem::COUNT) { if (step) m.confirmYes = !m.confirmYes; return; }
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
