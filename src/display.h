// TFT_eSPI + LVGL bring-up for the Elecrow 3.5" (ILI9488 + XPT2046). Registers
// the flush callback and a calibrated touch pointer indev. Call begin() once
// before building any LVGL UI.
#pragma once
#include <cstdint>
namespace display {
void begin();                    // tft.init, LVGL init, flush + touch indev
void setBacklight(uint8_t pct); // 0..100 PWM on GPIO27 (day/night dimming)
void tick();                     // call often: lv_timer_handler()
void calibrateTouch();           // runs TFT_eSPI corner-tap calibration, prints the 5-value array to serial, applies it for the session
// Latest touch state (calibrated screen coords), cached by the indev read.
// The app does its own tap/swipe detection from this — LVGL's gesture/click
// engine is unreliable on this resistive panel. Returns true while pressed;
// on release, x/y hold the last pressed point.
bool touch(int16_t* x, int16_t* y);
}  // namespace display
