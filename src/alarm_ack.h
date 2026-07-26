#pragma once

// Acknowledgement state for the full-screen alarm overlay, so an encoder press
// can dismiss it. Pure logic (no LVGL/Arduino) so it is host-testable.
//
// Rule: while an alarm is active the overlay shows until ack() is called; after
// an ack it stays hidden until ALL alarms clear (anyActive becomes false), which
// re-arms it so a fresh alarm shows again.
class AlarmAck {
 public:
  // Call once per frame with whether any alarm is currently active.
  // Returns true if the overlay should be shown this frame.
  bool shouldShow(bool anyActive) {
    if (!anyActive) { acked_ = false; return false; }   // all clear -> re-arm
    return !acked_;
  }
  void ack() { acked_ = true; }   // dismiss the currently-shown alarm

 private:
  // volatile: ack() runs on the core-0 I/O task while shouldShow() runs on the
  // core-1 render task — matches the codebase convention for cross-core flags.
  volatile bool acked_ = false;
};
