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
// that would have been written (snprintf semantics): the untruncated length,
// which can exceed n when `out` was too small. `ret >= n` is therefore the
// unambiguous signal that the banner was truncated — a clamped return value
// could not tell truncation apart from an exact fit, and that distinction is
// exactly what matters if the banner ever outgrows the caller's buffer.
// `out` is always NUL-terminated by snprintf when n > 0 regardless of
// truncation, so treat the return value as a length to TEST against n, never
// as an index to walk `out` with.
size_t formatBootBanner(const BootInfo& bi, char* out, size_t n);
