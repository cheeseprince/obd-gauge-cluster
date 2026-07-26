#include "vehicle_active.h"
// Dependency-free definition of the active-profile pointer. Compiled into every
// environment and linked by every host test, with NO profile dependencies — so an
// isolated per-profile test can set g_activeProfile without linking the registry.
const VehicleProfile* g_activeProfile = nullptr;
