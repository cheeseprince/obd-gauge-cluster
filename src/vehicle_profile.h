#pragma once
#include <cstddef>        // size_t (AddressingDef::emit signature)
#include "readouts.h"     // ReadoutDef, RF_* flags
#include "app_types.h"    // StatId, STAT_COUNT

// ============================================================================
//  VEHICLE PROFILE — everything the firmware knows about one specific vehicle.
//  Exactly ONE src/vehicles/*.cpp per build defines VEHICLE (compile-time
//  selection; Phase 2 makes this an array + runtime pointer).
//  A stat the vehicle can't read = row with cmd==nullptr and no RF_COMPUTED
//  flag -> never scheduled/polled; layouts may not reference it (test_profile
//  enforces this on the host).
//  StatId is the append-only GLOBAL union across vehicles (see app_types.h).
//  User layout override: define USER_LAYOUT in the env and provide
//  src/user_layout.h defining `extern const LayoutDef USER_LAYOUT_DEF` — the
//  facade (readouts.cpp) then serves it instead of VEHICLE.defaultLayout.
// ============================================================================
struct LayoutDef {
  const StatId (*pages)[4];      // 4 cells per page; StatId::COUNT = empty cell
  int pageCount;
  const char* const* pageNames;  // one short name per page (header bar)
  const StatId* helpers;         // polled but never displayed (computed inputs)
  int helperCount;
};

// How to select one CAN module/header on the ELM327. Each profile supplies its
// own table so the query engine stays addressing-blind. emit() returns the Nth
// ELM setup command to select this header (written into buf if it must be built,
// else a static string), or nullptr when the setup sequence is complete. Called
// on the core-0 query task only. can29 selects the 29-bit forms (GM WiFi adapter).
struct AddressingDef {
  const char* label;   // "7E0", "BMW-618" — diagnostics/logs only
  const char* (*emit)(int step, bool can29, char* buf, size_t n);
};

struct VehicleProfile {
  const char* name;        // splash line 1, e.g. "GMC Sierra 1500"
  const char* engine;      // splash line 2, e.g. "Duramax 3.0L Diesel"
  float dieselTankGal;     // fuel tank capacity (gal) — DSL+ gallons-to-fill
  float defTankGal;        // DEF tank capacity (gal) — DEF+ gallons-to-fill
  const ReadoutDef* readouts;   // exactly STAT_COUNT rows, StatId order
  LayoutDef defaultLayout;      // profile's default screens (USER_LAYOUT overrides)
  const AddressingDef* addressing;   // profile-supplied addressing table
  int addressingCount;               // a row's `header` index must be < this
};
