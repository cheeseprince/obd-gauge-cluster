#pragma once
#include "board_caps.h"

// ota_portal.h — phone-provisioned WiFi credential setup (menu "WiFi setup").
//
// Raises a SoftAP (SSID OBD-XXXX, where XXXX is 4 hex from the chip MAC;
// password random per device, NVS-persisted, shown on the dash screen) +
// captive portal at
// http://192.168.4.1: the page lists locally scanned SSIDs and the saved
// credential list (add / delete, stored in NVS via wifi_cred_store). Blocking;
// `pump(status)` is called continuously so the caller can keep LVGL alive and
// show connection info on-screen. Returns when the user taps "Done" on the
// page or after a 5-minute timeout — the caller is expected to reboot.
void otaPortalRun(void (*pump)(const char* status));
