#include "stack_watch.h"

static inline uint32_t minOf(uint32_t a, uint32_t b) { return a < b ? a : b; }

// NVS getUInt() returns 0 for a key that was never written; 0 free bytes is not
// something a running task can report, so 0 means "no data".
static inline uint32_t fromStored(uint32_t v) { return v == 0 ? STACK_UNSET : v; }

// True when `now` is a low enough new minimum, versus `was`, to earn a write.
static inline bool dropped(uint32_t now, uint32_t was) {
  if (now == STACK_UNSET) return false;          // nothing measured yet
  if (was == STACK_UNSET) return true;           // first real reading always counts
  return was > now && (was - now) >= StackWatch::WRITE_DROP_BYTES;
}

void StackWatch::seed(const StackMins& stored) {
  mins_.loopFree  = fromStored(stored.loopFree);
  mins_.obdFree   = fromStored(stored.obdFree);
  mins_.inputFree = fromStored(stored.inputFree);
  persisted_ = mins_;
}

bool StackWatch::update(const StackMins& sample, uint32_t nowMs) {
  mins_.loopFree  = minOf(mins_.loopFree,  sample.loopFree);
  mins_.obdFree   = minOf(mins_.obdFree,   sample.obdFree);
  mins_.inputFree = minOf(mins_.inputFree, sample.inputFree);

  const bool worth = dropped(mins_.loopFree,  persisted_.loopFree) ||
                     dropped(mins_.obdFree,   persisted_.obdFree)  ||
                     dropped(mins_.inputFree, persisted_.inputFree);
  if (!worth) return false;

  // Wrap-safe: the same idiom used for the timers in ota_update.cpp/main.cpp.
  if (everWrote_ &&
      (int32_t)(nowMs - (lastWriteMs_ + WRITE_MIN_INTERVAL_MS)) < 0) return false;
  return true;
}

void StackWatch::markPersisted(uint32_t nowMs) {
  persisted_  = mins_;
  lastWriteMs_ = nowMs;
  everWrote_   = true;
}
