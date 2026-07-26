#pragma once
#include "board_caps.h"
#include "solar.h"

// ota_update.h — menu-triggered firmware update ("Check update").
//
// Joins the first stored+visible WiFi network (wifi_cred_store), fetches
// manifest.txt (and manifest.sig) from the GitHub Pages release channel over
// HTTPS, and — if the release version differs from the running FW_VERSION —
// streams the bin into the spare OTA slot with a running SHA-256, verifying
// against the manifest hash BEFORE the slot is activated. Reboots itself on
// success; on any failure it shows the reason via pump() and returns
// (running slot untouched — the caller reboots to restore normal operation).
//
// TLS note: the fetch validates the server certificate chain against the
// Mozilla root-CA bundle embedded in the IDF libs (setCACertBundle). The
// manifest signature stays the primary integrity/authenticity control; TLS
// validation is defense in depth against an on-path attacker.
//
// Manifest authenticity: the manifest must carry a valid ECDSA-P256/SHA-256
// signature (manifest.sig) verifying against ota_pubkey.h's compiled-in
// OTA_PUBKEY_PEM, or the update is refused outright. A real key is mandatory
// (build-time assert in ota_update.cpp) — there is no unsigned fallback.
//
// HAS_OTA=0 boards (retired elecrow) get a no-op stub.

#if HAS_OTA
void otaCheckUpdate(void (*pump)(const char* status), const GeoLocation& geo);
#else
inline void otaCheckUpdate(void (*)(const char*), const GeoLocation&) {}
#endif
