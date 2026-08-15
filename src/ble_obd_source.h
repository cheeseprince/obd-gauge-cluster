#pragma once
#include "obd_source.h"

// ble_obd_source.h — Live OBD over BLE (vLinker MS in BT+BLE mode) for the
// ESP32-S3 board, via NimBLE. The S3 has no Bluetooth Classic radio, so BLE is
// the only OBD transport this firmware has. Profile captured on the truck:
//   service 0x18f0, notify 0x2af0 (replies), write 0x2af1 (commands), ELM327 v2.3.
//
// Built only when BLE_OBD is defined (the crowpanel_obd env). Every other build
// (mock, or the classic real build) gets a parse-only stub so the header is
// harmless to include from main.cpp unconditionally.

#if defined(BLE_OBD) && !defined(MOCK_OBD)
#include <string>
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "obd_schedule.h"
#include "obd_query.h"
#include "readouts.h"
#include "byte_ring.h"
#include "economy.h"

class BleObdSource : public ObdSource {
 public:
  void begin() override;
  void poll(uint32_t nowMs) override;
  ObdReadings latest() const override;

  // Console/menu hooks. requestPair() was removed with the serial 'p' key
  // (F-11): it was forget() plus an immediate retry, and forget() is what the
  // settings menu calls.
  void forget();        // Settings -> Forget adapter: clear cached adapter from NVS
  bool pairing() const { return false; }   // discovery is automatic; never owns the console
  // Vehicle bring-up probes. COMPILED OUT of the shipped firmware — see the
  // OBD_DEV_CONSOLE note on the implementations in the .cpp.
#if defined(OBD_DEV_CONSOLE)
  void requestDiag() { diagReq_ = true; }   // 'e' — dump raw EGT/DPF replies once
  void requestScan() { scanReq_ = true; }    // 'x' — sweep enhanced PID ranges + candidates once
  void requestDefProbe() { defProbeReq_ = true; }  // 'd' — dump DEF candidate PIDs once
  void requestGearOilProbe() { gearOilReq_ = true; }  // 'g' — ~20-sample gear/oil live probe
#endif

  ConnStatus connStatus() const override;
  void resetTrip() { resetTripReq_ = true; }  // menu "Reset trip" — marshalled to core 0 via resetTripReq_

  // Effective diesel tank capacity (gal), pushed from core 1 whenever the
  // setting or the active profile changes; 0 = unknown. Plain float rather than
  // a request flag: a torn read costs one frame of a wrong fill figure, which
  // self-corrects on the next poll, so it does not earn a critical section.
  void setDieselTankGal(float gal) { dieselTankGal_ = gal; }

  // Free the entire NimBLE stack before the OTA/portal flows take the radio.
  //
  // WHY, measured on the truck 2026-08-15: the TLS handshake failed with
  // MBEDTLS_ERR_SSL_ALLOC_FAILED (-32512) and only ~56 KB of heap free. Boot is
  // ~104 KB; bringing up WiFi costs ~47 KB of that, and mbedTLS then wants 16 KB
  // in + 16 KB out + context and cert parsing. Suspending OBD polling is NOT
  // enough -- NimBLE stays initialised and, with no adapter present, keeps
  // scanning and walking the GATT database of every stranger in range (23-25
  // BLE advertisers in a parking lot), churning and fragmenting the heap
  // underneath the handshake.
  //
  // One-way on purpose: unlike recoverBleStack() this does NOT re-init, because
  // every caller restarts immediately afterwards. Safe to call only with the
  // OBD task idled (g_suspendObd) -- nothing else may touch the stack after it.
  void shutdownForOta();

  // Called from the NimBLE host task when a notification arrives.
  void onNotify(const uint8_t* data, size_t len);

