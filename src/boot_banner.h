#pragma once
#include <cstddef>
#include <cstdint>

// One-shot identity line printed at the end of setup(). This exists because
// setup() otherwise prints NOTHING on the success path, so neither a field log
// nor the HIL rig can tell which build is running or why it restarted.
//
// Pure and host-tested (test/test_boot_banner.cpp): no Arduino, no ESP-IDF, no
// globals. setup() gathers the values; this only formats them.
//
// DELIBERATELY NO VIN FIELD. A VIN in a field log is a real PII leak and
// scripts/check_no_pii.py would reject it.
struct BootInfo {
  const char* env;         // OTA_ENV, e.g. "crowpanel_obd"
  const char* version;     // FW_VERSION — "local" for a bench `pio run`
  const char* git;         // FW_GIT     — "local" for a bench `pio run`
  const char* profileKey;  // settings.vehicleKey; "" or nullptr -> "generic"
  uint32_t psramBytes;
  uint32_t flashBytes;
  int      resetReason;    // esp_reset_reason() as an int
  uint32_t freeHeap;
};

// Formats two '\n'-separated lines into `out`. Returns the number of bytes
// actually written into `out` (excluding the NUL), clamped so it never
// exceeds n — unlike raw snprintf(), which reports the untruncated length
// even when the buffer was too small. A caller can still detect truncation
// by comparing the return value to strlen(out) or to n-1. `out` is always
// NUL-terminated when n > 0.
size_t formatBootBanner(const BootInfo& bi, char* out, size_t n);
