#pragma once
#include "nav_model.h"

// Device-layer interface for reading physical navigation buttons via a
// PCF8574 I2C GPIO expander wired to the board's Crowtail I2C port.
//
// Hardware: PCF8574T (addr range 0x20..0x27) or PCF8574A (0x38..0x3F).
// I2C bus: SDA=GPIO22, SCL=GPIO21 (Crowtail connector on the Elecrow board).
// Buttons wired active-low: pressed = GPIO LOW, released = GPIO HIGH.
//
// Call begin() once in setup(). update(navState) is normally called from a
// dedicated core-0 task (see main.cpp) so button polling never stalls behind the
// LVGL render/flush on core 1. NavState is shared across cores, so the I2C read
// is done lock-free and only the ButtonNav state mutation is guarded by a
// spinlock; the render core wraps its own NavState access in lockNav()/unlockNav().
namespace buttonInput {

// Configure I2C and scan for a PCF8574 expander.  Sets all pins to inputs
// by writing 0xFF (PCF8574 quasi-bidirectional with weak pull-ups).
// Logs found/not-found status over Serial.
void begin();

// Read one byte from the expander, invert active-low bits, remap to logical
// button order, and drive NavState (guarded). No-op if no expander present.
void update(NavState& s);

// Returns true if an expander was detected during begin().
bool present();

// Cross-core spinlock guarding the shared NavState. The render core brackets its
// NavState reads/writes (snapshot, touch tap-select) with these.
void lockNav();
void unlockNav();

}  // namespace buttonInput
