#pragma once
#include "nav_model.h"

// Navigation input device interface.
//
// The name is historical: this was the PCF8574 button-expander driver on the
// retired Elecrow board. It is now the INTERFACE that navigation input
// implements, and the one implementation that ships is the Modulino rotary
// encoder (encoder_input.cpp) on the CrowPanel Advance. The namespace is kept
// as buttonInput:: because main.cpp and the UI call through it; renaming it
// would churn several files to no functional end.
//
// Call begin() once in setup(). update(navState) is normally called from a
// dedicated core-0 task (see main.cpp) so input polling never stalls behind the
// LVGL render/flush on core 1. NavState is shared across cores, so the device
// read is done lock-free and only the NavState mutation is guarded by a
// spinlock; the render core wraps its own NavState access in lockNav()/unlockNav().

namespace buttonInput {

// Bring up the input device and auto-discover it on I2C.
// Logs found/not-found status over Serial.
void begin();

// Poll the device and drive NavState (guarded). No-op if none was found.
void update(NavState& s);

// Returns true if an input device was detected during begin().
bool present();

// Cross-core spinlock guarding the shared NavState. The render core brackets its
// NavState reads/writes (snapshot, touch tap-select) with these.
void lockNav();
void unlockNav();

}  // namespace buttonInput
