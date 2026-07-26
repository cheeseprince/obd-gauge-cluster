#include "button_nav.h"

// update() — debounce all 6 buttons and fire onPress() on stable rising edges.
//
// Debounce is PRESS-ONLY (rising edge): contact noise at button-down is filtered by
// requiring DEBOUNCE_TICKS consecutive "pressed" samples before accepting. Release
// (falling edge) is accepted immediately — physical button release is clean and
// immediate acceptance allows the next press cycle to begin without waiting.
//
// Algorithm per button b (0..5):
//   If raw is NOT pressed and debounced IS pressed -> accept release immediately,
//     clear debounced_ bit, reset counts_[b].
//   If raw IS pressed and debounced is NOT pressed -> accumulate counts_[b]; when
//     counts_[b] reaches DEBOUNCE_TICKS accept the press: set debounced_ bit,
//     reset counts_[b], call onPress(b, s).
//   If raw matches debounced -> reset counts_[b] = 0 (stable, nothing to do).
void ButtonNav::update(uint8_t pressedMask, NavState& s) {
  for (int b = 0; b < BTN_COUNT; b++) {
    bool rawBit      = (pressedMask >> b) & 1;
    bool debounceBit = (debounced_  >> b) & 1;

    if (!rawBit && debounceBit) {
      // --- Falling edge: accept release immediately (no debounce needed) ---
      debounced_ &= ~(1u << b);
      counts_[b]  = 0;
    } else if (rawBit && !debounceBit) {
      // --- Rising edge: accumulate and fire once stable ---
      counts_[b]++;
      if (counts_[b] >= DEBOUNCE_TICKS) {
        debounced_ |= (1u << b);
        counts_[b]  = 0;
        onPress(b, s);
      }
    } else {
      // Raw matches debounced (already stable in either state) — reset counter.
      counts_[b] = 0;
    }
  }
}

// onPress() — translate a debounced button press into a NavState mutation using
// the existing nav:: primitives. No page/stat math is duplicated here.
//
// In FOCUS view: ANY button returns to the Quad overview (showing the page that
// holds the focused stat). This is the simple "any key exits zoom" behavior.
//
// In QUAD view:
//   Quadrant buttons (BTN_TL..BTN_BR == cell indices 0..3) → zoom that cell's stat.
//   BTN_LEFT  → nav::swipeRight (previous page).
//   BTN_RIGHT → nav::swipeLeft  (next page).
//   NOTE: physical left = "previous" maps to swipeRight, physical right = "next"
//         maps to swipeLeft ("swipe left" gesture = content moves left = forward).
void ButtonNav::onPress(int btn, NavState& s) {
  // Focus: any button exits back to the quad overview.
  if (s.view == View::Focus) {
    nav::tapBack(s);   // returns to the quad page holding the focused stat
    return;
  }

  // Quad view.
  if (btn <= BTN_BR) {
    // Quadrant button (0..3, matches cell index directly) -> zoom that stat.
    s.focus = nav::statForCell(s.quadPage, btn);
    s.view  = View::Focus;
  } else if (btn == BTN_LEFT) {
    nav::swipeRight(s);   // previous page
  } else if (btn == BTN_RIGHT) {
    nav::swipeLeft(s);    // next page
  }
}