  // Bonding-policy check for the NimBLE security callbacks (which carry no
  // usable peer info on 1.4): may the peer of the in-progress connect attempt
  // pair with us? True for the cached adapter, or for ANY peer while no
  // adapter is cached (first setup / after "Forget adapter" — the
  // user-initiated trust-on-first-use window). See SECURITY.md.
  bool currentPeerAllowed() const;

 private:
  enum class Conn { Down, Up };
  Conn conn_ = Conn::Down;

  NimBLEClient*               client_     = nullptr;
  NimBLERemoteCharacteristic* writeChar_  = nullptr;
  NimBLERemoteCharacteristic* notifyChar_ = nullptr;

  ByteRing             rx_;                                  // notify bytes (host task) -> query loop
  mutable portMUX_TYPE rxMux_ = portMUX_INITIALIZER_UNLOCKED;
  ObdReadings          cur_;                                 // guarded by mux_
  mutable portMUX_TYPE mux_   = portMUX_INITIALIZER_UNLOCKED;
  ObdSchedule          sched_;
  Preferences          nvs_;

  String   addr_;             // cached adapter BLE address ("" = none)
  uint8_t  addrType_ = 0;     // public/random
  // Reconnect backoff. Now read AND written only by poll() on core 0 — the one
  // cross-core writer was requestPair(), removed with the serial 'p' key (F-11).
  // `volatile` is kept rather than dropped: it costs nothing here and removing it
  // is a memory-model change that wants its own commit, not a drive-by.
  volatile uint32_t nextRetryMs_ = 0;

  // Connecting-overlay status. Written on core 0 (poll/connect), read on core 1
  // (connStatus). Each field is volatile, but the trio is read without a lock —
  // an accepted DISPLAY-ONLY gap (worst case: one stale status frame). attempts_++
  // is likewise a non-atomic RMW on core 0 only, which is fine.
  volatile ConnPhase phase_        = ConnPhase::Idle;
  volatile uint16_t  attempts_     = 0;
  volatile uint32_t  phaseSinceMs_ = 0;
  uint32_t           curNow_       = 0;       // latest poll() timestamp
  char               addrBuf_[20]  = {0};     // stable copy of addr_ for connStatus()
  void setPhase(ConnPhase p) { phase_ = p; phaseSinceMs_ = curNow_; }
  void mirrorAddr() {                          // copy addr_ -> addrBuf_ (cheap, core 0)
    size_t n = addr_.length(); if (n >= sizeof(addrBuf_)) n = sizeof(addrBuf_) - 1;
    for (size_t i = 0; i < n; i++) addrBuf_[i] = addr_[i];
    addrBuf_[n] = 0;
  }

  // Written by setDieselTankGal() (core 1), read in poll() (core 0). 0 = unknown.
  volatile float dieselTankGal_ = 0.0f;

  volatile bool forgetReq_ = false;   // set by forget() (core 1), consumed in poll() (core 0)
  volatile bool resetTripReq_ = false;  // set by resetTrip() (core 1), consumed in poll() (core 0)
#if defined(OBD_DEV_CONSOLE)
  volatile bool diagReq_ = false;   // set by 'e' key (core 1), consumed in poll() (core 0)
  volatile bool scanReq_ = false;   // set by 'x' key (core 1), consumed in poll() (core 0)
  volatile bool defProbeReq_ = false;  // set by 'd' key (core 1), consumed in poll() (core 0)
  volatile bool gearOilReq_ = false;   // set by 'g' key (core 1), consumed in poll() (core 0)
#endif

  // VIN read runs once per connected session (Conn::Up success block); cleared
  // on disconnect so a reconnect re-reads (e.g. after an adapter/vehicle swap).
  bool vinRead_ = false;

  Economy economy_;            // trip MPG integrator (computed readouts)

