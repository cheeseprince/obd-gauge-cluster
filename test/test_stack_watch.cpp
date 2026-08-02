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
  // The FIRST write is a boot-time seed and must NOT arm the lockout (Fix 3):
  // only a SECOND write starts the window, and only a write inside THAT
  // window is suppressed.
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins a; a.loopFree = 9000; a.obdFree = 3000; a.inputFree = 1500;
    w.update(a, 0); w.markPersisted(0);   // write #1 (seed) -- must not arm the lockout
    StackMins b = a; b.loopFree = 9000 - StackWatch::WRITE_DROP_BYTES;
    check(w.update(b, 1000), "second write is not blocked by the seeding write");
    w.markPersisted(1000);                // write #2 -- NOW the lockout arms
    StackMins c = b; c.loopFree = b.loopFree - StackWatch::WRITE_DROP_BYTES;
    check(!w.update(c, 1000 + 1), "big drop still rate-limited inside the window");
    check(w.update(c, 1000 + StackWatch::WRITE_MIN_INTERVAL_MS), "big drop persists once the window passes");
  }
  // Any of the three tasks dropping is enough on its own, once the lockout is
  // actually armed (by a second write -- see Fix 3 above; the first write
  // must not arm it, so this exercises write #2 and #3 rather than #1).
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins a; a.loopFree = 9000; a.obdFree = 3000; a.inputFree = 1500;
    w.update(a, 0); w.markPersisted(0);                  // write #1 (seed) -- unarmed
    StackMins b = a; b.inputFree = 1500 - StackWatch::WRITE_DROP_BYTES;
    check(w.update(b, 1000), "second write (input-task drop) is not blocked by the seeding write");
    w.markPersisted(1000);                               // write #2 -- arms the lockout
    StackMins c = b; c.obdFree = 3000 - StackWatch::WRITE_DROP_BYTES;
    check(w.update(c, 1000 + StackWatch::WRITE_MIN_INTERVAL_MS), "obd-task drop alone triggers once the window passes");
  }
  // millis() wrap: once the lockout is armed by a second write timestamped
  // just before the wrap, the interval check must not mis-fire across it.
  // This demonstrates the rate limit does NOT mis-fire across the wrap --
  // it does not "prove" the wrap-safe idiom in general, since a naive
  // `nowMs - lastWriteMs_ < INTERVAL` comparison would pass this exact case
  // identically. It is a documented boundary case, not a proof.
  {
    StackWatch w; StackMins stored; w.seed(stored);
    StackMins a; a.loopFree = 9000; a.obdFree = 3000; a.inputFree = 1500;
    w.update(a, 0); w.markPersisted(0);                  // write #1 (seed) -- unarmed
    StackMins b = a; b.loopFree = 9000 - StackWatch::WRITE_DROP_BYTES;
    uint32_t late = 0xFFFFF000u;                         // just before wrap
    check(w.update(b, late), "second write arms the lockout, timestamped just before the wrap");
    w.markPersisted(late);
    StackMins c = b; c.loopFree = b.loopFree - StackWatch::WRITE_DROP_BYTES;
    uint32_t afterWrap = late + StackWatch::WRITE_MIN_INTERVAL_MS;  // wraps past 0
    check(w.update(c, afterWrap), "rate limit survives millis() wrap");
  }
  // Partially seeded StackMins: saveStackMins() (main.cpp) skips STACK_UNSET
  // fields, and commit 0df9b67 added UI that renders exactly this state, so
  // NVS can genuinely hold one field measured and the other two never
  // written (0 from NVS's own default). seed() must treat those as unset,
  // not as a real reading of 0 free bytes.
  {
    StackWatch w;
    StackMins stored;                     // loopFree measured before; obd/input never
    stored.loopFree = 5000; stored.obdFree = 0; stored.inputFree = 0;
    w.seed(stored);
    check(w.mins().loopFree == 5000, "seeded field keeps its stored value");
    check(w.mins().obdFree == STACK_UNSET, "never-written field reads as unset, not 0");
    check(w.mins().inputFree == STACK_UNSET, "never-written field reads as unset, not 0");

    // A sub-threshold drift on the already-seeded field alone must not trigger.
    StackMins a; a.loopFree = 5000 - (StackWatch::WRITE_DROP_BYTES - 1);
    a.obdFree = STACK_UNSET; a.inputFree = STACK_UNSET;
    check(!w.update(a, 0), "sub-threshold drift on the seeded field alone does not trigger");

    // But a first real reading on a still-unmeasured field always counts.
    StackMins b = a; b.obdFree = 3000;
    check(w.update(b, 0), "first real reading on a still-unset field triggers a persist");
  }
  printf(failures?"\n%d FAILED\n":"\nALL PASS\n",failures); return failures?1:0;
}
