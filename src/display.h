// Display device interface: LVGL bring-up and backlight control.
//
// One implementation ships today — display_lovyangfx.cpp, for the CrowPanel
// Advance 3.5" (LovyanGFX). The interface is kept separate from it so a second
// panel can be added without touching the UI layer; the Elecrow ILI9488/TFT_eSPI
// implementation that used to sit behind it was removed with that board.
//
// Call begin() once before building any LVGL UI.
#pragma once
#include <cstdint>
namespace display {
void begin();                    // tft.init, LVGL init, flush + touch indev
void setBacklight(uint8_t pct); // 0..100 PWM on GPIO27 (day/night dimming)
void tick();                     // call often: lv_timer_handler()
// Latest touch state (calibrated screen coords), cached by the indev read.
// The app does its own tap/swipe detection from this — LVGL's gesture/click
// engine is unreliable on this resistive panel. Returns true while pressed;
// on release, x/y hold the last pressed point.
bool touch(int16_t* x, int16_t* y);
}  // namespace display
