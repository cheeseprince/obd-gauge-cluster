#pragma once
#include <cstdint>
#include "nav_model.h"

// Physical bezel button indices.
// Layout: B0=top-left (cell 0), B1=top-right (cell 1),
//         B2=bottom-left (cell 2), B3=bottom-right (cell 3),
//         B4=left-edge, B5=right-edge.
// Cells 0..3 match nav::statForCell's 2x2 reading order.
enum { BTN_TL=0, BTN_TR=1, BTN_BL=2, BTN_BR=3, BTN_LEFT=4, BTN_RIGHT=5, BTN_COUNT=6 };

// Pure logic class: no Arduino/Wire headers — fully host-testable.
// Call update() every firmware tick with a bitmask of raw button state
// (bit i = 1 means button i is physically pressed).
class ButtonNav {
 public:
  // pressedMask: bit i set = button i physically pressed this tick.
  // Debounces each button and fires actions on stable rising edges.
  void update(uint8_t pressedMask, NavState& s);

 private:
  uint8_t debounced_ = 0;             // current debounced pressed-state bitmask
  uint8_t counts_[BTN_COUNT] = {0};   // consecutive ticks each button has been in the new state

  // Consecutive ticks required to accept a state change. Set to 1 (fire on the
  // first poll that sees the change): buttons are polled once per render loop
  // (tens of ms, variable), which is far slower than mechanical bounce (~1-5ms),
  // so the poll interval already debounces. Requiring 2 ticks made quick taps —
  // held across only one poll — get dropped.
  static const uint8_t DEBOUNCE_TICKS = 1;

  // Called once per button on a confirmed debounced rising edge.
  void onPress(int btn, NavState& s);
};
