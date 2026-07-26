#pragma once
#include <string>
#include "obd_source.h"

// real_obd_source.h — Live OBD source over classic-Bluetooth ELM327
//   (Vgate vLinker MS → BluetoothSerial/SPP → ELMduino).
//
// MOCK_OBD=1 (elecrow env): the heavy BT/ELM/NVS headers are not in lib_deps,
//   so we expose only a no-op stub class that satisfies the ObdSource interface.
//   main.cpp selects MockObdSource at compile time, so this stub is never
//   instantiated, but it must at least parse cleanly.
//
// MOCK_OBD undefined (elecrow_obd env): full implementation included.

#if !defined(MOCK_OBD) && !defined(BLE_OBD)
// ──────────────────────────────────────────────────────────────────────────────
// Real classic-BT build — full headers + implementation. (BLE_OBD builds use
// BleObdSource instead; the S3 has no BluetoothSerial, so skip this classic-BT
// path there.)
// ──────────────────────────────────────────────────────────────────────────────
#include <vector>
#include <Arduino.h>
#include <BluetoothSerial.h>
#include <Preferences.h>
#include <ELMduino.h>
#include "obd_schedule.h"
#include "obd_query.h"
#include "readouts.h"
#include "economy.h"

// Live OBD source over classic-Bluetooth ELM327 (Vgate vLinker MS).
// Runs in the core-0 IO task; poll() is non-blocking. latest() is safe to call
// from the render core — cur_ is guarded by a FreeRTOS spinlock (portMUX_TYPE).
//
// Task split:
//   Task 3 (this): BT connect, NVS MAC, link state, spinlock-guarded cur_.
//   Task 4: pairing UI — doPairing() console scan/select flow.
//   Task 5: PID queries — pollQuery() ELMduino non-blocking reads.
class RealObdSource : public ObdSource {
 public:
  void begin() override;
  void poll(uint32_t nowMs) override;
  ObdReadings latest() const override;   // by value: see ObdSource::latest()

  // Console pairing hooks (implemented in Task 4).
  void requestPair();       // set pairReq_ flag; poll() drives the flow
  void forget();            // sets forgetReq_; poll() clears NVS MAC on core 0
  bool pairing() const;     // true while the scan/select flow owns the console
  void resetTrip() { resetTripReq_ = true; }   // menu hook — marshalled to core 0

 private:
  // Connection state machine.
  enum class Conn { Unpaired, Connecting, Up };

  BluetoothSerial bt_;      // classic SPP master
  ELM327          elm_;     // ELM327 handshake layer over bt_
  Preferences     nvs_;     // NVS namespace "obd" — stores "addr" + "name"
  ObdSchedule     sched_;   // query tier scheduler (table-driven)
  ObdReadings     cur_;     // live readings — guarded by mux_
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;  // spinlock for cur_

  Conn      conn_        = Conn::Unpaired;
  String    addr_;            // "AA:BB:CC:DD:EE:FF" from NVS ("" = none)
  uint32_t  nextRetryMs_ = 0; // retry backoff timestamp
  volatile bool pairReq_  = false;  // requestPair() sets this
  volatile bool pairing_  = false;  // true while doPairing() runs (Task 4)
  // Marshalled requests: set on core 1 ('f' key / menu), consumed in poll() on
  // core 0. forget() used to mutate addr_/conn_/NVS directly from core 1 while
  // tryConnect() was reading addr_.c_str() on core 0 — a String use-after-free.
  volatile bool forgetReq_    = false;
  volatile bool resetTripReq_ = false;

  // VIN read runs once per connected session (Conn::Up success block); cleared
  // on disconnect so a reconnect re-reads (e.g. after an adapter/vehicle swap).
  bool vinRead_ = false;

  Economy economy_;           // trip MPG integrator (computed readouts)

  // Load MAC from NVS into addr_; returns true if a valid address is stored.
  bool loadMac();

  // Attempt a BT connect + ELM handshake; called from poll() in Connecting state.
  void tryConnect(uint32_t nowMs);

  // Task 4: console scan/select pairing flow.
  void doPairing();

  // Task 5: issue one non-blocking query step per poll() call.
  void pollQuery(uint32_t nowMs);

  // Non-blocking query transaction state — shared engine (obd_query.h).
  PidQueryState q_;
  float         values_[STAT_COUNT] = {};     // DecodeCtx backing store; feeds decode fns
  // Transport adapter handed to pidQueryStep (nested type -> private access).
  struct QueryIo {
    RealObdSource* s;
    void write(const char* c) { s->bt_.print(c); }
    int  available()          { return s->bt_.available(); }
    int  read()               { return s->bt_.read(); }
    void flushRx()            { while (s->bt_.available()) s->bt_.read(); }
  };
};

#else
// ──────────────────────────────────────────────────────────────────────────────
// Mock build — minimal parse-only stub; never instantiated (MockObdSource is
// used instead), but must compile cleanly without BT/ELM/NVS headers.
// ──────────────────────────────────────────────────────────────────────────────
class RealObdSource : public ObdSource {
 public:
  void begin() override {}
  void poll(uint32_t) override {}
  ObdReadings latest() const override { return cur_; }
 private:
  ObdReadings cur_;
};

#endif  // MOCK_OBD
