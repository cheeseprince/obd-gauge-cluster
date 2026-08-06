#include "ble_obd_source.h"

#if defined(BLE_OBD) && !defined(MOCK_OBD)

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "obd_parse.h"
#include "pid_decode.h"
#include "wdt_kick.h"
#include "ble_rank.h"   // pure, host-tested connect-order ranking
#include "vin_read.h"

static_assert(STAT_COUNT == 33, "STAT_COUNT must equal READOUT_COUNT=33 (31 + OilP + Gear helper)");

// vLinker MS BLE GATT (captured on the truck).
static const uint16_t SVC_UUID    = 0x18f0;
static const uint16_t NOTIFY_UUID = 0x2af0;
static const uint16_t WRITE_UUID  = 0x2af1;

static BleObdSource* g_self = nullptr;   // for the notify/security free callbacks

// BRING-UP: on-screen BLE step beacon. main.cpp's connecting-status overlay
// draws g_bleStep so we can see WHERE connect fails (scan/connect/secure/gatt/sub).
volatile char g_bleStep[48] = "boot";
static void bleStep(const char* s) { strncpy((char*)g_bleStep, s, 47); g_bleStep[47] = 0; }
// On-screen list of BLE devices seen in the last scan (strongest first): shows
// whether the vLinker is even advertising. name (or short MAC) + RSSI per line.
//
// CROSS-CORE, DELIBERATELY UNSYNCHRONISED. Written by the scan on core 0 (see the
// snprintf below) and read by the UI on core 1, with no lock and — unlike
// g_bleStep above — no `volatile`. That is intentional, not an oversight:
//   * the only failure mode is a torn read, i.e. one frame of the scan overlay
//     showing a mix of the old and new list. It self-corrects on the next frame.
//   * it cannot run off the end: the buffer starts NUL-terminated and every
//     writer is a bounded snprintf into the same array, so a reader always finds
//     a terminator within `sizeof g_bleScan`.
// A lock here would mean the render core blocking on the BLE scan for cosmetic
// text. If this buffer ever feeds anything but a debug overlay, that trade stops
// being acceptable — add the guard then.
char g_bleScan[240] = "(no scan yet)";

// Pairing policy: the adapter is headless with a fixed, publicly-known 123456
// PIN, and this side has no human to compare a passkey — so bonding is PINNED
// to the stored adapter instead of blanket auto-confirmed. Pairing completes
// only for the cached adapter address, or for any device while NO adapter is
// cached (first setup / right after "Forget adapter" — a deliberate,
// user-initiated trust-on-first-use window). Anything else is refused, so a
// nearby clone can't bond and feed fabricated readings mid-drive. Documented
// in SECURITY.md ("BLE adapter trust model"). The security-callback API
// differs between NimBLE 1.4.x and 2.x, so both are compiled per
// NIMBLE_CPP_VERSION_MAJOR (defined by NimBLEDevice.h in 2.x).
// Wrong-on-purpose passkey for a disallowed peer — SMP then fails
// cryptographically (there is no explicit "reject" injection for the
// display/entry roles). Must be RANDOM, not a fixed wrong value: this file is
// public source, and in these roles the PEER chooses/observes its side of the
// passkey, so an adaptive attacker would just use a fixed value from the
// source and bond every time. Random caps them at ~1e-6 per attempt; nudged
// off 123456 so it can never accidentally equal the real PIN and allow.
static uint32_t wrongPasskey() {
  uint32_t k = esp_random() % 1000000;
  return k == 123456 ? 123457 : k;
}

class BleSecCB : public NimBLEClientCallbacks {
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
  uint32_t onPassKeyDisplay(NimBLEConnInfo&) override {
    return g_self && g_self->currentPeerAllowed() ? 123456 : wrongPasskey();
  }
  void onPassKeyEntry(NimBLEConnInfo& ci) override {
    NimBLEDevice::injectPassKey(
        ci, g_self && g_self->currentPeerAllowed() ? 123456 : wrongPasskey());
  }
  void onConfirmPasskey(NimBLEConnInfo& ci, uint32_t pin) override {
    bool ok = g_self && g_self->currentPeerAllowed();
    Serial.printf("[BLE] confirm %06u -> %s\n", (unsigned)pin,
                  ok ? "yes" : "REFUSED (not the bonded adapter)");
    NimBLEDevice::injectConfirmPasskey(ci, ok);
  }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    Serial.printf("[BLE] auth enc=%d bond=%d\n", ci.isEncrypted(), ci.isBonded());
  }
#else
  uint32_t onPassKeyRequest() override {
    return g_self && g_self->currentPeerAllowed() ? 123456 : wrongPasskey();
  }
  bool onConfirmPIN(uint32_t pin) override {
    bool ok = g_self && g_self->currentPeerAllowed();
    Serial.printf("[BLE] confirm %06u -> %s\n", (unsigned)pin,
                  ok ? "yes" : "REFUSED (not the bonded adapter)");
    return ok;
  }
  void onAuthenticationComplete(ble_gap_conn_desc* d) override {
    Serial.printf("[BLE] auth enc=%d bond=%d\n", d->sec_state.encrypted, d->sec_state.bonded);
  }
#endif
};
static BleSecCB g_secCb;

static void notifyThunk(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (g_self) g_self->onNotify(data, len);
}

