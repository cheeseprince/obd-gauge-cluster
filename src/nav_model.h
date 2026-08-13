#pragma once
#include "app_types.h"
#include "settings_menu.h"   // MenuState
#include "rtc.h"             // DateTime

enum class View { Quad, Focus, Menu, TimeSet, VehiclePick, TankPick };

struct NavState {
  View view = View::Quad;
  int quadPage = 0;            // 0 or 1
  StatId focus = StatId::Trans;
  MenuState menu;              // settings-menu cursor (View::Menu)
  DateTime editDt{2026,1,1,0,0,0};   // time-set editor working value (View::TimeSet)
  uint8_t  editField = 0;            // 0=y 1=mon 2=d 3=h 4=min
  uint8_t  vehSel = 0;                // cursor in the vehicle-pick list (View::VehiclePick)
  TankPickState tank;                 // fuel-tank capacity picker (View::TankPick)
  DateTime rtcNow{2026,1,1,0,0,0};   // cached clock for display (core 0 -> core 1)
  bool rtcValid = false;             // true once rtcRead() has EVER succeeded — rtcNow
                                     // otherwise still holds its plausible default (knob
                                     // absent / I2C fail / RTC battery dead), which must
                                     // not be trusted for SD log filenames
};

namespace nav {
StatId statForCell(int quadPage, int cellIndex);  // 0..3 reading order
int quadPageForStat(StatId s);
void swipeLeft(NavState& s);   // quad: next page; focus: next stat (wraps)
void swipeRight(NavState& s);  // quad: prev page; focus: prev stat (wraps)
void tapCell(NavState& s, int cellIndex);  // quad only -> focus that cell's stat
void tapBack(NavState& s);     // focus -> quad page holding the focused stat
void cursorNext(NavState& s);  // encoder: step focus to next displayed stat (wraps), auto-page
void cursorPrev(NavState& s);  // encoder: step focus to prev displayed stat (wraps), auto-page
void press(NavState& s);       // encoder: Quad->Focus(focused stat); Focus->Quad(page of focus)
}  // namespace nav
