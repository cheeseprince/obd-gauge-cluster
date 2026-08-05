// Software-watchdog stall predicate.
//
// The bug this pins: core 1 samples `now = millis()` at the top of loop(), then
// does work -- including a BLOCKING flash write
// (esp_ota_mark_app_valid_cancel_rollback) at ~20 s -- before reaching the
// watchdog check. Meanwhile core 0 keeps stamping the heartbeat. So the
// heartbeat can be NEWER than the `now` it is compared against, and an unsigned
// `now - heartbeat` underflows to ~4.29e9 ms, which is trivially greater than
// any window. The board then reboots itself while perfectly healthy.
//
// Observed on the bench 2026-08-04:
//   [WDT] OBD task stalled 4294967s in phase Up (18s in phase, 1 connect attempts)
// 4294967 s is UINT32_MAX ms -- the underflow, printed.
#include <cstdio>
#include "../src/wdt_kick.h"

static int failures = 0;
static void check(bool c, const char* m) { if (!c) { printf("FAIL: %s\n", m); failures++; } }

static const uint32_t W = 240000;   // the shipping window

int main() {
  // Never stamped: the task has not run yet, which is not a stall.
  check(!obdHeartbeatStalled(0, 0, W), "heartbeat never stamped -> not stalled");
  check(!obdHeartbeatStalled(500000, 0, W), "heartbeat never stamped, late boot -> not stalled");

  // Ordinary healthy cases.
  check(!obdHeartbeatStalled(1000, 1000, W), "same instant -> not stalled");
  check(!obdHeartbeatStalled(300000, 299000, W), "1s old -> not stalled");
  check(!obdHeartbeatStalled(300000, 300000 - W, W), "exactly the window -> not stalled");

  // A genuine stall.
  check(obdHeartbeatStalled(300000, 300000 - W - 1, W), "one ms past the window -> stalled");
  check(obdHeartbeatStalled(600000, 100000, W), "500s old -> stalled");

  // THE REGRESSION. The heartbeat is AHEAD of the sampled `now` because core 0
  // stamped it after core 1 read the clock. Unsigned arithmetic makes this look
  // like a 49-day stall; it is a healthy board a few ms out of step.
  check(!obdHeartbeatStalled(20000, 20001, W), "heartbeat 1ms ahead -> NOT stalled");
  check(!obdHeartbeatStalled(20000, 20050, W), "heartbeat 50ms ahead -> NOT stalled");
  check(!obdHeartbeatStalled(20000, 21000, W), "heartbeat 1s ahead -> NOT stalled");

  // millis() wraps every ~49 days. Both of these straddle the wrap: elapsed time
  // is small in the first and genuinely long in the second, and the predicate
  // must tell them apart -- which is the whole reason for a signed delta rather
  // than a special case for "heartbeat > now".
  {
    const uint32_t nearMax = 0xFFFFFF00u;
    check(!obdHeartbeatStalled(0x00000100u, nearMax, W),
          "wrap: 512ms elapsed across the rollover -> not stalled");
    check(obdHeartbeatStalled(nearMax + W + 1000, nearMax, W),
          "wrap: a real stall that straddles the rollover -> stalled");
  }

  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("test_wdt_heartbeat: ALL PASS\n");
  return 0;
}
