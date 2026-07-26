#include <cstdio>
#include "nav_model.h"
#include "button_nav.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

static int failures = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL: %s\n", m); failures++; } }

// Helper: feed the same mask for N ticks.
static void feed(ButtonNav& bn, NavState& s, uint8_t mask, int ticks) {
  for (int i = 0; i < ticks; i++) bn.update(mask, s);
}

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  // -----------------------------------------------------------------------
  // 1. Responsiveness: a single-tick press registers (DEBOUNCE_TICKS=1).
  //    A quick tap caught by even one poll must fire — this is the fix for
  //    quick taps being dropped by the old 2-tick requirement.
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    feed(bn, s, 1u << BTN_RIGHT, 1);  // B5 (right) seen for exactly 1 poll
    feed(bn, s, 0, 1);                // released
    check(s.quadPage == 1, "responsive: 1-tick press fires (page->1)");
  }

  // -----------------------------------------------------------------------
  // 2. No repeat while held: a sustained press fires exactly ONCE.
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    feed(bn, s, 1u << BTN_RIGHT, 5);   // held 5 ticks
    check(s.quadPage == 1, "held: triggers action once (page->1)");
    feed(bn, s, 1u << BTN_RIGHT, 10);  // still held
    check(s.quadPage == 1, "held: no repeat while held");
  }

  // -----------------------------------------------------------------------
  // 3. Quad page0: B0 (TL) -> Focus, focus == Trans
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    feed(bn, s, 1u << BTN_TL, 2);   // press B0 two stable ticks
    feed(bn, s, 0, 1);               // release
    check(s.view  == View::Focus,   "quad->B0: enter Focus");
    check(s.focus == StatId::Trans, "quad->B0: focus==Trans");
  }

  // -----------------------------------------------------------------------
  // 4. From Focus on Trans: press B0 again -> back to Quad
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    // Enter Focus on Trans via B0.
    feed(bn, s, 1u << BTN_TL, 2); feed(bn, s, 0, 1);
    check(s.view  == View::Focus,   "setup: in Focus");
    // Press B0 again (release first so counts reset).
    feed(bn, s, 1u << BTN_TL, 2); feed(bn, s, 0, 1);
    check(s.view     == View::Quad, "focus->B0 (same): back to Quad");
    check(s.quadPage == 0,          "focus->B0 (same): quadPage restored to 0");
  }

  // -----------------------------------------------------------------------
  // 5. From Quad page0: B5 (right) -> quadPage==1
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    feed(bn, s, 1u << BTN_RIGHT, 2); feed(bn, s, 0, 1);
    check(s.view     == View::Quad, "right from quad: stay Quad");
    check(s.quadPage == 1,          "right from quad: quadPage->1");
  }

  // -----------------------------------------------------------------------
  // 6. From Quad page1: B4 (left) -> quadPage==0
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    // Get to page 1 first.
    feed(bn, s, 1u << BTN_RIGHT, 2); feed(bn, s, 0, 1);
    check(s.quadPage == 1, "setup: page->1");
    feed(bn, s, 1u << BTN_LEFT, 2); feed(bn, s, 0, 1);
    check(s.quadPage == 0, "left from quad page1: quadPage->0");
  }

  // -----------------------------------------------------------------------
  // 7. On page1: B1 (TR, cell1) -> Focus, focus == Hp (statForCell(1,1) in the
  //    situation layout; page1 = POWER {Boost, Hp, Rpm, Load})
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    // Navigate to page 1.
    feed(bn, s, 1u << BTN_RIGHT, 2); feed(bn, s, 0, 1);
    check(s.quadPage == 1, "setup: on page1");
    // Press B1.
    feed(bn, s, 1u << BTN_TR, 2); feed(bn, s, 0, 1);
    check(s.view  == View::Focus,  "page1->B1: enter Focus");
    check(s.focus == StatId::Hp,   "page1->B1: focus==Hp (POWER page)");
  }

  // -----------------------------------------------------------------------
  // 8. In Focus: ANY button returns to the Quad overview (page holding the stat)
  // -----------------------------------------------------------------------
  {
    // Edge button exits focus.
    ButtonNav bn; NavState s;
    feed(bn, s, 1u << BTN_TL, 2); feed(bn, s, 0, 1);   // Focus on Trans (page0)
    check(s.view == View::Focus, "setup: in Focus");
    feed(bn, s, 1u << BTN_RIGHT, 2); feed(bn, s, 0, 1); // any button -> exit
    check(s.view == View::Quad, "focus + right edge -> back to Quad");
    check(s.quadPage == 0,      "focus exit: quadPage holds the stat (0)");
  }
  {
    // Quadrant button (even a different one) also just exits focus.
    ButtonNav bn; NavState s;
    s.view = View::Focus; s.focus = StatId::Intake;     // page5 stat (DIAG page)
    feed(bn, s, 1u << BTN_TL, 2); feed(bn, s, 0, 1);    // different quadrant button
    check(s.view     == View::Quad, "focus + any quadrant -> back to Quad");
    check(s.quadPage == 5,          "focus exit: quadPage holds focused stat (Intake=page5)");
  }
  {
    // Left edge also exits focus.
    ButtonNav bn; NavState s;
    feed(bn, s, 1u << BTN_TR, 2); feed(bn, s, 0, 1);    // Focus on Oil (page0)
    check(s.view == View::Focus, "setup: focus Oil");
    feed(bn, s, 1u << BTN_LEFT, 2); feed(bn, s, 0, 1);
    check(s.view == View::Quad, "focus + left edge -> back to Quad");
  }

  // -----------------------------------------------------------------------
  // 11. Multi-button: two buttons pressed simultaneously — each fires once
  // -----------------------------------------------------------------------
  {
    ButtonNav bn; NavState s;
    // Pressing B4 (left) and B5 (right) together — each should debounce independently.
    // Net effect on 2-page Quad: left then right (or right then left per loop order) = toggle twice = back to 0.
    feed(bn, s, (1u << BTN_LEFT) | (1u << BTN_RIGHT), 2);
    feed(bn, s, 0, 1);
    // Both fired once: page toggled twice (0->1->0).
    check(s.quadPage == 0, "simultaneous L+R: page toggles twice, returns to 0");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
