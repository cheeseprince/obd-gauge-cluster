#pragma once
// Derived per-board capability flags. main.cpp/ui.cpp gate on CAPABILITIES
// (what the board can do), not board names — so adding a board is an env
// define plus a line here, not a hunt through #if BOARD_x soup.
//
// Boards:
//   BOARD_CROWPANEL  CrowPanel Advance 3.5" (dash unit): encoder menu, SD, RTC.

#define HAS_KNOB_MENU 1     // rotary encoder input + settings-menu overlay

// Still conditional: the mock env builds the same board without a live OBD
// source, and the CSV logger has nothing to log.
#if !defined(MOCK_OBD)
#define HAS_SD_LOG 1        // microSD CSV logger (SPI SD on HSPI)
#else
#define HAS_SD_LOG 0
#endif

// OTA updates + WiFi provisioning portal (S3 boards: WiFi radio + the 8MB
// dual-app partition layout the board flashes).
#define HAS_OTA 1

// Hardware presence (menu-row gating; independent of whether a feature is
// compiled — e.g. crowpanel's mock env hides no rows so bench == truck menu).
#define HAS_RTC     1   // PCF8563 + coin cell
#define HAS_SD_SLOT 1   // SPI microSD
