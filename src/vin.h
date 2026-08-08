#pragma once
#include <cstdint>

// Parse the trailing 17 alphanumeric bytes of a Mode-09 PID-02 payload into a
// VIN (out[18], NUL-terminated, uppercased). Returns false on short/garbled input.
bool parseVinFromPayload(const uint8_t* payload, int len, char out[18]);

// Assemble + parse a full Mode-09 0902 ELM reply string (multi-frame) into a VIN.
bool parseVinReply(const char* elmReply, char out[18]);

// What a VIN tells us about the vehicle, INDEPENDENT of whether we have a
// profile for it.
//
// Splitting identity from profile selection is the point. Recognising a vehicle
// and having tuned PID tables for it are different things, and conflating them
// meant a truck we could name perfectly well showed up as "Generic" with no
// explanation. `profileKey` is nullptr for a vehicle we can identify but cannot
// yet drive; adding a profile later is filling that field in, with no change to
// any VIN logic.
struct VinIdentity {
  const char* profileKey;   // registry key, or nullptr: identified but unprofiled
  const char* name;         // e.g. "Ford F-250" -- shown when profileKey is null
  const char* engine;       // e.g. "6.7L Power Stroke", or "" when not nameable
};

// Identify a VIN, filling *out. Returns false (and leaves *out untouched) when
// nothing matches.
//
// Fills a caller-provided struct rather than returning a pointer to a table
// row, because name and engine come from SEPARATE lookups: every make encodes
// its model line and its engine at different VIN positions, so a single row
// per (model, engine) pair would duplicate the engine strings across dozens of
// rows. `engine` is "" when the code is one we deliberately refuse to name.
//
// Every pattern here was derived from NHTSA vPIC (US Government, public domain)
// by decoding partial VINs, never guessed -- see docs/VEHICLES.md for the
// method. Patterns are bounded to the model years they were verified over,
// because a positional rule alone is NOT unique across 20 years: the GM rule
// once also matched a 2011-12 Express van.
bool vinIdentify(const char* vin, VinIdentity* out);

// Map a VIN to a REGISTRY KEY, or nullptr. A vehicle we can name but have no
// profile for returns nullptr here, which is what keeps it on the Generic
// profile: identifying a truck and knowing how to read it are different things.
const char* vinToProfileKey(const char* vin);

// What the dash should REMEMBER about this vehicle for the boot splash. Returns
// false for "remember nothing" (which the caller reads as: clear whatever is
// stored). True only for a vehicle we can name but have no profile for --
// a profiled vehicle needs no stored name because its profile carries one, and
// leaving a stale name behind would caption the wrong truck.
bool vinDisplayIdentity(const char* vin, VinIdentity* out);

// The reboot-or-not decision. Returns the key to switch to, or nullptr for
// "no change" (auto off, empty/unmapped VIN, key not in registry, or already current).
const char* vinAutoTarget(const char* vin, bool vehicleAuto, const char* currentKey);
