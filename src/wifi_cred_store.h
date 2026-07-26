#pragma once
#include <cstdint>

// wifi_cred_store.h — small list of WiFi credentials for the OTA/update flows.
// Provisioned from a phone via the captive portal (ota_portal) and stored ONLY
// in NVS — never compiled into the (publicly hosted) firmware binaries.
//
// Pure list model (host-tested in test/test_wifi_creds.cpp); NVS load/save are
// device-only (defined under ARDUINO in the .cpp).

constexpr int WIFI_CRED_MAX = 5;

struct WifiCred {
  char ssid[33];   // 802.11 max 32 + NUL
  char pass[65];   // WPA2 max 64 + NUL ("" = open network)
};

struct WifiCredList {
  WifiCred c[WIFI_CRED_MAX];
  int n = 0;
};

// Add (or update — same SSID replaces the stored password). Returns false only
// when the list is full AND the SSID is new. Overlong ssid/pass are truncated.
bool credAdd(WifiCredList& l, const char* ssid, const char* pass);

// Remove by index; compacts the list. False if idx out of range.
bool credRemove(WifiCredList& l, int idx);

// Index of ssid in the list, -1 if absent.
int credFind(const WifiCredList& l, const char* ssid);

// NVS persistence (namespace "wifi"). Device-only.
void credLoad(WifiCredList& l);
void credSave(const WifiCredList& l);
