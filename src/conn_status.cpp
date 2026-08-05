#include "obd_source.h"
#include <cstdio>

const char* connPhaseName(ConnPhase p) {
  switch (p) {
    case ConnPhase::Scanning:     return "Scanning";
    case ConnPhase::Connecting:   return "Connecting";
    case ConnPhase::Initializing: return "Initializing";
    case ConnPhase::Up:           return "Up";
    default:                      return "Idle";
  }
}

void formatConnStatus(const ConnStatus& cs, uint32_t now, char* out, int outSize) {
  const char* title = (cs.phase == ConnPhase::Scanning) ? "Scanning..."
                                                        : "Connecting to OBD...";
  const char* who   = (cs.addr && cs.addr[0]) ? cs.addr : "no saved adapter";
  uint32_t elapsed  = (now >= cs.sinceMs) ? (now - cs.sinceMs) / 1000u : 0u;
  // ASCII only — LVGL Montserrat lacks middle-dot / arrow glyphs.
  snprintf(out, outSize,
           "%s\n%s\nPhase: %s   Attempt %u - %us\nhold knob for settings",
           title, who, connPhaseName(cs.phase),
           (unsigned)cs.attempts, (unsigned)elapsed);
}
