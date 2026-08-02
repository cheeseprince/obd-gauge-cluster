#include <cstdio>
#include "stack_watch.h"
static int failures=0; static void check(bool c,const char*m){if(!c){printf("FAIL: %s\n",m);failures++;}}

int main(){
  // A stored 0 means "never written" (NVS getUInt default) -> treated as unset,
  // and the first real sample is always worth persisting.
  {
    StackWatch w;
    StackMins stored;                       // all default = STACK_UNSET
    stored.loopFree = 0; stored.obdFree = 0; stored.inputFree = 0;
    w.seed(stored);
    StackMins s; s.loopFree = 9000; s.obdFree = 3000; s.inputFree = 1500;
    check(w.update(s, 1000), "first real sample is worth persisting");
    check(w.mins().loopFree == 9000, "min takes the first sample");
  }
  // Minimums only ever decrease: a larger later sample must not raise them.
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins lo; lo.loopFree = 5000; lo.obdFree = 5000; lo.inputFree = 5000;
    w.update(lo, 0); w.markPersisted(0);
    StackMins hi; hi.loopFree = 9000; hi.obdFree = 9000; hi.inputFree = 9000;
    check(!w.update(hi, 100000), "higher sample does not raise the minimum");
    check(w.mins().loopFree == 5000, "minimum stays at the low-water value");
  }
  // A drop smaller than WRITE_DROP_BYTES is not worth a flash write.
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins a; a.loopFree = 9000; a.obdFree = 3000; a.inputFree = 1500;
    w.update(a, 0); w.markPersisted(0);
    StackMins b = a; b.loopFree = 9000 - (StackWatch::WRITE_DROP_BYTES - 1);
    check(!w.update(b, 100000), "sub-threshold drop does not trigger a write");
    check(w.mins().loopFree == b.loopFree, "but the minimum is still tracked");
  }
  // A drop >= WRITE_DROP_BYTES does trigger, once the rate limit allows it.
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins a; a.loopFree = 9000; a.obdFree = 3000; a.inputFree = 1500;
    w.update(a, 0); w.markPersisted(0);
    StackMins b = a; b.loopFree = 9000 - StackWatch::WRITE_DROP_BYTES;
    check(!w.update(b, 1000), "big drop still rate-limited inside the window");
    check(w.update(b, StackWatch::WRITE_MIN_INTERVAL_MS), "big drop persists once the window passes");
  }
  // Any of the three tasks dropping is enough on its own.
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins a; a.loopFree = 9000; a.obdFree = 3000; a.inputFree = 1500;
    w.update(a, 0); w.markPersisted(0);
    StackMins b = a; b.inputFree = 1500 - StackWatch::WRITE_DROP_BYTES;
    check(w.update(b, StackWatch::WRITE_MIN_INTERVAL_MS), "input-task drop alone triggers");
  }
  // millis() wrap must not defeat the rate limit (wrap-safe comparison).
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins a; a.loopFree = 9000; a.obdFree = 3000; a.inputFree = 1500;
    uint32_t late = 0xFFFFF000u;                 // just before wrap
    w.update(a, late); w.markPersisted(late);
    StackMins b = a; b.loopFree = 9000 - StackWatch::WRITE_DROP_BYTES;
    uint32_t afterWrap = late + StackWatch::WRITE_MIN_INTERVAL_MS;  // wraps past 0
    check(w.update(b, afterWrap), "rate limit survives millis() wrap");
  }
  printf(failures?"\n%d FAILED\n":"\nALL PASS\n",failures); return failures?1:0;
}
