#include <Arduino.h>
#include <Wire.h>
#include "button_input.h"
#include "button_nav.h"

// I2C pin assignments — Crowtail I2C connector on the Elecrow 3.5" ESP32 board.
// Do NOT rely on Wire defaults: the Elecrow board routes SDA to GPIO22, SCL to GPIO21.
static constexpr int SDA_PIN = 22;
static constexpr int SCL_PIN = 21;

// PCF8574T address range: 0x20–0x27 (A0/A1/A2 tied to GND gives 0x20).
// PCF8574A address range: 0x38–0x3F (same pin wiring gives 0x38).
// begin() scans both ranges and uses the first device found.
static constexpr uint8_t PCF8574T_BASE  = 0x20;
static constexpr uint8_t PCF8574A_BASE  = 0x38;
static constexpr uint8_t PCF8574_RANGE  = 8;  // 8 addresses in each family

// File-scope state shared between begin() and update().
static ButtonNav btnNav;       // pure-logic debounce + nav driver
static uint8_t   expAddr  = 0; // I2C address of the found expander (0 = none)
static bool      present_ = false;

// Cross-core spinlock for the shared NavState (button task on core 0 vs. render
// on core 1). Held only around the fast ButtonNav state mutation, never I2C.
static portMUX_TYPE navMux = portMUX_INITIALIZER_UNLOCKED;

// Physical wiring -> logical button index. Maps which PCF8574 pin each bezel
// button is soldered to onto the logical index button_nav expects
// (BTN_TL=0, TR=1, BL=2, BR=3, LEFT=4, RIGHT=5). Our build (2026-06-22):
//   P0=top-right, P1=top-left, P2=left-edge, P3=bottom-left,
//   P4=bottom-right, P5=right-edge.
static const uint8_t PIN_TO_LOGICAL[6] = {
  BTN_TR,    // P0 -> top-right    (zoom Oil)
  BTN_TL,    // P1 -> top-left     (zoom Trans)
  BTN_LEFT,  // P2 -> left edge    (page prev)
  BTN_BL,    // P3 -> bottom-left  (zoom Boost)
  BTN_BR,    // P4 -> bottom-right (zoom Coolant)
  BTN_RIGHT, // P5 -> right edge   (page next)
};

namespace buttonInput {

// Scan both PCF8574 address ranges and configure the first expander found.
void begin() {
  Wire.begin(SDA_PIN, SCL_PIN);

  // Scan PCF8574T (0x20..0x27) then PCF8574A (0x38..0x3F).
  for (int base : {(int)PCF8574T_BASE, (int)PCF8574A_BASE}) {
    for (int offset = 0; offset < PCF8574_RANGE; offset++) {
      uint8_t addr = (uint8_t)(base + offset);
      Wire.beginTransmission(addr);
      uint8_t err = Wire.endTransmission();
      if (err == 0) {
        Serial.printf("[I2C] device 0x%02X\n", addr);
        if (!present_) {
          // Use the first found device as the button expander.
          expAddr  = addr;
          present_ = true;
        }
      }
    }
  }

  if (present_) {
    // Write 0xFF so all 8 pins are configured as inputs.
    // PCF8574 quasi-bidirectional I/O: writing 1 enables the weak pull-up;
    // an externally pulled-low pin (button pressed) then reads back as 0.
    Wire.beginTransmission(expAddr);
    Wire.write(0xFF);
    Wire.endTransmission();
    Serial.printf("[buttonInput] PCF8574 at 0x%02X — %d buttons active\n", expAddr, BTN_COUNT);
  } else {
    Serial.println("[buttonInput] WARNING: no PCF8574 expander found — physical buttons disabled");
  }
}

// Read the expander, invert active-low logic, and drive ButtonNav.
void update(NavState& s) {
  if (!present_) return;

  Wire.requestFrom(expAddr, (uint8_t)1);
  if (!Wire.available()) return;

  uint8_t raw = Wire.read();

  // Buttons are wired active-low (pressed = GPIO LOW). Invert + mask to P0..P5.
  uint8_t physical = (uint8_t)(~raw) & 0x3F;

  // Remap physical pins to logical button indices per the bezel wiring.
  uint8_t logical = 0;
  for (int p = 0; p < 6; p++)
    if (physical & (1u << p)) logical |= (uint8_t)(1u << PIN_TO_LOGICAL[p]);

  // Guard only the NavState mutation (the I2C read above is lock-free).
  taskENTER_CRITICAL(&navMux);
  btnNav.update(logical, s);
  taskEXIT_CRITICAL(&navMux);
}

bool present() { return present_; }

void lockNav()   { taskENTER_CRITICAL(&navMux); }
void unlockNav() { taskEXIT_CRITICAL(&navMux); }

}  // namespace buttonInput
