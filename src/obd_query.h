#pragma once
// Shared non-blocking PID query state machine — the single implementation
// behind BleObdSource (NimBLE, S3 board) and RealObdSource (classic BT,
// retired WROVER board). The two copies had drifted only in transport calls
// and reply timeout; keeping one engine stops future divergence.
//
// Flow (one step per poll() tick):
//   Idle -> flush rx, pick next readout; if its ECU header differs from the
//   last one, send AT SH as its OWN step (WaitHeader) so the "OK>" ack is
//   consumed before the query — sending both back-to-back made the reader
//   stop at the header's '>' and never see the data reply (all gauges "--").
//   WaitData accumulates to '>', parses, decodes via the table, and stores
//   under the caller's spinlock. Timeout abandons the step (last good kept).
//
// IO policy (duck-typed, zero-overhead): the caller passes an adapter with
//   void write(const char*);  int available();  int read();  void flushRx();
// Device-only: include from inside the source's build guard (uses portMUX).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "obd_source.h"     // ObdReadings
#include "obd_schedule.h"
#include "obd_parse.h"
#include "readouts.h"       // READOUTS / DecodeCtx / isDisplayed
#include "vehicle_profile.h" // VEHICLE (tank capacities)
#include "vehicle_active.h" // VEHICLE/READOUTS/READOUT_COUNT macros over g_activeProfile
#include "economy.h"        // trip-MPG integrator (updateComputedReadouts)
#include "pid_decode.h"     // computeHorsepower / gallonsToFill

enum class Tx { Idle, WaitHeader, WaitData };

struct PidQueryState {
  Tx          txState   = Tx::Idle;
  int         curIdx    = 0;      // readout table index being queried
  int         curHeader = -1;     // last header index selected; -1 forces re-send
  int         setupStep = 0;      // next step to hand to addressing[].emit()
  uint32_t    deadlineMs = 0;
  std::string rxBuf;              // accumulates ELM ASCII reply bytes
};

#include "can29_ecm_addr.h"

template <typename IO>
inline void pidQueryStep(PidQueryState& q, ObdSchedule& sched, IO& io,
                         uint32_t nowMs, uint32_t replyTimeoutMs,
                         float* values, ObdReadings& cur, portMUX_TYPE& mux,
                         bool can29 = false) {
  if (q.txState == Tx::Idle) {
    q.curIdx = sched.next();
    const ReadoutDef& r = READOUTS[q.curIdx];
    io.flushRx();                 // drop stale bytes from a timed-out prior query (desync guard)
    q.rxBuf.clear();
    if (r.header != q.curHeader) {
      q.curHeader = r.header;
      q.setupStep = 0;
      static char shBuf[24];        // core-0 query task only — no concurrent callers
      const char* setup = VEHICLE.addressing[r.header].emit(q.setupStep++, can29, shBuf, sizeof shBuf);
      if (setup) {
        io.write(setup);
        q.deadlineMs = nowMs + replyTimeoutMs;
        q.txState = Tx::WaitHeader;
        return;
      }
      // no setup commands: fall through and send the query immediately
    }
    char b[16]; snprintf(b, sizeof b, "%s\r", r.cmd);
    io.write(b);
    q.deadlineMs = nowMs + replyTimeoutMs;
    q.txState = Tx::WaitData;
    return;
  }

  if (q.txState == Tx::WaitHeader) {
    while (io.available()) {
      char c = (char)io.read();
      if (c == '>') {               // one setup step acked — send the next, or the query
        static char shBuf[24];
        const char* setup = VEHICLE.addressing[q.curHeader].emit(q.setupStep++, can29, shBuf, sizeof shBuf);
        if (setup) {
          io.write(setup);
          q.deadlineMs = nowMs + replyTimeoutMs;
          return;                   // stay in WaitHeader for the next ack
        }
        q.rxBuf.clear();
        char b[16]; snprintf(b, sizeof b, "%s\r", READOUTS[q.curIdx].cmd);
        io.write(b);
        q.deadlineMs = nowMs + replyTimeoutMs;
        q.txState = Tx::WaitData;
        return;
      }
    }
    // SIGNED delta (wrap-safe): a raw `nowMs >= deadline` misbehaves for weeks
    // around the 49.7-day millis() wrap — same idiom as obd_source.h:74.
    if ((int32_t)(nowMs - q.deadlineMs) >= 0) { q.curHeader = -1; q.txState = Tx::Idle; }  // re-set header next time
    return;
  }

  // WaitData: drain available bytes without blocking.
  while (io.available()) {
    char c = (char)io.read();
    q.rxBuf += c;
    if (q.rxBuf.size() > 1024) { q.rxBuf.clear(); q.txState = Tx::Idle; return; }  // flood guard: no '>' -> bail
    if (c == '>') {
      const ReadoutDef& r = READOUTS[q.curIdx];
      const char* cmd = r.cmd;
      uint8_t  mode = (cmd[0] == '2' && cmd[1] == '2') ? 0x22 : 0x01;
      uint16_t pid  = (uint16_t)strtol(cmd + 2, nullptr, 16);
      std::vector<uint8_t> d;
      if (parseObdResponse(q.rxBuf, mode, pid, d) && !d.empty()) {
        DecodeCtx ctx{values};
        float v = r.decode(d.data(), (int)d.size(), ctx);
        if (v == v) {             // skip NaN: decoder flagged a bad/short frame, keep last good
          portENTER_CRITICAL(&mux);
          values[q.curIdx] = v;   // backing store always (feeds ctx for other decodes)
          // Publish EVERY polled row to cur, hidden helpers included: the SD CSV
          // logs active stats, and an isDisplayed() gate here left the helper
          // columns (BARO/TORQUE/RefTq/OUTSPD) permanently blank (valid[] never
          // set — found on the 2026-07-17 probe drive). Publishing helpers is
          // invisible on screen (the UI renders only PAGES cells) and they carry
          // no thresholds, so the alarm scan ignores them.
          cur.v[q.curIdx] = v;
          cur.valid[q.curIdx] = true;
          cur.ms[q.curIdx] = nowMs;   // freshness stamp for staleness detection
          portEXIT_CRITICAL(&mux);
        }
      }
      q.txState = Tx::Idle;
      return;
    }
  }
  if ((int32_t)(nowMs - q.deadlineMs) >= 0) q.txState = Tx::Idle;   // abandon (wrap-safe); last good value stays
}

