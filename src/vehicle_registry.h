#pragma once
#include "vehicle_profile.h"

struct ProfileEntry {
  const char*           key;      // stable NVS key, order-independent
  const char*           label;    // menu text
  const VehicleProfile* profile;
};

extern const ProfileEntry PROFILE_REGISTRY[];
extern const int          PROFILE_REGISTRY_COUNT;

const VehicleProfile* profileForKey(const char* key);   // nullptr if unknown
int          profileCount();                            // == PROFILE_REGISTRY_COUNT
const char*  profileKeyAt(int i);                       // "" if out of range
const char*  profileLabelAt(int i);                     // "" if out of range
int          profileIndexForKey(const char* key);       // -1 if unknown
const char*  profileKeyFor(const VehicleProfile* p);    // "" if unknown/null — key of the ACTIVE profile
