#include "vehicle_registry.h"
#include <cstring>

extern const VehicleProfile GENERIC_PROFILE;
extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;
extern const VehicleProfile BMW_F10_535I_PROFILE;
extern const VehicleProfile AUDI_Q5_PROFILE;
extern const VehicleProfile JEEP_WS_PROFILE;
extern const VehicleProfile FORD_SD_67_PROFILE;
extern const VehicleProfile STANDARD_PLUS_DIESEL_PROFILE;
extern const VehicleProfile STANDARD_PLUS_GAS_PROFILE;

// Generic FIRST — it is the default and the unknown-VIN fallback.
const ProfileEntry PROFILE_REGISTRY[] = {
  { "generic",       "Generic OBD-II",              &GENERIC_PROFILE },
  { "gm_sierra_lz0", "GMC Sierra 1500 - Duramax",   &GM_SIERRA_LZ0_PROFILE },
  { "bmw_f10_535i",  "BMW 535i - N55",              &BMW_F10_535I_PROFILE },
  { "audi_q5",       "Audi Q5 - 2.0T",              &AUDI_Q5_PROFILE },
  { "jeep_ws",       "Jeep Wagoneer - 5.7L",        &JEEP_WS_PROFILE },
  { "ford_sd_67",    "Ford Super Duty - 6.7L",      &FORD_SD_67_PROFILE },
  // Standard+ is NOT a scanned vehicle profile -- it is the legislated Mode-01
  // set, selected by VIN for trucks we can name but have never scanned.
  { "std_diesel",    "Standard+ Diesel",            &STANDARD_PLUS_DIESEL_PROFILE },
  { "std_gas",       "Standard+ Gas",               &STANDARD_PLUS_GAS_PROFILE },
};
const int PROFILE_REGISTRY_COUNT = (int)(sizeof(PROFILE_REGISTRY)/sizeof(PROFILE_REGISTRY[0]));

int profileCount() { return PROFILE_REGISTRY_COUNT; }

const VehicleProfile* profileForKey(const char* key) {
  if (!key) return nullptr;
  for (int i = 0; i < PROFILE_REGISTRY_COUNT; i++)
    if (strcmp(PROFILE_REGISTRY[i].key, key) == 0) return PROFILE_REGISTRY[i].profile;
  return nullptr;
}

int profileIndexForKey(const char* key) {
  if (!key) return -1;
  for (int i = 0; i < PROFILE_REGISTRY_COUNT; i++)
    if (strcmp(PROFILE_REGISTRY[i].key, key) == 0) return i;
  return -1;
}

// Reverse lookup: the registry key of a profile we already hold a pointer to.
// Needed because settings.vehicleKey is EMPTY in VIN auto-detect mode, so it
// cannot identify the profile actually running — but the tank override has to
// be scoped to the real vehicle either way. Pointer compare, not strcmp: every
// profile is a single extern object, so identity is the definition here.
const char* profileKeyFor(const VehicleProfile* p) {
  if (!p) return "";
  for (int i = 0; i < PROFILE_REGISTRY_COUNT; i++)
    if (PROFILE_REGISTRY[i].profile == p) return PROFILE_REGISTRY[i].key;
  return "";
}

const char* profileKeyAt(int i)   { return (i>=0 && i<PROFILE_REGISTRY_COUNT) ? PROFILE_REGISTRY[i].key   : ""; }
const char* profileLabelAt(int i) { return (i>=0 && i<PROFILE_REGISTRY_COUNT) ? PROFILE_REGISTRY[i].label : ""; }
