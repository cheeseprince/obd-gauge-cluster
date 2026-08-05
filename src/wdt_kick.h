#pragma once
#include <cstdint>

// Has the OBD task's heartbeat gone stale? Pure, so it is host-tested.
//
// THE DELTA MUST BE SIGNED. Core 1 samples `now = millis()` at the top of loop()
// and only reaches the watchdog check further down -- after, among other things,
// a blocking flash write (esp_ota_mark_app_valid_cancel_rollback) at ~20 s.
// Core 0 keeps stamping the heartbeat throughout. So the heartbeat is routinely
// a few milliseconds NEWER than the `now` it is compared against, and an
// unsigned `now - heartbeat` underflows to ~4.29e9 ms -- greater than any
// window, so a perfectly healthy board reboots itself. Observed on the bench
// 2026-08-04 as, verbatim:
//     [WDT] OBD task stalled 4294967s in phase Up (18s in phase, 1 attempts)
// 4294967 s is UINT32_MAX ms: the underflow, printed.
//
// A signed delta reads "heartbeat is ahead of now" as a small negative, and
// keeps the wrap-safety the unsigned form had across the ~49-day millis()
// rollover. Same idiom as the timer comparisons in ota_update.cpp and main.cpp.
//
// heartbeat == 0 means the task has never stamped it, which is not a stall.
inline bool obdHeartbeatStalled(uint32_t nowMs, uint32_t heartbeatMs,
                                uint32_t windowMs) {
  if (heartbeatMs == 0) return false;
  return (int32_t)(nowMs - heartbeatMs) > (int32_t)windowMs;
}

// Software-watchdog kick for long synchronous core-0 OBD work.
//
// main.cpp's core-1 loop reboots the board if the OBD task's heartbeat stalls
// >240s. The heartbeat is normally stamped once per poll() — but the one-shot
// diagnostics ('x' scan = 544 sequential probes, 'g' probe = 20 samples) run
// entirely INSIDE a single poll(), and with a quiet ECU each probe waits its
// full timeout, easily exceeding the window. Those loops call this per probe
// so a healthy-but-slow diagnostic can't be mistaken for a hang. Defined in
// main.cpp (it owns the heartbeat); safe to call from core 0 at any rate.
void obdWatchdogKick();