// ---------------------------------------------------------------------------
void BleObdSource::begin() {
  g_self = this;
  sched_.build();              // build the query schedule from the now-selected profile
  cur_.linkUp = false;
  values_[IDX_BARO] = 101.3f;            // sane baro so boost reads ~0 before first baro query
  economy_.reset();            // fresh trip average each key-on

  nvs_.begin("obd", false);
  addr_ = nvs_.getString("bleaddr", "");
  addrType_ = nvs_.getUChar("bletype", 0);
  mirrorAddr();

  NimBLEDevice::init("GMC-OBD");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, true, true);            // bond + MITM + SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
  // Prefer a large ATT MTU (client auto-exchanges on connect). Default 23 fragments
  // every multi-line ELM reply into 20-byte notifies; the vLinker supports more.
  // If the adapter refuses, negotiation falls back — no harm.
  NimBLEDevice::setMTU(247);
}

void BleObdSource::onNotify(const uint8_t* data, size_t len) {
  portENTER_CRITICAL(&rxMux_);
  for (size_t i = 0; i < len; i++) rx_.push(data[i]);
  portEXIT_CRITICAL(&rxMux_);
}

void BleObdSource::poll(uint32_t nowMs) {
  curNow_ = nowMs;

  if (forgetReq_) {
    forgetReq_ = false;
    nvs_.remove("bleaddr"); nvs_.remove("bletype");
    addr_ = ""; addrType_ = 0;
    if (client_ && client_->isConnected()) client_->disconnect();
    conn_ = Conn::Down;
    vinRead_ = false;   // re-read VIN on the next successful connect
    // Clear the published link flag too — the Conn::Up link-lost branch below is
    // skipped once conn_ is Down, so without this the UI/logger would keep treating
    // frozen values as live until the connect-fail escalation reboots the board.
    portENTER_CRITICAL(&mux_); cur_.linkUp = false; portEXIT_CRITICAL(&mux_);
    mirrorAddr(); setPhase(ConnPhase::Idle);
  }

  // Reset trip — marshalled from core 1 (menu); runs on core 0 every poll
  // (connected or not) so economy_ is never touched from two cores at once.
  if (resetTripReq_) { resetTripReq_ = false; economy_.reset(); }

  if (conn_ == Conn::Up) {
    if (!client_ || !client_->isConnected()) {     // link dropped
      conn_ = Conn::Down;
      vinRead_ = false;   // re-read VIN on the next successful connect
      portENTER_CRITICAL(&mux_); cur_.linkUp = false; portEXIT_CRITICAL(&mux_);
      Serial.println("[BLE] link lost — will retry");
      setPhase(ConnPhase::Connecting);
      return;
    }
    if (diagReq_) { diagReq_ = false; runDiag(); return; }   // one-shot raw dump
    if (scanReq_) { scanReq_ = false; runScan(); return; }   // one-shot range scan
    if (defProbeReq_) { defProbeReq_ = false; runDefProbe(); return; }   // one-shot DEF probe
    if (gearOilReq_)  { gearOilReq_  = false; runGearOilProbe(); return; }  // gear/oil live probe
    pollQuery(nowMs);
    // Derived rows (economy/HP/fill) — shared engine, one impl for all sources.
    updateComputedReadouts(economy_, values_, cur_, mux_, nowMs);
    return;
  }

  // Down: respect retry backoff (wrap-safe signed delta), then attempt connect.
  if ((int32_t)(nowMs - nextRetryMs_) < 0) return;
  nextRetryMs_ = nowMs + 3000;
  if (connectAndSetup()) {
    connFails_ = 0;
    conn_ = Conn::Up;
    q_.txState = Tx::Idle; q_.curHeader = -1;
    portENTER_CRITICAL(&mux_); cur_.linkUp = true; portEXIT_CRITICAL(&mux_);
    setPhase(ConnPhase::Up);
    Serial.println("[BLE] connected + ELM ready");

    // One-time-per-session VIN read (standard ATSH7E0 / Mode-09 0902 — never
    // the profile's enhanced addressing). Runs once per connect; vinRead_ is
    // cleared on disconnect (forget + link-lost, above) so a reconnect re-reads.
    if (!vinRead_) {
      QueryIo io{this};
      char vin[18] = "";
      // Bounded retry: a garbled/timed-out 0902 on the FIRST cold-connect read
      // used to latch vinRead_ and never re-read until a full reconnect — the
      // observed "stuck on Generic" field bug. A transient cold-connect miss
      // usually answers on the next attempt, so try a few times before latching.
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
  } else {
    // Escalating recovery for "fails forever" (a wedged NimBLE stack). ~3s backoff:
    // re-init at ~24s of failure, full reboot at ~60s.
    connFails_++;
    if (connFails_ == 8) {
      Serial.println("[BLE] 8 consecutive fails — re-init NimBLE stack");
      recoverBleStack();
    } else if (connFails_ >= 20) {
      Serial.println("[BLE] 20 consecutive fails — restarting");
      delay(50);
      ESP.restart();
    }
  }
}

// Tear down and re-create the NimBLE stack to clear a wedged state after repeated
// connect failures. Mirrors begin()'s init. client_/chars are freed by deinit, so
// null them; the next connectAndSetup() recreates the client.
void BleObdSource::recoverBleStack() {
  if (client_ && client_->isConnected()) client_->disconnect();
  NimBLEDevice::deinit(true);
  client_ = nullptr; writeChar_ = nullptr; notifyChar_ = nullptr;
  q_.curHeader = -1; q_.txState = Tx::Idle;
  NimBLEDevice::init("GMC-OBD");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
  NimBLEDevice::setMTU(247);   // same preference as begin()
}

// Returned BY VALUE: a function-static snapshot here was a cross-core data race —
// core-0 logTick and the core-1 render loop both call latest(), and one core's
// copy-in rewrote the shared static while the other was still reading it (the
// spinlock covers only the copy, not the caller's subsequent reads).
ObdReadings BleObdSource::latest() const {
  ObdReadings snap;
  portENTER_CRITICAL(&mux_); snap = cur_; portEXIT_CRITICAL(&mux_);
  return snap;
}

ConnStatus BleObdSource::connStatus() const {
  ConnStatus cs;
  cs.phase    = phase_;
  cs.attempts = attempts_;
  cs.sinceMs  = phaseSinceMs_;
  cs.addr     = addrBuf_;
  return cs;
}

void BleObdSource::forget()      { forgetReq_ = true; }

// Bonding policy (see BleSecCB): pin pairing to the cached adapter. Runs on
// the NimBLE host task while connectAndSetup() blocks on core 0, so addrBuf_
// (only rewritten by that same core-0 path, via mirrorAddr) is stable for the
// duration of the attempt.
bool BleObdSource::currentPeerAllowed() const {
  if (!addrBuf_[0]) return true;   // no cached adapter: user-initiated pairing window
  if (!client_) return false;
  bool ok = strcasecmp(client_->getPeerAddress().toString().c_str(), addrBuf_) == 0;
  // A refused pairing must be tellable from a dead adapter at the wheel: if
  // the cached address ever goes stale (adapter swapped, address rotation),
  // the pin-to-bonded policy would otherwise present as a silent, unfixable
  // connect/reboot loop. The status overlay names the way out.
  if (!ok) bleStep("refused: not bonded\nuse Forget adapter");
  return ok;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
// The GATT profiles this firmware can drive. FILE SCOPE deliberately: both the
// pre-connect advertisement filter in connectAndSetup() and the post-connect
// bind in bindChars() read this same table, and two copies would drift -- the
// filter would skip a device the binder could actually have handled.
struct Profile { NimBLEUUID svc, notify, write; const char* tag; };
static const Profile PROFILES[] = {
  {NimBLEUUID((uint16_t)0x18f0), NimBLEUUID((uint16_t)0x2af0), NimBLEUUID((uint16_t)0x2af1), "vlinker 18f0"},
  {NimBLEUUID((uint16_t)0xfff0), NimBLEUUID((uint16_t)0xfff1), NimBLEUUID((uint16_t)0xfff2), "clone fff0"},
  {NimBLEUUID((uint16_t)0xffe0), NimBLEUUID((uint16_t)0xffe1), NimBLEUUID((uint16_t)0xffe1), "clone ffe0"},
  // Nordic UART Service (some BLE-ELM327 clones on Nordic silicon): TX
  // (device->client, notify) ...0003, RX (client->device, write) ...0002.
  {NimBLEUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e"),
   NimBLEUUID("6e400003-b5a3-f393-e0a9-e50e24dcca9e"),
   NimBLEUUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e"), "nordic-uart"},
};

bool BleObdSource::connectAndSetup() {
  attempts_++;
  if (!client_) client_ = NimBLEDevice::createClient();
  client_->setClientCallbacks(&g_secCb, false);
  // Bound each connect so a dead adapter fails fast (library default is 30s).
  // UNITS CHANGED BETWEEN LIBRARY MAJORS: NimBLE 1.4.x took SECONDS, 2.x takes
  // MILLISECONDS ("The number of milliseconds before timeout" — NimBLEClient.h).
  // The parameter stayed uint32_t, so passing 4 still compiles on 2.x and silently
  // becomes a 4 ms budget — no adapter can complete a connection in that, and the
  // dash sits in connecting -> failed forever. Shipped that way in v0.1.0.
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
  client_->setConnectTimeout(4000);   // 2.x: milliseconds
#else
  client_->setConnectTimeout(4);      // 1.4.x: seconds
#endif

  // Fast path: reconnect to the cached adapter.
  if (addr_.length()) {
    setPhase(ConnPhase::Connecting);
    Serial.printf("[BLE] connecting cached %s …\n", addr_.c_str());
    if (client_->connect(NimBLEAddress(std::string(addr_.c_str()), addrType_)) && bindChars())
      return true;
    if (client_->isConnected()) client_->disconnect();
  }

  // Discovery: scan, then connect (strongest signal first — the adapter is
  // plugged in next to the board) and keep the one exposing the 0x18f0 profile.
  //
  // Kick before the scan: the cached-connect attempt above already spent up to
  // its 4 s connect timeout plus a full bindChars() (bonding included) before
  // failing, and the 6 s blocking scan below is charged to the same poll().
  obdWatchdogKick();
  setPhase(ConnPhase::Scanning);
  Serial.println("[BLE] scanning 6s …");
  NimBLEScan* scan = NimBLEDevice::getScan();
  // setInterval/setWindow also changed units across the major: 1.4.x took BLE
  // 0.625 ms ticks (100 -> 62.5 ms), 2.x takes milliseconds outright. Both
  // readings leave window ~= interval, i.e. scan almost continuously, so this is
  // benign either way — but the numbers no longer mean what a 1.4-era reader
  // would assume, hence the note rather than a silent value change.
  scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
  scan->clearResults();
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
  NimBLEScanResults res = scan->getResults(6000, false);   // 2.x: ms, blocking, returns results
#else
  NimBLEScanResults res = scan->start(6, false);           // 1.4: seconds
#endif
  int n = res.getCount();
  { char sb[32]; snprintf(sb, sizeof sb, "scan: %d dev", n); bleStep(sb); }
  Serial.printf("[BLE] scan found %d devices\n", n);

  // getDevice() returns a value in NimBLE 1.4 but a pointer in 2.x — normalize.
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
  #define RSSI_OF(r, i) ((r).getDevice(i)->getRSSI())
#else
  #define RSSI_OF(r, i) ((r).getDevice(i).getRSSI())
#endif
  // Rank the try-order: OBD-named adapters first, then by RSSI (strongest
  // first). The adapter is plugged in next to the board, but in a crowded RF
  // spot (a parking lot) a strong phone or watch shouldn't be tried ahead of a
  // named OBD dongle. The ranking is a pure, host-tested function (ble_rank.cpp).
  int order[64]; int m = n < 64 ? n : 64;
  std::string bleNames[64];   // own the name strings so the c_str()s stay valid
  BleCand cands[64];
  for (int i = 0; i < m; i++) {
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
    bleNames[i] = res.getDevice(i)->getName();
#else
    { NimBLEAdvertisedDevice d = res.getDevice(i); bleNames[i] = d.getName(); }
#endif
    cands[i].name = bleNames[i].c_str();
    cands[i].rssi = RSSI_OF(res, i);
    // Read what the ADVERTISEMENT claims about services, before connecting to
    // anything. This is the whole point of UX-6: the old path connected first
    // and inspected afterwards, so it GATT-connected to strangers' phones and
    // attempted bonding with them.
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
    const NimBLEAdvertisedDevice* ad = res.getDevice(i);
#else
    NimBLEAdvertisedDevice adObj = res.getDevice(i);
    const NimBLEAdvertisedDevice* ad = &adObj;
#endif
    cands[i].svc = SvcHint::None;
    for (const Profile& p : PROFILES) {
      if (ad->isAdvertisingService(p.svc)) { cands[i].svc = SvcHint::Obd; break; }
    }
    if (cands[i].svc == SvcHint::None && ad->haveServiceUUID())
      cands[i].svc = SvcHint::Other;   // said something, and it was not us
  }
  bleRankCandidates(cands, m, order);

  // Publish the "devices seen" list (strongest first) for the on-screen scan view.
  { int p = snprintf(g_bleScan, sizeof g_bleScan, "%d seen (top 6):\n", n);
    for (int k = 0; k < m && k < 6; k++) {
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
      const NimBLEAdvertisedDevice* d = res.getDevice(order[k]);
#else
      NimBLEAdvertisedDevice dObj = res.getDevice(order[k]); NimBLEAdvertisedDevice* d = &dObj;
#endif
      std::string nm = d->getName(), mac = d->getAddress().toString();
      p += snprintf(g_bleScan + p, sizeof g_bleScan - p, "%.14s %d\n",
                    nm.empty() ? mac.c_str() : nm.c_str(), d->getRSSI());
      if (p >= (int)sizeof g_bleScan - 20) break;
    }
  }

  // Try the strongest few; the vLinker should be near the top.
  //
  // WATCHDOG: this loop is long synchronous core-0 work inside ONE poll(), which
  // is exactly the shape wdt_kick.h exists for — the heartbeat is stamped once
  // per poll(), so without a kick a slow round looks identical to a hang. The
  // round is not small: a 6 s scan, then up to twelve iterations of a 4 s connect
  // plus bindChars()'s secureConnection() bonding, which takes many seconds
  // against a stranger's device. main.cpp already widened the watchdog window
  // from 90 s to 240 s because of this, which treated the symptom; kicking here
  // addresses it directly. Kick per ITERATION, not once before the loop, or a
  // round that stalls late still trips it.
  // Bound the round. Even with the skip below, a busy RF environment can offer
  // more silent devices than there is time for, and connectAndSetup() runs
  // inside ONE poll() -- the caller retries every 3 s, so giving up early costs
  // nothing and stops one round monopolising the OBD task.
  const uint32_t roundDeadline = millis() + 45000;
  int skipped = 0;
  for (int k = 0; k < m && k < 12; k++) {
    obdWatchdogKick();
    if ((int32_t)(millis() - roundDeadline) >= 0) {
      Serial.printf("[BLE] round budget spent after %d tried — retrying shortly\n", k);
      break;
    }
    // Never connect to something that advertised a service set that is not
    // ours. Devices advertising NOTHING are still tried: silence is not
    // evidence, and some adapters do not advertise their service UUID.
    if (bleShouldSkip(cands[order[k]])) { skipped++; continue; }
#if defined(NIMBLE_CPP_VERSION_MAJOR) && NIMBLE_CPP_VERSION_MAJOR >= 2
    const NimBLEAdvertisedDevice* dev = res.getDevice(order[k]);   // 2.x: pointer
#else
    NimBLEAdvertisedDevice devObj = res.getDevice(order[k]);       // 1.4: by value
    NimBLEAdvertisedDevice* dev = &devObj;
#endif
    Serial.printf("[BLE] try %s rssi %d '%s'\n",
                  dev->getAddress().toString().c_str(), dev->getRSSI(), dev->getName().c_str());
    bleStep("connecting");
    if (!client_->connect(dev)) { bleStep("connect FAIL"); Serial.println("   connect failed"); continue; }
    bleStep("connected");
    Serial.println("   connected; checking GATT…");
    if (bindChars()) {
      addr_ = dev->getAddress().toString().c_str();
      addrType_ = dev->getAddress().getType();
      nvs_.putString("bleaddr", addr_);
      nvs_.putUChar("bletype", addrType_);
      mirrorAddr();
      Serial.printf("[BLE] adapter = %s — cached\n", addr_.c_str());
      return true;
    }
    if (client_->isConnected()) client_->disconnect();
  }
  Serial.printf("[BLE] no OBD adapter found this round (%d skipped: advertised other services)\n",
                skipped);
  return false;
}

// After connect: bond, locate the ELM chars, subscribe, and run the AT init.
bool BleObdSource::bindChars() {
  setPhase(ConnPhase::Initializing);
  bleStep("securing..");
  // secureConnection() is the single longest blocking call in the connect path —
  // bonding with a device that is not an OBD adapter (which is most of what a
  // busy RF environment offers) can take many seconds and has no timeout we set.
  // Kick either side of it: before, so a slow bond starts from a fresh heartbeat;
  // after, so the time it consumed is not charged to the AT init that follows.
  obdWatchdogKick();
  client_->secureConnection();          // bond if the adapter requires it
  obdWatchdogKick();
  bleStep("find svc");
  // Try known BLE-ELM327 GATT profiles in order so the cheapest adapter works:
  // {service, notify-char, write-char}. vLinker MS first, then generic clones
  // (0xFFF0 3-char, and 0xFFE0 where notify+write share one 0xFFE1 char).
  // {service, notify-char, write-char} for each known BLE-ELM327 GATT layout,
  // tried in order so the cheapest adapter still works. NimBLEUUID holds both
  // 16-bit (clones) and 128-bit (Nordic UART) identifiers. Built locally, not
  // static, to avoid global-init ordering on the 128-bit string UUIDs.
  writeChar_ = notifyChar_ = nullptr;
  for (const Profile& p : PROFILES) {
    NimBLERemoteService* svc = client_->getService(p.svc);
    if (!svc) continue;
    NimBLERemoteCharacteristic* nc = svc->getCharacteristic(p.notify);
    NimBLERemoteCharacteristic* wc = svc->getCharacteristic(p.write);
    if (!nc || !wc) continue;
    if (!nc->subscribe(true, notifyThunk)) continue;
    notifyChar_ = nc; writeChar_ = wc;
    char sb[24]; snprintf(sb, sizeof sb, "%s ok", p.tag); bleStep(sb);
    Serial.printf("   bound GATT profile %s\n", p.tag);
    break;
  }
  if (!notifyChar_ || !writeChar_) { bleStep("no OBD profile"); Serial.println("   no known BLE-ELM327 profile"); return false; }
  delay(200);
  // ELM init, hand-rolled rather than via a library: reset, echo/linefeed/spaces off.
  // ATE0 (echo off) is CRITICAL: if it never acks, every reply is prefixed with the
  // echoed command and ALL parses fail (link shows UP but gauges stay "--"). Gate the
  // connect on it (retry once). ATL0/ATS0 are best-effort — the parser strips
  // linefeeds/spaces anyway, so their failure is non-fatal.
  atInit("ATZ\r", 2000);                       // reset (echo defaults ON after this)
  bool echoOff = atInit("ATE0\r", 1000);
  if (!echoOff) { Serial.println("   ATE0 no ack — retry"); delay(100); echoOff = atInit("ATE0\r", 1000); }
  if (!echoOff) { bleStep("ATE0 FAIL"); Serial.println("   ATE0 failed — abandon connect"); return false; }
  atInit("ATL0\r", 1000);
  atInit("ATS0\r", 1000);
  // Response-timing tune (best-effort — parser is unaffected if either is
  // ignored). By default the ELM idles ~205 ms after every reply waiting for
  // more responders before printing '>', even on physically-addressed 7E0/7E2
  // queries — that tail dominates per-query latency. ATAT2 = aggressive
  // adaptive timing (learns each PID's real response time); ATST19 caps the
  // ceiling at 25 x 4.096 ms ~= 102 ms for PIDs it hasn't learned yet.
  atInit("ATAT2\r", 1000);
  atInit("ATST19\r", 1000);
  bleStep("LINK UP");
  Serial.printf("   GATT ok; ELM init done (MTU %u)\n", client_ ? client_->getMTU() : 0);
  return true;
}

// ---------------------------------------------------------------------------
// BLE transport shims
// ---------------------------------------------------------------------------
void BleObdSource::bleWrite(const char* s) {
  if (writeChar_) writeChar_->writeValue((uint8_t*)s, strlen(s), false);
}
int BleObdSource::bleAvailable() {
  portENTER_CRITICAL(&rxMux_); int n = (int)rx_.available(); portEXIT_CRITICAL(&rxMux_);
  return n;
}
int BleObdSource::bleRead() {
  portENTER_CRITICAL(&rxMux_); int c = rx_.read(); portEXIT_CRITICAL(&rxMux_);
  return c;
}
bool BleObdSource::atInit(const char* cmd, uint32_t timeoutMs) {
  portENTER_CRITICAL(&rxMux_); rx_.clear(); portEXIT_CRITICAL(&rxMux_);
  bleWrite(cmd);
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    int c = bleRead();
    if (c == '>') return true;
    if (c < 0) delay(5);
  }
  return false;
}

// ---------------------------------------------------------------------------
// PID query — shared engine (obd_query.h) over the BLE transport shims.
// ---------------------------------------------------------------------------
void BleObdSource::pollQuery(uint32_t nowMs) {
  QueryIo io{this};
  pidQueryStep(q_, sched_, io, nowMs, /*replyTimeoutMs=*/400, values_, cur_, mux_);
}

// Send one command at `sh` (AT SH header) then `cmd`, accumulate the reply to the
// '>' prompt, and return it with CR/LF escaped for readability ("" if empty).
// Owns the rx ring for the call. Shared by runDiag and runScan.
std::string BleObdSource::probePid(const char* sh, const char* cmd) {
  obdWatchdogKick();   // diagnostic loops run many of these inside one poll()
  // Set header; drain to the prompt.
  portENTER_CRITICAL(&rxMux_); rx_.clear(); portEXIT_CRITICAL(&rxMux_);
  bleWrite(sh);
  uint32_t t0 = millis();
  while (millis() - t0 < 700) { int ch = bleRead(); if (ch == '>') break; if (ch < 0) delay(5); }
  // Send the command; accumulate the raw reply to the prompt.
  portENTER_CRITICAL(&rxMux_); rx_.clear(); portEXIT_CRITICAL(&rxMux_);
  bleWrite(cmd);
  std::string reply;
  t0 = millis();
  while (millis() - t0 < 700) {
    int ch = bleRead();
    if (ch < 0) { delay(5); continue; }
    if (ch == '>') break;
    reply += (char)ch;
  }
  std::string esc;
  for (char ch : reply) {
    if (ch == '\r') esc += "\\r";
    else if (ch == '\n') esc += "\\n";
    else esc += ch;
  }
  return esc;
}

// True if a reply looks like a positive read (mode-22 '62' or mode-2C '6C'),
// not NO DATA / '?' / a '7F' negative response.
static bool scanIsPositive(const std::string& r) {
  if (r.empty()) return false;
  if (r.find("NO DATA") != std::string::npos) return false;
  if (r.find('?') != std::string::npos) return false;
  return r.find("62") != std::string::npos || r.find("6C") != std::string::npos;
}

// Diagnostic: dump the raw ELM replies for the EGT/DPF PIDs (both GM mode-22 at
// 7E0 and generic mode-01 at 7DF). Throwaway instrumentation. Runs on core 0 from
// poll() (pollQuery skipped this tick), owning the BLE link.
void BleObdSource::runDiag() {
  struct DiagCmd { const char* sh; const char* cmd; };
  static const DiagCmd cmds[] = {
    {"AT SH 7E0\r", "220078\r"}, {"AT SH 7E0\r", "22007A\r"}, {"AT SH 7E0\r", "22007C\r"},
    {"AT SH 7DF\r", "0178\r"},   {"AT SH 7DF\r", "017A\r"},   {"AT SH 7DF\r", "017C\r"},
  };
  Serial.println("[DIAG] EGT/DPF raw-reply dump:");
  for (const auto& c : cmds) {
    std::string esc = probePid(c.sh, c.cmd);
    char label[8];
    snprintf(label, sizeof label, "%.*s", (int)(strlen(c.cmd) - 1), c.cmd);  // drop trailing \r
    Serial.printf("[DIAG] %s => \"%s\"\n", label, esc.c_str());
  }
  q_.curHeader = -1;        // force AT SH re-issue on the next normal query
  q_.txState   = Tx::Idle;
  Serial.println("[DIAG] done");
}

// Like probePid, but built for a clean single-frame capture: after setting the
// header it drops the AT-SH "OK" residual (the earlier 'd' dump caught that "OK"
// instead of the PID reply), uses wider deadlines, and returns the RAW reply so
// the caller can run it through parseObdResponse.
std::string BleObdSource::probeFrame(const char* sh, const char* cmd) {
  obdWatchdogKick();   // diagnostic loops run many of these inside one poll()
  portENTER_CRITICAL(&rxMux_); rx_.clear(); portEXIT_CRITICAL(&rxMux_);
  bleWrite(sh);
  uint32_t t0 = millis();
  while (millis() - t0 < 1000) { int ch = bleRead(); if (ch == '>') break; if (ch < 0) delay(5); }
  delay(40);                                                  // let trailing "OK\r\r" arrive
  portENTER_CRITICAL(&rxMux_); rx_.clear(); portEXIT_CRITICAL(&rxMux_);  // ...then drop it
  bleWrite(cmd);
  std::string reply;
  t0 = millis();
  while (millis() - t0 < 1500) {
    int ch = bleRead();
    if (ch < 0) { delay(5); continue; }
    if (ch == '>') break;
    reply += (char)ch;
  }
  return reply;
}

// Diagnostic: full-frame DEF dump. For the two parseable PIDs (mode-01 019B,
// mode-22 22009B) it prints every assembled data byte INDEXED, so comparing a
// capture now (full) against the known 52% reading reveals which byte is the real
// tank level (vs DEF concentration, which barely moves). GM-ext 2CFE903F is raw
// only (mode 2C isn't parsed here). Owns the BLE link (pollQuery skipped). Throwaway.
void BleObdSource::runDefProbe() {
  struct DefCmd { const char* sh; const char* cmd; uint8_t mode; uint16_t pid; const char* note; };
  static const DefCmd cmds[] = {
    {"AT SH 7DF\r", "019B\r",     0x01, 0x009B, "std mode-01 DEF sensor data"},
    {"AT SH 7E0\r", "22009B\r",   0x22, 0x009B, "GM mode-22 DEF (tile reads byte[1])"},
    {"AT SH 7E0\r", "2CFE903F\r", 0x00, 0x0000, "GM-ext DEF level (raw only; A*3.92 %)"},
  };
  Serial.println("[DEF] full-frame dump (diff each byte vs the known 52% reading):");
  for (const auto& c : cmds) {
    std::string raw = probeFrame(c.sh, c.cmd);
    std::string esc;
    for (char ch : raw) { if (ch == '\r') esc += "\\r"; else if (ch == '\n') esc += "\\n"; else esc += ch; }
    char label[12];
    snprintf(label, sizeof label, "%.*s", (int)(strlen(c.cmd) - 1), c.cmd);  // drop trailing \r
    Serial.printf("[DEF] %-9s raw=\"%s\"\n", label, esc.c_str());
    if (c.mode == 0x01 || c.mode == 0x22) {
      std::vector<uint8_t> d;
      if (parseObdResponse(raw, c.mode, c.pid, d) && !d.empty()) {
        Serial.printf("[DEF]   parsed %d byte(s):", (int)d.size());
        for (size_t i = 0; i < d.size(); i++)
          Serial.printf("  [%d]=0x%02X(%d)", (int)i, d[i], d[i]);
        Serial.println();
      } else {
        Serial.println("[DEF]   parse: NO DATA / mismatch");
      }
    }
    Serial.printf("[DEF]   (%s)\n", c.note);
  }
  q_.curHeader = -1;        // force AT SH re-issue on the next normal query
  q_.txState   = Tx::Idle;
  Serial.println("[DEF] done");
}

// Diagnostic: ~20 quick samples of the gear + oil-pressure candidates, one line
// per sample, so the driver can hold a STEADY gear/RPM and watch which value is
// the gear (matches the DIC) and whether any oil candidate rises with RPM /
// matches the DIC oil-pressure needle. RPM is included for context. Throwaway.
void BleObdSource::runGearOilProbe() {
  struct P { const char* sh; const char* cmd; uint16_t pid; const char* tag; };
  static const P ps[] = {
    {"AT SH 7E0\r", "22000C\r", 0x000C, "rpm"},      // ((A*256)+B)/4
    {"AT SH 7E0\r", "22005C\r", 0x005C, "oilT"},      // oil temp (A-40 C) — pressure context
    {"AT SH 7E2\r", "22199A\r", 0x199A, "g199A"},     // gear candidate (prime)
    {"AT SH 7E2\r", "221995\r", 0x1995, "g1995"},     // gear candidate (alt)
    {"AT SH 7E0\r", "22115C\r", 0x115C, "o115C"},     // oil candidate
    {"AT SH 7E0\r", "22115D\r", 0x115D, "o115D"},     // oil candidate
    {"AT SH 7E0\r", "2200AF\r", 0x00AF, "oAF"},       // oil candidate (rose with load)
  };
  Serial.println("[GO] gear/oil live probe (20 samples). Hold a steady gear/RPM; note DIC gear + oil psi.");
  for (int i = 0; i < 20; i++) {
    if (!client_ || !client_->isConnected()) { Serial.println("[GO] link lost — aborting probe"); break; }
    char line[160];
    int n = snprintf(line, sizeof line, "[GO] %2d", i);
    // snprintf returns the WOULD-BE length, not bytes written: once the line is
    // full, an unclamped n makes `sizeof line - n` underflow (huge size_t) and
    // `line + n` point past the buffer — an out-of-bounds stack write on the
    // very next append. Clamp after EVERY accumulation so both stay in-bounds
    // (at the clamp, size is 1 and snprintf writes only the NUL).
    auto clampN = [&n, &line]() { if (n > (int)sizeof line - 1) n = (int)sizeof line - 1; };
    for (const auto& p : ps) {
      std::string raw = probeFrame(p.sh, p.cmd);
      std::vector<uint8_t> d;
      if (parseObdResponse(raw, 0x22, p.pid, d) && !d.empty()) {
        if (p.pid == 0x000C && d.size() >= 2) {
          n += snprintf(line + n, sizeof line - n, "  %s=%d", p.tag, ((d[0] << 8) + d[1]) / 4);
          clampN();
        } else if (p.pid == 0x005C) {
          n += snprintf(line + n, sizeof line - n, "  %s=%dF", p.tag, (d[0] - 40) * 9 / 5 + 32);
          clampN();
        } else {
          n += snprintf(line + n, sizeof line - n, "  %s=", p.tag);
          clampN();
          for (uint8_t b : d) {
            n += snprintf(line + n, sizeof line - n, "%02X", b);
            clampN();
            if (n >= (int)sizeof line - 1) break;   // line full — stop hex dump
          }
          // Decimal beside the hex (and the o115C scaling guess as psi) so the
          // driver can correlate against the DIC needle without decoding hex
          // at the wheel. The psi formula is the UNCONFIRMED scan-notes fit
          // ((A*0.65)-17.5) — the '?' marks it as a candidate, not a reading.
          if (d.size() <= 2) {
            unsigned v = d.size() == 2 ? (unsigned)((d[0] << 8) | d[1]) : d[0];
            n += snprintf(line + n, sizeof line - n, "(%u)", v);
            clampN();
            if (p.pid == 0x115C) {
              n += snprintf(line + n, sizeof line - n, "~%dpsi?", (int)(d[0] * 0.65f - 17.5f));
              clampN();
            }
          }
        }
      } else {
        n += snprintf(line + n, sizeof line - n, "  %s=--", p.tag);
        clampN();
      }
      if (n >= (int)sizeof line - 8) break;   // guard the buffer
    }
    Serial.println(line);
  }
  q_.curHeader = -1;
  q_.txState   = Tx::Idle;
  Serial.println("[GO] done");
}

// Range scanner: brute-sweep the standardized 2200xx mirror block (7E0) and the
// 2219xx trans block (7E2), logging positive replies; then probe a curated list of
// GM-proprietary / slot-read candidate addresses (logging every attempt). Capture
// at idle and under load; positives that change with load are live sensors. Owns
// the BLE link (pollQuery skipped). Throwaway diagnostic.
void BleObdSource::runScan() {
  char cmd[16];
  int hits;
  // 544 sequential probes; with a quiet ECU each waits its full 1.4s. probePid
  // kicks the watchdog, and a dropped BLE link aborts the sweep (every remaining
  // probe would just burn its timeout and return "").
  auto linkLost = [this]() {
    if (client_ && client_->isConnected()) return false;
    Serial.println("[SCAN] link lost — aborting scan");
    return true;
  };

  Serial.println("[SCAN] phase 1: 2200xx @ 7E0");
  hits = 0;
  for (int pid = 0x00; pid <= 0xFF && !linkLost(); pid++) {
    snprintf(cmd, sizeof cmd, "2200%02X\r", pid);
    std::string r = probePid("AT SH 7E0\r", cmd);
    if (scanIsPositive(r)) { Serial.printf("[SCAN] 7E0 2200%02X => \"%s\"\n", pid, r.c_str()); hits++; }
  }
  Serial.printf("[SCAN] phase 1 done: %d hits\n", hits);

  Serial.println("[SCAN] phase 2: 2219xx @ 7E2");
  hits = 0;
  for (int pid = 0x00; pid <= 0xFF && !linkLost(); pid++) {
    snprintf(cmd, sizeof cmd, "2219%02X\r", pid);
    std::string r = probePid("AT SH 7E2\r", cmd);
    if (scanIsPositive(r)) { Serial.printf("[SCAN] 7E2 2219%02X => \"%s\"\n", pid, r.c_str()); hits++; }
  }
  Serial.printf("[SCAN] phase 2 done: %d hits\n", hits);

  // Phase 3: curated candidates outside the swept ranges (all 7E0). Log every
  // attempt (positive and not) so we know which answered.
  Serial.println("[SCAN] phase 3: candidates @ 7E0");
  static const char* const candidates[] = {
    "221470\r", "22115C\r", "2CFE1894\r",            // oil pressure (3 conflicting)
    "2CFE9043\r",                                     // fuel rail desired
    "2CFEB2C6\r", "2CFE906C\r",                       // DPF soot % / grams
    "2CFE90F8\r", "2CFE8097\r",                       // DPF regen status
    "2CFE90F0\r",                                     // distance since regen
    "2CFE3826\r",                                     // turbo VGT position
    "2CFE903F\r", "2CFE9055\r",                       // DEF level / range
    "2CFEB244\r",                                     // reductant consumed
    "2CFE90DB\r", "2CFE9027\r", "2CFEB29B\r",         // NOx 1 / 2 / cat efficiency
    "2CFE80A7\r",                                     // boost sensor (slot)
    "2CFE800A\r", "2CFE8096\r",                       // intake air temp 2 / 3 (slot)
    "2CFE90F2\r", "2CFE90D2\r", "2CFE90E1\r", "2CFE90C3\r",  // EGT sensors 1-4 (slot)
    "2CFE007C\r",                                     // injection timing (slot)
    "2CFE9250\r",                                     // fuel temp (slot)
    "22115D\r",                                       // lift pump (guess)
    "22162F\r", "221630\r", "221631\r", "221632\r",  // cyl balance rates 1-8
    "221633\r", "221634\r", "221635\r", "221636\r",
  };
  hits = 0;
  const int n = (int)(sizeof candidates / sizeof candidates[0]);
  for (int i = 0; i < n && !linkLost(); i++) {
    std::string r = probePid("AT SH 7E0\r", candidates[i]);
    char label[12];
    snprintf(label, sizeof label, "%.*s", (int)(strlen(candidates[i]) - 1), candidates[i]);  // drop \r
    Serial.printf("[SCAN] 7E0 %s => \"%s\"\n", label, r.empty() ? "(none)" : r.c_str());
    if (scanIsPositive(r)) hits++;
  }
  Serial.printf("[SCAN] phase 3 done: %d/%d candidates answered\n", hits, n);

  q_.curHeader = -1;
  q_.txState   = Tx::Idle;
  Serial.println("[SCAN] done");
}

#endif  // BLE_OBD && !MOCK_OBD
