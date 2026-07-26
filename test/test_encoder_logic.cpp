#include <cstdio>
#include "encoder_logic.h"
static int failures=0;
static void check(bool c,const char*m){if(!c){printf("FAIL: %s\n",m);failures++;}}
int main(){
  // rotation: first call seeds (0), then returns deltas (signed).
  EncoderLogic r;
  check(r.rotation(100) == 0,  "first rotation call seeds, returns 0");
  check(r.rotation(103) == 3,  "rotation +3 detents");
  check(r.rotation(102) == -1, "rotation -1 detent");
  check(r.rotation(102) == 0,  "rotation no change");

  // button: short press (press then release before LONG_MS).
  EncoderLogic b;
  check(b.button(false, 0)   == EncEvent::None,       "idle = None");
  check(b.button(true, 100)  == EncEvent::None,       "press edge = None");
  check(b.button(true, 300)  == EncEvent::None,       "held <600 = None");
  check(b.button(false, 400) == EncEvent::PressShort, "release <600 = PressShort");

  // button: long press fires once at threshold, not again, none on release.
  EncoderLogic l;
  check(l.button(true, 1000)  == EncEvent::None,      "press edge = None");
  check(l.button(true, 1599)  == EncEvent::None,      "held 599 = None");
  check(l.button(true, 1600)  == EncEvent::PressLong, "held 600 = PressLong");
  check(l.button(true, 1900)  == EncEvent::None,      "still held = None (fires once)");
  check(l.button(false, 2000) == EncEvent::None,      "release after long = None");

  printf(failures?"\n%d FAILED\n":"\nALL PASS\n",failures); return failures?1:0;
}
