#include "ota_update.h"

#if HAS_OTA

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <cstring>
#include "wifi_cred_store.h"
#include "ota_common.h"
#include "fw_git.h"
#include "ota_pubkey.h"
#include "ntp_time.h"

// Release channel (overridable per-env via build flags).
#ifndef OTA_BASE_URL
#define OTA_BASE_URL "https://cheeseprince.github.io/obd-gauge-cluster/"
#endif
#ifndef OTA_ENV
#define OTA_ENV "unknown"
#endif

// Mozilla root-CA bundle for TLS certificate validation on the OTA fetch.
// The blob ships inside the prebuilt IDF libs (libmbedtls.a, built with
// CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y); referencing the symbol here makes the
// linker pull it in (~64 KB flash). Arduino core 2.x's setCACertBundle() does
// NOT fall back to it automatically — the pointer must be passed explicitly.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

// A real compiled-in public key is MANDATORY. The old "transition mode"
// (skip verification while OTA_PUBKEY_PEM was still the empty placeholder)
// was a fail-OPEN branch: a bad merge or stripped header would silently
// disable signature enforcement. Assert at build time instead, so that
// failure mode is a compile error, never a weakened device.
static_assert(sizeof(OTA_PUBKEY_PEM) > 64,
              "OTA_PUBKEY_PEM must hold a real EC P-256 public key (see ota_pubkey.h)");

// Show a message and hold it long enough to read (terminal states).
static void say(void (*pump)(const char*), const char* msg, uint32_t holdMs = 4000) {
  Serial.printf("[OTA] %s\n", msg);
  pump(msg);
  uint32_t t0 = millis();
  while ((int32_t)(millis() - (t0 + holdMs)) < 0) { pump(msg); delay(50); }
}

// Download buffer: STATIC, not a loop-task stack local — 4KB of locals plus
// the TLS handshake blew the ~8KB loopTask stack (panic-reboot with nothing
// drawn, truck-observed on the dash's first Check update).
static uint8_t s_buf[4096];

// Verify an ECDSA-P256/SHA-256 signature (DER, from openssl dgst -sign) over
// `manifest`/`mlen` using the compiled-in OTA_PUBKEY_PEM (ota_pubkey.h).
// Returns true iff the signature is valid for that key. A missing or invalid
// signature is fatal to the update: there is no fail-open path, and the
// file-scope static_assert above makes a real compiled-in key a build
// requirement (see otaCheckUpdate).
static bool otaVerifyManifest(const uint8_t* manifest, size_t mlen, const uint8_t* sig, size_t siglen) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  if (mbedtls_pk_parse_public_key(&pk, (const unsigned char*)OTA_PUBKEY_PEM, sizeof(OTA_PUBKEY_PEM)) != 0) {
    mbedtls_pk_free(&pk);
    return false;
  }
  uint8_t h[32];
  mbedtls_sha256(manifest, mlen, h, 0);   // 0 = SHA-256 (not 224)
  int rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, h, sizeof h, sig, siglen);
  mbedtls_pk_free(&pk);
  return rc == 0;
}

// Defined in main.cpp, next to the other stack-instrumentation persist
// helpers. Captures the just-recorded high-water mark (this function's own
// TLS-handshake peak) unconditionally, ignoring the drop threshold and rate
// limit, because otaCheckUpdate()'s SUCCESS path reboots internally a few
// lines below and this is the only moment that reading will ever exist.
// See the call site at the end of this function, and main.cpp's comment on
// otaPersistStackMins() for why the CheckUpdate menu handler does NOT also
// call this on that path (it would be unreachable there anyway).
extern void otaPersistStackMins();

