#pragma once
#include "obd_source.h"
#if defined(ARDUINO)
#include <Arduino.h>   // portMUX spinlock for the cross-core snapshot (device builds)
#endif

// Synthetic OBD source: deterministic, time-driven animated readings.
// Two modes:
//   safe mode  (default) — thresholded stats stay in their GREEN band, so the
//                          alarm overlay never fires (lets you test UI/buttons).
//   sweep mode            — thresholded stats cross warn/crit zones to exercise
//                          the alarm overlay.
class MockObdSource : public ObdSource {
 public:
  void begin() override;
  void poll(uint32_t nowMs) override;
  // Guarded snapshot on device: poll() rewrites all 31 slots on core 0 while the
  // core-1 render copies — an unlocked copy was a torn-snapshot data race (every
  // real source locks this). Host tests are single-threaded, so no lock there.
  ObdReadings latest() const override;

  void setSafeMode(bool on) { safe_ = on; }
  bool safeMode() const { return safe_; }

 private:
  ObdReadings cur_;
#if defined(ARDUINO)
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;   // spinlock for cur_
#endif
  bool safe_ = true;   // default safe: no alarms on boot (bench UI/button testing)
};
