#pragma once
#include <cstdint>

// Parse the trailing 17 alphanumeric bytes of a Mode-09 PID-02 payload into a
// VIN (out[18], NUL-terminated, uppercased). Returns false on short/garbled input.
bool parseVinFromPayload(const uint8_t* payload, int len, char out[18]);

// Assemble + parse a full Mode-09 0902 ELM reply string (multi-frame) into a VIN.
bool parseVinReply(const char* elmReply, char out[18]);

// Map a VIN's WMI (chars 1-3, case-insensitive) to a REGISTRY KEY, or nullptr.
const char* vinToProfileKey(const char* vin);

// The reboot-or-not decision. Returns the key to switch to, or nullptr for
// "no change" (auto off, empty/unmapped VIN, key not in registry, or already current).
const char* vinAutoTarget(const char* vin, bool vehicleAuto, const char* currentKey);
