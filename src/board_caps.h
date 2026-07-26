#pragma once
// Derived per-board capability flags. main.cpp/ui.cpp gate on CAPABILITIES
// (what the board can do), not board names — so adding a board is an env
// define plus a line here, not a hunt through #if BOARD_x soup.
//
// Boards:
//   BOARD_CROWPANEL  CrowPanel Advance 3.5" (dash unit): encoder menu, SD, RTC.
//   (neither)        Elecrow WROVER v2.2 (retired): touch + buttons, no menu.

#if defined(BOARD_CROWPANEL)
#define HAS_KNOB_MENU 1     // rotary encoder input + settings-menu overlay
#else
#define HAS_KNOB_MENU 0
#endif

#if defined(BOARD_CROWPANEL) && !defined(MOCK_OBD)
#define HAS_SD_LOG 1        // microSD CSV logger (SPI SD on HSPI)
#else
#define HAS_SD_LOG 0
#endif

// OTA updates + WiFi provisioning portal (S3 boards: WiFi radio + the 8MB
// dual-app partition layout both envs already flash). Elecrow (retired, 4MB)
// stays out.
#if defined(BOARD_CROWPANEL)
#define HAS_OTA 1
#else
#define HAS_OTA 0
#endif

// Hardware presence (menu-row gating; independent of whether a feature is
// compiled — e.g. crowpanel's mock env hides no rows so bench == truck menu).
#if defined(BOARD_CROWPANEL)
#define HAS_RTC     1   // PCF8563 + coin cell
#define HAS_SD_SLOT 1   // SPI microSD
#else
#define HAS_RTC     0   // elecrow: n/a (no menu)
#define HAS_SD_SLOT 0   // elecrow: n/a (no menu)
#endif
