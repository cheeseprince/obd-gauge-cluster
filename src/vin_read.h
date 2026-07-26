#pragma once
#include <Arduino.h>
#include <string>
#include "vin.h"

// How many times a source re-attempts the one-per-connect 0902 VIN read before
// latching. A transient cold-connect miss (the "stuck on Generic" field bug)
// usually answers by the 2nd attempt; the cap bounds a car that never answers
// 0902 to ~3 read timeouts, after which auto-detect re-tries on next reconnect.
static constexpr int VIN_READ_ATTEMPTS = 3;

// Read the VIN over an already-connected ELM `io` (the same duck-typed IO
// pidQueryStep uses: write/available/read/flushRx). Sets standard header 7E0,
// sends 0902, drains to the '>' prompt, parses via parseVinReply. Returns true +
// fills out[18]. Read-only (Mode-09 only). The caller MUST reset the query
// engine header afterward (q_.curHeader = -1) so polling re-issues the profile's
// header — this call leaves the ELM at 7E0.
template <typename IO>
inline bool readVinOverIo(IO& io, char out[18], uint32_t timeoutMs = 1500) {
  auto drainToPrompt = [&](uint32_t ms) -> std::string {
    std::string rx; uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      while (io.available()) { char c = (char)io.read(); rx += c; if (c == '>') return rx; }
    }
    return rx;
  };
  io.flushRx();
  io.write("ATSH7E0\r");
  drainToPrompt(400);                    // consume the OK> from ATSH
  io.flushRx();
  io.write("0902\r");
  std::string rx = drainToPrompt(timeoutMs);
  bool ok = parseVinReply(rx.c_str(), out);
  if (!ok) Serial.println("[VIN] parse failed");   // no raw reply logged — it contains the VIN
  return ok;
}
