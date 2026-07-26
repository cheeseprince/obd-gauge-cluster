#include "encoder_logic.h"

int32_t EncoderLogic::rotation(int32_t absCount) {
  if (!haveCount_) { lastCount_ = absCount; haveCount_ = true; return 0; }
  int32_t d = absCount - lastCount_;
  lastCount_ = absCount;
  return d;
}

EncEvent EncoderLogic::button(bool pressed, uint32_t nowMs) {
  if (pressed && !down_) {                 // press edge
    down_ = true; downMs_ = nowMs; longFired_ = false;
    return EncEvent::None;
  }
  if (pressed && down_) {                   // held
    if (!longFired_ && (nowMs - downMs_) >= LONG_MS) { longFired_ = true; return EncEvent::PressLong; }
    return EncEvent::None;
  }
  if (!pressed && down_) {                   // release edge
    down_ = false;
    return longFired_ ? EncEvent::None : EncEvent::PressShort;
  }
  return EncEvent::None;                      // idle
}
