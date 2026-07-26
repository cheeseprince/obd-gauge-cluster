#pragma once
// vehicle_active.h — the active vehicle profile, selected at runtime.
//
// Every consumer reads the profile through these three macros. They used to be
// link-time globals (one profile compiled per binary); now they redirect to a
// runtime pointer set once at boot (main.cpp) or in a test's setup. This is what
// lets all profiles coexist in one binary. See the vehicle-profile-framework spec.
#include "vehicle_profile.h"   // full VehicleProfile (includes readouts.h for ReadoutDef)

// g_activeProfile is nullptr until main.cpp's setup() selects a profile. NOTHING
// that runs during static initialization — e.g. the constructor of a file-scope
// global such as the OBD source (which holds an ObdSchedule) — may read
// VEHICLE/READOUTS, or it will dereference null before setup() runs. Defer any
// such read to a begin()-style method invoked from setup() after selection.
extern const VehicleProfile* g_activeProfile;   // null until setup() sets it; non-null thereafter

#define VEHICLE        (*g_activeProfile)
#define READOUTS       (VEHICLE.readouts)        // const ReadoutDef* — READOUTS[i] still works
#define READOUT_COUNT  STAT_COUNT                // constexpr; == every profile's table size
