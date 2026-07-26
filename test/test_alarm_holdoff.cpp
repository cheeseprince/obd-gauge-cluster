#include <cstdio>
#include "alarm_holdoff.h"
#include "gauge_model.h"
static int failures=0; static void check(bool c,const char*m){if(!c){printf("FAIL: %s\n",m);failures++;}}
int main(){
  AlarmHoldoff h;
  // Below hold-off: not confirmed.
  check(!h.confirmed(0, Zone::Red, 0),    "t0 red not yet confirmed");
  check(!h.confirmed(0, Zone::Red, 3999), "t<4s still not confirmed");
  // At/after hold-off: confirmed.
  check( h.confirmed(0, Zone::Red, 4000), "t=4s confirmed");
  check( h.confirmed(0, Zone::Amber, 8000), "stays confirmed while non-green");
  // Green resets.
  check(!h.confirmed(0, Zone::Green, 9000), "green clears");
  check(!h.confirmed(0, Zone::Red, 9500),   "re-arm starts new window");
  check( h.confirmed(0, Zone::Red, 13500),  "confirmed after new 4s");
  // Independent per index.
  AlarmHoldoff h2;
  check(!h2.confirmed(1, Zone::Red, 0), "idx1 independent t0");
  check( h2.confirmed(0, Zone::Red, 0) == false, "idx0 independent t0");
  printf(failures?"\n%d FAILED\n":"\nALL PASS\n",failures); return failures?1:0;
}
