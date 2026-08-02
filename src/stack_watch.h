#pragma once
#include <cstdint>

// "Never measured" sentinel. NVS returns 0 for a key that was never written,
// and 0 free bytes is not a value a live task can report, so seed() maps 0 to
// this and everything downstream compares against it explicitly.
inline constexpr uint32_t STACK_UNSET = 0xFFFFFFFFu;

// Minimum-ever FREE stack per task, in BYTES.
//
// BYTES, not words: uxTaskGetStackHighWaterMark() returns bytes on ESP-IDF
// (freertos/task.h — "in bytes not words"), unlike vanilla FreeRTOS which
// returns words. Do not scale these by sizeof(StackType_t).
struct StackMins {
  uint32_t loopFree  = STACK_UNSET;   // Arduino loopTask (runs OTA/TLS)
  uint32_t obdFree   = STACK_UNSET;   // obd_core0
  uint32_t inputFree = STACK_UNSET;   // input_core0
};

// Tracks the low-water mark of each task's free stack and decides when a new
// low is worth a flash write. Deliberately free of FreeRTOS and NVS: sampling
// and persistence are the caller's job, which keeps this host-testable.
class StackWatch {
 public:
  // A new low must beat the persisted value by at least this much to earn a
  // write. Without it, a stack that creeps down a few bytes at a time would
  // write on nearly every sample.
  static const uint32_t WRITE_DROP_BYTES = 256;
  // Minimum spacing between writes. Bounds flash wear if something pathological
  // drives the stack down repeatedly.
  static const uint32_t WRITE_MIN_INTERVAL_MS = 60000;

  // Load previously persisted minima (0 for any never written).
  void seed(const StackMins& stored);

  // Fold in a fresh sample. Returns true when the caller should persist mins().
  bool update(const StackMins& sample, uint32_t nowMs);

  // Call after a successful persist so the rate limit and baseline advance.
  void markPersisted(uint32_t nowMs);

  const StackMins& mins() const { return mins_; }

 private:
  StackMins mins_;
  StackMins persisted_;
  uint32_t  lastWriteMs_ = 0;
  bool      everWrote_   = false;
};
