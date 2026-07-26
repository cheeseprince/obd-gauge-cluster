#include <cstdio>
#include "nav_model.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  check(nav::statForCell(0,3)==StatId::Egt,     "p0c3 egt (TOW)");
  check(nav::statForCell(2,2)==StatId::Nox,     "p2c2 nox (REGEN)");
  check(nav::statForCell(6,1)==StatId::Volts,   "p6c1 volts (MISC)");
  check(nav::quadPageForStat(StatId::L100km)==4,"l100km on page4 (TRIP)");

  // 7-page swipe wrap
  NavState s;
  for (int p = 1; p <= 6; p++) { nav::swipeLeft(s); check(s.quadPage==p, "swipe up"); }
  nav::swipeLeft(s); check(s.quadPage==0, "wrap to p0");

  // Focus swipe = reading-order. Trans -> Coolant (p0c1).
  s = NavState{}; nav::tapCell(s, 0);
  nav::swipeLeft(s);
  check(s.focus==StatId::Coolant && s.quadPage==0, "focus swipe -> Coolant (reading order)");

  // reading-order through a filled page then auto-page: Egt(p0c3) -> Boost(p1c0)
  s.focus = StatId::Egt; s.quadPage = 0;
  nav::swipeLeft(s);
  check(s.focus==StatId::Boost && s.quadPage==1, "Egt->Boost (auto-page)");

  // reading order within p6: Volts(p6c1) -> Oil temp(p6c2)
  s.focus = StatId::Volts; s.quadPage = 6;
  nav::swipeLeft(s);
  check(s.focus==StatId::Oil && s.quadPage==6, "Volts->Oil temp");

  // wraps from the last tile: Oil temp(p6c2) -> Trans(p0c0)
  s.focus = StatId::Oil; s.quadPage = 6;
  nav::swipeLeft(s);
  check(s.focus==StatId::Trans && s.quadPage==0, "Oil->Trans (skips empty, wraps)");

  // --- Encoder cursor: reading order, never the hidden helpers (RefTq/Baro/ActTq) ---
  {
    NavState s;
    nav::cursorNext(s);
    check(s.focus == StatId::Coolant && s.quadPage == 0, "cursorNext Trans->Coolant");
    // wrap across last displayed (Oil temp, p6c2)
    s.focus = StatId::Oil; s.quadPage = 6;
    nav::cursorNext(s);
    check(s.focus == StatId::Trans && s.quadPage == 0, "cursorNext wraps Oil->Trans");
    s.focus = StatId::Trans; s.quadPage = 0;
    nav::cursorPrev(s);
    check(s.focus == StatId::Oil && s.quadPage == 6, "cursorPrev wraps Trans->Oil");

    // full forward cycle: monotonic pages 0->6, never lands on a hidden helper
    // (RefTq/Baro/ActTq/Gear). 27 displayed.
    s = NavState{};
    int prevPage = 0; bool monotonic = true, hitHidden = false;
    for (int i = 0; i < 26; i++) {
      nav::cursorNext(s);
      if (s.quadPage < prevPage) monotonic = false;
      if (s.focus == StatId::RefTq || s.focus == StatId::Baro ||
          s.focus == StatId::ActTq || s.focus == StatId::Gear)
        hitHidden = true;
      prevPage = s.quadPage;
    }
    check(monotonic && !hitHidden && s.quadPage == 6, "cursor monotonic 0->6, never a helper");
    nav::cursorNext(s);
    check(s.focus == StatId::Trans && s.quadPage == 0, "27th wraps to Trans p0");

    NavState q; nav::press(q);
    check(q.view == View::Focus, "press in Quad enters Focus");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
