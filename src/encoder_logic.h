#pragma once
#include <cstdint>

// Pure encoder event logic — no hardware. The device layer (encoder_input, Plan 2)
// reads the Modulino's absolute count + button over I2C and feeds them here.
enum class EncEvent : uint8_t { None, PressShort, PressLong };

class EncoderLogic {
 public:
  static const uint32_t LONG_MS = 600;
  // Net detents since the previous call (+ = next/CW, - = prev/CCW). The first
  // call seeds the reference and returns 0 (no phantom jump on boot).
  int32_t rotation(int32_t absCount);
  // PressLong fires once when continuously held >= LONG_MS; PressShort fires on
  // release if no long press fired; None otherwise.
  EncEvent button(bool pressed, uint32_t nowMs);
 private:
  int32_t lastCount_ = 0; bool haveCount_ = false;
  bool down_ = false; uint32_t downMs_ = 0; bool longFired_ = false;
};
