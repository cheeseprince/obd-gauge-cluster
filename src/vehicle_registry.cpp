#include "vehicle_registry.h"
#include <cstring>

extern const VehicleProfile GENERIC_PROFILE;
extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;
extern const VehicleProfile BMW_F10_535I_PROFILE;
extern const VehicleProfile AUDI_Q5_PROFILE;
extern const VehicleProfile JEEP_WS_PROFILE;

// Generic FIRST — it is the default and the unknown-VIN fallback.
const ProfileEntry PROFILE_REGISTRY[] = {
  { "generic",       "Generic OBD-II",              &GENERIC_PROFILE },
  { "gm_sierra_lz0", "GMC Sierra 1500 - Duramax",   &GM_SIERRA_LZ0_PROFILE },
  { "bmw_f10_535i",  "BMW 535i - N55",              &BMW_F10_535I_PROFILE },
  { "audi_q5",       "Audi Q5 - 2.0T",              &AUDI_Q5_PROFILE },
  { "jeep_ws",       "Jeep Wagoneer - 5.7L",        &JEEP_WS_PROFILE },
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

const char* profileKeyAt(int i)   { return (i>=0 && i<PROFILE_REGISTRY_COUNT) ? PROFILE_REGISTRY[i].key   : ""; }
const char* profileLabelAt(int i) { return (i>=0 && i<PROFILE_REGISTRY_COUNT) ? PROFILE_REGISTRY[i].label : ""; }