// ---------------------------------------------------------------------------
// Computed readouts — the SINGLE implementation shared by all live sources
// (BLE / WiFi / classic-BT). These rows are derived on-device from PIDs already
// in values[] (no extra queries): trip economy, horsepower, gallons-to-fill.
// Call once per poll() tick while the link is Up, right after pidQueryStep.
// Runs on the core-0 OBD task only (economy is single-writer); publishes to cur
// under the caller's spinlock in ONE critical section.
// ---------------------------------------------------------------------------

// Tank capacities for the "gallons to fill" tiles come from the active
// vehicle profile (VEHICLE.dieselTankGal / defTankGal).

inline void updateComputedReadouts(Economy& econ, float* values, ObdReadings& cur,
                                   portMUX_TYPE& mux, uint32_t nowMs) {
  // Compute everything OUTSIDE the lock (core 0 is the only values[] writer).
  econ.update(values[(int)StatId::FuelRate], values[(int)StatId::Speed], nowMs);
  float mi   = econ.instantMpg(),     ma   = econ.avgMpg();
  float g100 = econ.avgGalPer100mi(), l100 = econ.avgLPer100km();
  bool  av   = econ.valid();

  // HP from the hidden torque helpers + RPM.
  float hp = computeHorsepower(values[(int)StatId::ActTq], values[(int)StatId::RefTq],
                               values[(int)StatId::Rpm]);
  bool hpValid = values[(int)StatId::RefTq] > 0.0f && values[(int)StatId::Rpm] > 0.0f;

  // Tank "gallons to fill" from level % + capacity.
  float dsl = gallonsToFill(VEHICLE.dieselTankGal, values[(int)StatId::FuelLevel]);
  float def = gallonsToFill(VEHICLE.defTankGal,    values[(int)StatId::Def]);

  portENTER_CRITICAL(&mux);
  values[(int)StatId::MpgInst]  = mi;   values[(int)StatId::MpgAvg]  = ma;
  values[(int)StatId::Gal100mi] = g100; values[(int)StatId::L100km]  = l100;
  cur.v[(int)StatId::MpgInst]  = mi;   cur.valid[(int)StatId::MpgInst]  = av;
  cur.v[(int)StatId::MpgAvg]   = ma;   cur.valid[(int)StatId::MpgAvg]   = av;
  cur.v[(int)StatId::Gal100mi] = g100; cur.valid[(int)StatId::Gal100mi] = av;
  cur.v[(int)StatId::L100km]   = l100; cur.valid[(int)StatId::L100km]   = av;
  // Computed live each cycle from current inputs -> fresh while the link runs.
  cur.ms[(int)StatId::MpgInst] = cur.ms[(int)StatId::MpgAvg] =
  cur.ms[(int)StatId::Gal100mi] = cur.ms[(int)StatId::L100km] = nowMs;

  values[(int)StatId::Hp] = hp;
  cur.v[(int)StatId::Hp] = hp; cur.valid[(int)StatId::Hp] = hpValid;
  cur.ms[(int)StatId::Hp] = nowMs;

  values[(int)StatId::DslFill] = dsl; values[(int)StatId::DefFill] = def;
  cur.v[(int)StatId::DslFill] = dsl; cur.valid[(int)StatId::DslFill] = cur.valid[(int)StatId::FuelLevel];
  cur.v[(int)StatId::DefFill] = def; cur.valid[(int)StatId::DefFill] = cur.valid[(int)StatId::Def];
  cur.ms[(int)StatId::DslFill] = cur.ms[(int)StatId::FuelLevel];   // inherit source freshness
  cur.ms[(int)StatId::DefFill] = cur.ms[(int)StatId::Def];
  portEXIT_CRITICAL(&mux);
}