// Join the first stored credential whose SSID is visible. True once connected.
// Parse a "vX.Y.Z" release string into one monotonically-comparable value.
// Non-release strings ("local", "dev-<hash>") parse to 0 (oldest), so a USB/dev
// build still updates to any signed release. Each field is clamped to 10 bits.
static uint32_t parseSemver(const char* v) {
  if (!v) return 0;
  if (*v == 'v' || *v == 'V') v++;
  unsigned f[3] = {0, 0, 0};
  int idx = 0;
  bool sawDigit = false;
  for (; *v && idx < 3; v++) {
    if (*v >= '0' && *v <= '9') {
      f[idx] = f[idx] * 10 + (unsigned)(*v - '0');
      if (f[idx] > 1023) f[idx] = 1023;
      sawDigit = true;
    } else if (*v == '.') {
      idx++;
    } else {
      break;   // stop at any non-version char ("-rc1", etc.)
    }
  }
  return sawDigit ? ((f[0] << 20) | (f[1] << 10) | f[2]) : 0;
}

static bool joinKnownWifi(void (*pump)(const char*), char* ssidOut, size_t ssidN) {
  WifiCredList list;
  credLoad(list);
  if (list.n == 0) { say(pump, "Update: no WiFi saved\nRun 'WiFi setup' first"); return false; }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  pump("Update: scanning WiFi...");
  delay(200);                       // radio settle after mode switch
  int n = WiFi.scanNetworks();
  if (n <= 0) { delay(500); n = WiFi.scanNetworks(); }   // one retry: first scan
                                                          // right after a mode
                                                          // change can fail (-2)
  Serial.printf("[OTA] scan: %d networks, %d saved, heap %u\n",
                n, list.n, (unsigned)ESP.getFreeHeap());

  char failedSsid[33] = "";        // last visible network we couldn't join
  for (int c = 0; c < list.n; c++) {
    bool visible = false;
    for (int i = 0; i < n; i++)
      if (WiFi.SSID(i) == String(list.c[c].ssid)) { visible = true; break; }
    if (!visible) continue;

    char msg[80];
    snprintf(msg, sizeof msg, "Update: joining\n%s ...", list.c[c].ssid);
    pump(msg);
    WiFi.begin(list.c[c].ssid, list.c[c].pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - (t0 + 15000)) < 0) {
      pump(msg);
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(ssidOut, ssidN, "%s", list.c[c].ssid);
      WiFi.scanDelete();
      return true;
    }
    Serial.printf("[OTA] join failed: %s (status %d)\n", list.c[c].ssid, (int)WiFi.status());
    snprintf(failedSsid, sizeof failedSsid, "%s", list.c[c].ssid);
    WiFi.disconnect(true);
  }
  WiFi.scanDelete();
  // Distinguish "not here" from "here but rejected us" — the latter is almost
  // always a mistyped password (fix it by re-adding the SSID in WiFi setup).
  if (failedSsid[0]) {
    char m[96];
    snprintf(m, sizeof m, "Update: %s in range\nbut join FAILED\n(wrong password? re-add\nit in WiFi setup)", failedSsid);
    say(pump, m, 6000);
  } else {
    say(pump, "Update: no saved WiFi\nnetwork in range");
  }
  return false;
}