  // Non-blocking PID query state machine — shared engine (obd_query.h).
  PidQueryState q_;
  float         values_[STAT_COUNT] = {};
  // Transport adapter handed to pidQueryStep (nested type -> private access).
  struct QueryIo {
    BleObdSource* s;
    void write(const char* c) { s->bleWrite(c); }
    int  available()          { return s->bleAvailable(); }
    int  read()               { return s->bleRead(); }
    void flushRx() {
      portENTER_CRITICAL(&s->rxMux_); s->rx_.clear(); portEXIT_CRITICAL(&s->rxMux_);
    }
  };

  int connFails_ = 0;        // consecutive connectAndSetup failures (core 0); resets on success
  // Device count from the most recent connectAndSetup() discovery scan. In
  // poll()'s escalating recovery, connFails_ increments on every failed round
  // (cheap recoverBleStack() re-init fires every 8 of those, unconditionally)
  // -- but the full ESP.restart(), which is the only step that wipes the
  // session-scoped BLE reject ring, additionally requires lastScanCount_<=0.
  // That is the signal that the whole radio can't even scan, not just that
  // connecting is failing; a round that scanned successfully and found devices
  // is proof the stack is alive, regardless of whether any of those devices
  // was the adapter, so it must never trigger a reboot on its own.
  // Set every time connectAndSetup() reaches its discovery scan (i.e. on every
  // failure path -- the fast cached-addr path returns before reaching it).
  int lastScanCount_ = 0;

  // Connection: try cached addr, else scan + identify by the 0x18f0/2af1 profile.
  bool connectAndSetup();
  void recoverBleStack();    // deinit+reinit NimBLE after repeated failures (clears a wedged stack)
  bool bindChars();          // after connect: secure, find chars, subscribe, AT-init
  void pollQuery(uint32_t nowMs);
  // --- Vehicle bring-up probes — DEV BUILD ONLY (F-11) ---------------------
  // PR #12 gated the serial DISPATCH so these are unreachable in the shipped
  // firmware, but the implementations and their PID tables were still compiled
  // into it. Unreachable is not the same as absent: the code, the candidate DID
  // lists and the sweep ranges all shipped, on a device whose OBD link is
  // read-only by design. Gating the implementations too removes them from the
  // binary rather than merely from the menu.
  //
  // `runScan` alone sweeps 544 PIDs and monopolises the OBD link for minutes —
  // on a moving vehicle that is a denial of the gauges, which is why discovery
  // is a bench activity with its own env (crowpanel_obd_dev).
#if defined(OBD_DEV_CONSOLE)
  void runDiag();            // raw-dump EGT/DPF PIDs to Serial (diagnostic)
  void runScan();            // sweep 2200xx/2219xx + candidate PIDs, log positives (diagnostic)
  void runDefProbe();        // raw-dump DEF candidate PIDs to Serial (diagnostic)
  void runGearOilProbe();    // ~20-sample live dump of gear + oil candidates (diagnostic)
  // Send one command at a header, accumulate the reply to the prompt, return it
  // CR/LF-escaped ("" if nothing). Shared by runDiag and runScan.
  std::string probePid(const char* sh, const char* cmd);
  // Like probePid but drops the AT-SH "OK" residual before sending the PID and
  // uses wider deadlines — returns the RAW (unescaped) PID reply for parsing.
  std::string probeFrame(const char* sh, const char* cmd);
#endif

  // BLE transport shims used by pollQuery — the seam the templated query engine
  // writes/reads through, so it stays transport-agnostic.
  void bleWrite(const char* s);
  int  bleAvailable();
  int  bleRead();            // -1 if empty
  bool atInit(const char* cmd, uint32_t timeoutMs);   // send + drain to '>'
};

#else
// Stub — never instantiated on non-BLE builds; must parse cleanly without NimBLE.
class BleObdSource : public ObdSource {
 public:
  void begin() override {}
  void poll(uint32_t) override {}
  ObdReadings latest() const override { return cur_; }
  void setDieselTankGal(float) {}    // no-op: keeps main.cpp board-agnostic
 private:
  ObdReadings cur_;
};
#endif
