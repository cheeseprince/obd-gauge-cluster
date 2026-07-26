#include "real_obd_source.h"

// The mock build (MOCK_OBD=1) and the BLE build (BLE_OBD) both use a parse-only
// stub from the header; this translation unit only carries the classic-BT impl.
#if !defined(MOCK_OBD) && !defined(BLE_OBD)

#include <cstdlib>
#include <string>
#include <vector>
#include "obd_parse.h"
#include "pid_decode.h"
#include "vehicle_active.h"
#include "vin_read.h"

// Enforce that the values_ backing array covers every READOUT row.
// STAT_COUNT (constexpr) is used as the array size; READOUT_COUNT (extern const)
// must equal it — checked here at compile time in the translation unit that can see both.
static_assert(STAT_COUNT == 33, "STAT_COUNT must equal READOUT_COUNT=33 (31 + OilP + Gear helper); update values_ size if table grows");

// ---------------------------------------------------------------------------
// SSP auto-confirm — file-scope state
// ---------------------------------------------------------------------------
//
// The onConfirmRequest callback must be a free function (C-style), not a member.
// We store a pointer to the single BluetoothSerial instance so the callback can
// call confirmReply() without capturing a class reference.
static BluetoothSerial* g_bt = nullptr;

// SSP numeric-comparison: the ESP32 is headless (no keypad), so we always
// auto-accept.  The passkey is logged for debugging at the truck.
static void onConfirmRequest(uint32_t numVal) {
  Serial.printf("[OBD] SSP confirm %u -> auto-yes\n", numVal);
  if (g_bt) g_bt->confirmReply(true);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void RealObdSource::begin() {
  sched_.build();     // build the query schedule from the now-selected profile
  cur_.linkUp = false;
  economy_.reset();   // fresh trip average each key-on

  // Seed baro with sea-level standard (101.3 kPa) so boost's MAP-minus-baro
  // math reads ~0 at idle from the very first connect, before the rare baro
  // query has run. Without this, values_[IDX_BARO] starts at 0 and boost reads
  // ~15 psi high for the first few query cycles. (Restores pre-refactor default.)
  values_[IDX_BARO] = 101.3f;

  // Open NVS namespace "obd" in read-write mode.
  nvs_.begin("obd", false);

  // Store the instance pointer so the file-scope SSP callback can reach it.
  g_bt = &bt_;

  // Enable Secure Simple Pairing so the vLinker MS can pair without a PIN
  // entry on the ESP32 side.  Must be called before bt_.begin().
  bt_.enableSSP();

  // Register the auto-confirm callback before bt_.begin() so it is in place
  // by the time the stack is live.
  bt_.onConfirmRequest(onConfirmRequest);

  // Start Bluetooth in master (isMaster=true) mode so we can initiate connects.
  bt_.begin("GMC-OBD", /*isMaster=*/true);

  if (loadMac()) conn_ = Conn::Connecting;
  else           conn_ = Conn::Unpaired;
}

void RealObdSource::poll(uint32_t nowMs) {
  // Yield the console to the pairing flow while it's active (Task 4).
  if (pairing_) return;

  // Forget — marshalled from core 1 ('f' key / menu); runs here on core 0 so it
  // can't free addr_'s String buffer under tryConnect()'s sscanf or write NVS
  // concurrently with doPairing().
  if (forgetReq_) {
    forgetReq_ = false;
    nvs_.remove("addr");
    nvs_.remove("name");
    addr_ = "";
    if (bt_.connected()) bt_.disconnect();
    conn_ = Conn::Unpaired;
    vinRead_ = false;   // re-read VIN on the next successful connect
    portENTER_CRITICAL(&mux_);
    cur_.linkUp = false;
    portEXIT_CRITICAL(&mux_);
    Serial.println("[OBD] adapter forgotten");
    return;
  }

  // Reset trip — marshalled from core 1 (menu); economy_ is core-0-only.
  if (resetTripReq_) { resetTripReq_ = false; economy_.reset(); }

  // Service a pending pair request first.
  if (pairReq_) {
    pairReq_ = false;
    doPairing();
    return;
  }

  switch (conn_) {
    case Conn::Unpaired:
      // Nothing to do until the user pairs an adapter.
      return;

    case Conn::Connecting:
      tryConnect(nowMs);
      return;

    case Conn::Up:
      // Check for link drop.
      if (!bt_.connected()) {
        conn_ = Conn::Connecting;
        vinRead_ = false;   // re-read VIN on the next successful connect
        portENTER_CRITICAL(&mux_);
        cur_.linkUp = false;
        portEXIT_CRITICAL(&mux_);
        Serial.println("[OBD] link lost — will retry");
        return;
      }
      // Run one step of the non-blocking PID query loop.
      pollQuery(nowMs);
      // Derived rows (economy/HP/fill) — shared engine, one impl for all sources.
      // (This build previously had NO computed block, so its 8 computed tiles
      // read "--" forever — the drift cost of the old three-way copy/paste.)
      updateComputedReadouts(economy_, values_, cur_, mux_, nowMs);
      return;
  }
}

ObdReadings RealObdSource::latest() const {
  // Snapshot cur_ under the spinlock so the render core sees a consistent
  // struct even if poll() is simultaneously writing on core 0. Returned BY
  // VALUE: the old function-static snapshot was itself a cross-core data race
  // when both cores called latest() concurrently.
  ObdReadings snap;
  portENTER_CRITICAL(&mux_);
  snap = cur_;
  portEXIT_CRITICAL(&mux_);
  return snap;
}

// ---------------------------------------------------------------------------
// Private — connection helpers
// ---------------------------------------------------------------------------

bool RealObdSource::loadMac() {
  addr_ = nvs_.getString("addr", "");
  return addr_.length() > 0;
}

void RealObdSource::tryConnect(uint32_t nowMs) {
  // Respect the retry backoff (wrap-safe signed delta) to avoid hammering the BT stack.
  if ((int32_t)(nowMs - nextRetryMs_) < 0) return;
  nextRetryMs_ = nowMs + 3000;  // 3-second retry window

  // Parse "AA:BB:CC:DD:EE:FF" into 6 raw bytes for bt_.connect().
  uint8_t mac[6];
  if (sscanf(addr_.c_str(),
             "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
    Serial.printf("[OBD] bad MAC in NVS: '%s' — clearing\n", addr_.c_str());
    conn_ = Conn::Unpaired;
    return;
  }

  Serial.printf("[OBD] connecting to %s …\n", addr_.c_str());

  // connect(mac, channel=1, SEC_NONE, role=MASTER): hardcode RFCOMM channel 1 to
  // SKIP the SDP service discovery (the "ESP_SPP_DISCOVERY_COMP_EVT failed
  // status:2" path), and SEC_NONE because clone ELM327 dongles reject the default
  // ESP_SPP_SEC_ENCRYPT|AUTHENTICATE security. Researched fix — default
  // connect(mac) hit both traps. Blocks until connect-or-timeout (no timeout arg).
  if (bt_.connect(mac, 1, ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER)) {
    delay(1000);  // let the link settle before ELM327 chatter (clones need this)
    // ELM327 AT handshake (ATZ / ATE0 / ATH1 / etc.) over the connected stream.
    // elm_.begin(Stream&, debug, timeout_ms) — matches installed ELMduino 3.4.x.
    if (elm_.begin(bt_, /*debug=*/false, /*timeout=*/2000)) {
      conn_ = Conn::Up;
      // Fresh ELM after its init/reset: the adapter's AT SH header is back at the
      // default, so force the query engine to re-issue it and restart from Idle
      // (same invariant the BLE source re-establishes on reconnect).
      q_.txState = Tx::Idle; q_.curHeader = -1;
      portENTER_CRITICAL(&mux_);
      cur_.linkUp = true;
      portEXIT_CRITICAL(&mux_);
      Serial.println("[OBD] connected and ELM327 handshake OK");

      // One-time-per-session VIN read (standard ATSH7E0 / Mode-09 0902 — never
      // the profile's enhanced addressing). Runs once per connect; vinRead_ is
      // cleared on disconnect (forget + link-lost, above) so a reconnect re-reads.
      if (!vinRead_) {
        QueryIo io{this};
        char vin[18] = "";
        // Bounded retry: a garbled/timed-out 0902 on the FIRST cold-connect read
        // used to latch vinRead_ and never re-read until a full reconnect — the
        // observed "stuck on Generic" field bug. Retry a few times before latching.
        bool ok = false;
        for (int attempt = 0; attempt < VIN_READ_ATTEMPTS && !ok; attempt++) {
          if (attempt) delay(150);
          ok = readVinOverIo(io, vin);
        }
        if (ok) {
          portENTER_CRITICAL(&mux_);
          strncpy(cur_.vin, vin, sizeof cur_.vin - 1); cur_.vin[sizeof cur_.vin - 1] = '\0';
          portEXIT_CRITICAL(&mux_);
          Serial.printf("[VIN] read OK (WMI %.3s)\n", vin);   // WMI only — never log the full VIN
        } else {
          Serial.println("[VIN] read failed after retries — auto-detect re-tries on next reconnect");
        }
        vinRead_ = true;
        q_.curHeader = -1;   // VIN read left ATSH at 7E0; force re-issue for the profile header
      }
      return;
    }
    Serial.println("[OBD] ELM327 handshake failed");
  } else {
    Serial.println("[OBD] BT connect failed");
  }

  // Either bt_.connect() or elm_.begin() failed; stay in Connecting for retry.
  conn_ = Conn::Connecting;
  portENTER_CRITICAL(&mux_);
  cur_.linkUp = false;
  portEXIT_CRITICAL(&mux_);
}

// ---------------------------------------------------------------------------
// Public pairing hooks (console control — implemented in Task 4)
// ---------------------------------------------------------------------------

void RealObdSource::requestPair() {
  pairReq_ = true;  // poll() will call doPairing() on the next tick
}

void RealObdSource::forget() {
  // Called from core 1 ('f' key / menu). Only sets a flag — the actual NVS wipe
  // and addr_/conn_ reset run in poll() on core 0 (see the forgetReq_ handler),
  // so we never free addr_'s String buffer while tryConnect() is parsing it.
  forgetReq_ = true;
}

bool RealObdSource::pairing() const {
  return pairing_;
}

// ---------------------------------------------------------------------------
// Pairing flow (Task 4)
// ---------------------------------------------------------------------------

void RealObdSource::doPairing() {
  // Signals the main loop to hand the console to us for the duration.
  pairing_ = true;

  // ── Scan ──────────────────────────────────────────────────────────────────
  Serial.println("[OBD] scanning for adapters (~10s)...");
  // discover() takes milliseconds (int); blocks until timeout or stack finds
  // no more devices.  Returns nullptr on stack error.
  BTScanResults* res = bt_.discover(10000);
  if (!res || res->getCount() == 0) {
    Serial.println("[OBD] no devices found; press p to retry");
    pairing_ = false;
    return;
  }

  // ── List ──────────────────────────────────────────────────────────────────
  int n = res->getCount();
  for (int i = 0; i < n; i++) {
    BTAdvertisedDevice* d = res->getDevice(i);
    // getName() → std::string; getAddress().toString() → Arduino String.
    Serial.printf("  [%d] %s  %s\n", i,
                  d->getName().c_str(),
                  d->getAddress().toString().c_str());
  }
  Serial.println("[OBD] type the number of the adapter, then Enter (30s timeout):");

  // ── Selection read ────────────────────────────────────────────────────────
  // Block here — pairing_ is set so the main loop cedes the console.
  // Accumulate digit characters until newline or 30-second wall timeout.
  String line;
  uint32_t deadline = millis() + 30000;
  bool gotNewline = false;
  while (!gotNewline && (int32_t)(millis() - deadline) < 0) {   // wrap-safe
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        gotNewline = true;
        break;
      }
      if (c >= '0' && c <= '9') line += c;
    }
    if (!gotNewline) delay(10);
  }

  // ── Validate ──────────────────────────────────────────────────────────────
  int sel = (line.length() > 0) ? line.toInt() : -1;
  if (sel < 0 || sel >= n) {
    Serial.println("[OBD] cancelled (invalid selection or timeout)");
    pairing_ = false;
    return;
  }

  // ── Commit ────────────────────────────────────────────────────────────────
  BTAdvertisedDevice* chosen = res->getDevice(sel);
  // BTAddress::toString() returns an Arduino String; copy to addr_ directly.
  addr_ = chosen->getAddress().toString();
  nvs_.putString("addr", addr_);
  // getName() returns std::string; .c_str() gives a const char* Preferences can use.
  nvs_.putString("name", chosen->getName().c_str());
  Serial.printf("[OBD] saved %s (%s); connecting...\n",
                addr_.c_str(), chosen->getName().c_str());

  // Kick off the connection state machine.
  conn_        = Conn::Connecting;
  nextRetryMs_ = 0;  // connect on next poll() tick without waiting for backoff
  pairing_     = false;
}

// ---------------------------------------------------------------------------
// Task 5: PID query — shared engine (obd_query.h) over the BT serial stream.
// ---------------------------------------------------------------------------
void RealObdSource::pollQuery(uint32_t nowMs) {
  QueryIo io{this};
  pidQueryStep(q_, sched_, io, nowMs, /*replyTimeoutMs=*/250, values_, cur_, mux_);
}

#endif  // MOCK_OBD