void otaCheckUpdate(void (*pump)(const char* status), const GeoLocation& geo) {
  char ssid[33];
  if (!joinKnownWifi(pump, ssid, sizeof ssid)) return;

  // Keep the result. NTP is a small UDP exchange over the SAME link the
  // manifest fetch uses, so "the clock synced" is direct evidence that the
  // network works -- which is what tells a bare HTTPC error apart from an
  // unreachable network. Still best-effort: it never blocks the update.
  const bool clockSynced = ntpSyncRtc(geo, pump);

  WiFiClientSecure net;
  // Validate the server's certificate chain against the Mozilla root bundle
  // (defense in depth on top of the manifest signature, which remains the
  // primary control). Chain-of-trust only: the prebuilt libs compile mbedTLS
  // without CONFIG_MBEDTLS_HAVE_TIME_DATE, so validity dates are NOT checked —
  // an expired cert with a valid chain still passes (which is also why this
  // works with no RTC/location set). An on-path attacker without a
  // chain-valid *.github.io cert can neither read nor tamper with the fetch.
  net.setCACertBundle(rootca_crt_bundle_start,
                      (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start));
  HTTPClient http;

  // ── Manifest ──────────────────────────────────────────────────────────────
  pump("Update: checking...");
  Serial.printf("[OTA] joined %s, heap %u — GET manifest\n", ssid, (unsigned)ESP.getFreeHeap());
  if (!http.begin(net, OTA_BASE_URL "manifest.txt")) { say(pump, "Update: bad URL"); return; }
  int code = http.GET();
  Serial.printf("[OTA] manifest HTTP %d, heap %u\n", code, (unsigned)ESP.getFreeHeap());
  if (code != 200) {
    // 160, not 96: the longest branch below is 102 bytes with the code
    // substituted, and snprintf truncates SILENTLY -- a half-written
    // instruction is worse than the terse one this replaced.
    char m[160];
    if (code < 0) {
      // A NEGATIVE code is not an HTTP status -- it is HTTPClient's own error,
      // and it means the connection never opened at all (-1 is
      // HTTPC_ERROR_CONNECTION_REFUSED). Showing the raw number tells the
      // person standing at the vehicle nothing, and it looks like a network
      // fault even when it is not.
      //
      // ⚠️ DO NOT NAME A SINGLE CAUSE HERE. This used to read "Start the
      // engine, then retry", generalised from one truck on 2026-08-05 where
      // accessory power could not sustain the TLS handshake. That is a real
      // cause -- the handshake is by a wide margin the highest-current thing
      // this device does -- but it is not the only one, and the message was
      // wrong on a truck with the engine running (2026-08-15). Confirmed the
      // same day that the firmware and the release are fine: a bench board
      // running this exact v0.4.0 image updated to v0.4.4 over the air.
      //
      // Anything that stops the TLS connection OPENING lands here: a supply
      // that sags on the handshake burst (a current-limited USB port does this
      // with the engine running), a link too weak or lossy for a multi-round
      // -trip handshake, or an access point with no route out.
      //
      // So report what is known and let the reader choose. `clockSynced` is
      // the useful discriminator: NTP is a single small UDP exchange over the
      // same link, so if it worked, the network is up and the problem is
      // specific to the handshake.
      if (clockSynced) {
        snprintf(m, sizeof m,
                 "Update: server unreachable\nWiFi OK (clock synced)\n"
                 "Try another USB supply, or\nmove nearer WiFi (net %d)", code);
      } else {
        snprintf(m, sizeof m,
                 "Update: no route out\nJoined WiFi but no traffic\n"
                 "Check that network (net %d)", code);
      }
    } else {
      snprintf(m, sizeof m, "Update: manifest\nHTTP %d", code);
    }
    Serial.printf("[OTA] manifest fetch failed, code %d, clockSynced=%d, heap %u%s\n",
                  code, (int)clockSynced, (unsigned)ESP.getFreeHeap(),
                  code < 0 ? "  (connection never opened)" : "");
    http.end(); say(pump, m); return;
  }
  String manifest = http.getString();
  http.end();

  // ── Manifest signature (ECDSA-P256/SHA-256 over the manifest.txt bytes) ──
  bool haveSig = false;
  size_t sigLen = 0;
  static uint8_t sigBuf[128];   // ECDSA-P256 DER sig is ~70-72 bytes; generous margin
  if (http.begin(net, OTA_BASE_URL "manifest.sig")) {
    int sigCode = http.GET();
    if (sigCode == 200) {
      int sz = http.getSize();
      if (sz > 0 && (size_t)sz <= sizeof(sigBuf)) {
        WiFiClient* s = http.getStreamPtr();
        size_t got = 0;
        uint32_t t0 = millis();
        while (got < (size_t)sz && (int32_t)(millis() - (t0 + 5000)) < 0) {
          size_t avail = s->available();
          if (!avail) { delay(5); continue; }
          int r = s->readBytes(sigBuf + got, (size_t)(avail > (sizeof(sigBuf) - got) ? (sizeof(sigBuf) - got) : avail));
          if (r <= 0) break;
          got += (size_t)r;
        }
        if (got == (size_t)sz) { sigLen = got; haveSig = true; }
      }
    }
    http.end();
  }

  // Signature is mandatory (see the file-scope OTA_PUBKEY_PEM static_assert).
  if (!haveSig || sigLen < 8) {
    Serial.println("[OTA] manifest.sig missing/short — refusing to trust manifest");
    say(pump, "Update: signature invalid"); return;
  } else if (!otaVerifyManifest((const uint8_t*)manifest.c_str(), manifest.length(), sigBuf, sigLen)) {
    Serial.println("[OTA] manifest signature FAILED verification");
    say(pump, "Update: signature invalid"); return;
  }

  OtaRelease rel;
  if (!findRelease(manifest.c_str(), OTA_ENV, rel)) {
    say(pump, "Update: no release\nfor this device yet"); return;
  }
  // Install only a STRICTLY NEWER release. Equal => up to date; OLDER => refuse,
  // which blocks a rollback/downgrade attack (a replayed old-but-validly-signed
  // manifest can no longer push the device backwards). A "local"/"dev-*" current
  // version parses to 0, so a USB/dev build still takes any release.
  if (parseSemver(rel.version) <= parseSemver(FW_VERSION)) {
    char m[64]; snprintf(m, sizeof m, "Up to date\n(%s)", FW_VERSION);
    say(pump, m); return;
  }

  // ── Download into the spare OTA slot, hashing as we stream ───────────────
  char url[160];
  snprintf(url, sizeof url, "%s%s", OTA_BASE_URL, rel.bin);
  if (!http.begin(net, url)) { say(pump, "Update: bad bin URL"); return; }
  code = http.GET();
  if (code != 200 || (uint32_t)http.getSize() != rel.size) {
    char m[64]; snprintf(m, sizeof m, "Update: bin fetch\nHTTP %d", code);
    http.end(); say(pump, m); return;
  }
  if (!Update.begin(rel.size)) { http.end(); say(pump, "Update: no room\nin OTA slot"); return; }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);          // 0 = SHA-256 (not 224)

  WiFiClient* stream = http.getStreamPtr();
  uint32_t got = 0, lastPct = 101;
  while (got < rel.size && http.connected()) {
    size_t avail = stream->available();
    if (!avail) { delay(5); continue; }
    int r = stream->readBytes(s_buf, avail > sizeof s_buf ? sizeof s_buf : avail);
    if (r <= 0) break;
    if (Update.write(s_buf, r) != (size_t)r) break;
    mbedtls_sha256_update(&sha, s_buf, r);
    got += r;
    uint32_t pct = got * 100u / rel.size;
    if (pct != lastPct) {                  // repaint only on % change
      lastPct = pct;
      char m[64]; snprintf(m, sizeof m, "Updating to %s\n%u%%", rel.version, (unsigned)pct);
      pump(m);
    }
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);

  // ── Verify BEFORE activating — a bad/truncated image never gets booted ───
  if (got != rel.size || strcasecmp(hex, rel.sha256) != 0) {
    Update.abort();
    say(pump, "Update FAILED\n(bad download - kept\ncurrent firmware)");
    return;
  }
  if (!Update.end(true) || !Update.isFinished()) {
    say(pump, "Update FAILED\n(flash error - kept\ncurrent firmware)");
    return;
  }

  say(pump, "Update OK - rebooting", 1500);
  // Capture the stack high-water mark BEFORE the restart below destroys it.
  // The force-persist block in the CheckUpdate menu handler (main.cpp) sits
  // AFTER this call and never runs on this path — ESP.restart() does not
  // return — so this is the only place the SUCCESS-path measurement is ever
  // taken. Every other exit from this function (no WiFi, join failed, up to
  // date, download failed) returns normally instead, and the caller's
  // force-persist block covers those via the same otaPersistStackMins().
  otaPersistStackMins();
  ESP.restart();
}

#endif  // HAS_OTA
