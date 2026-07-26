#pragma once
#include <cstdint>
#include "app_types.h"
#include "readouts.h"   // READOUTS[].tier (per-tier staleness window)
#include "vehicle_active.h"  // READOUTS macro over g_activeProfile (applyReadings() below)

// Connection progress for the "connecting" overlay (BLE source fills it; others
// use the default Idle status).
enum class ConnPhase : uint8_t { Idle, Scanning, Connecting, Initializing, Up };
struct ConnStatus {
  ConnPhase   phase    = ConnPhase::Idle;
  uint16_t    attempts = 0;       // connect attempts since boot
  uint32_t    sinceMs  = 0;       // millis() when the current phase began
  const char* addr     = "";      // saved adapter address, "" if none
};

// Build the 4-line connecting overlay text into out. now = current millis().
void formatConnStatus(const ConnStatus& cs, uint32_t now, char* out, int outSize);

struct ObdReadings {
  float    v[STAT_COUNT] = {0};
  bool     valid[STAT_COUNT] = {false};  // true once a real reading has arrived for stat i
  uint32_t ms[STAT_COUNT] = {0};         // millis() of stat i's last FRESH (non-NaN) store
  bool     linkUp = false;
  char     vin[18] = "";                 // VIN read on connect (empty until read)
};

class ObdSource {
 public:
  virtual ~ObdSource() = default;
  virtual void begin() = 0;
  virtual void poll(uint32_t nowMs) = 0;        // refresh latest() (nowMs = millis on device)
  // BY VALUE on purpose: latest() is called from both cores (core-0 logTick,
  // core-1 render). A shared static snapshot returned by reference was a data
  // race — each caller gets its own copy (~300 B, negligible).
  virtual ObdReadings latest() const = 0;
  virtual ConnStatus connStatus() const { return {}; }   // default: Idle/0
};

// Per-tier "blank" timeout (ms) — how long we keep showing the last-good value before
// drawing "--". This is DECOUPLED from the poll rate (also the tier): its only job is to
// catch a genuinely DEAD PID. Only the fast tier (RPM/speed/boost/rail/torque/MAF/pedal)
// changes second-to-second, so a frozen value there misleads quickly -> tighter window.
// Slow/rare stats change over minutes, so their last-good value stays accurate for a long
// time; they effectively "hold while linked" and only blank if a PID is dead for the whole
// window (or the link drops, which swaps to the connecting screen anyway). Generous windows
// are required because a marginal BLE frame decodes to NaN (skipped, no fresh stamp), which
// stretches a healthy stat's clean-frame gap — tight windows flickered "--" while driving.
inline uint32_t staleMsForTier(uint8_t tier) {
  return tier == 0 ? 120000u : tier == 1 ? 1800000u : 3600000u;   // 2min / 30min / 60min
}

// Push the source's latest values into the gauge set (value + peak).
//
// HOLD-WHILE-LINKED: as long as the BLE link is up, a stat that has ever read keeps
// showing its last-good value — it NEVER blanks to "--". This is the flicker fix: a
// healthy stat's clean-frame gap can exceed any per-tier window because marginal BLE
// frames decode to NaN and are skipped (no fresh stamp), and the poll schedule (~31 PIDs)
// revisits each stat slowly — so a tight window blinked "--" mid-drive even though the
// link was fine and the held value was still accurate.
//
// DEAD-PID GUARD: holding forever would let a permanently dead PID (ECU stops
// answering, or every frame decodes to NaN) masquerade as a live reading — e.g. a
// frozen "safe" trans temp suppressing the overheat alarm mid-tow. So while linked,
// a stat past its tier window is HELD but marked STALE: the UI renders it grey (no
// blank/flicker) and the alarm scan skips it (no new alarms from frozen data, and a
// value frozen in an alarm zone can't latch red forever). A fresh read clears stale.
// When the link is DOWN the per-tier window blanks as before (the connecting overlay
// covers the screen anyway). A never-read stat (valid==false) stays "--".
inline void applyReadings(GaugeSet& s, const ObdReadings& r, uint32_t now) {
  // OIL P alarm arming: confirmed-running = link up + FRESH RPM at running speed
  // (freshness via the poll stamp — a held stale RPM must disarm; see gauge_model.h).
  {
    const int rpm = (int)StatId::Rpm;
    bool rpmFresh = r.valid[rpm] && (int32_t)(now - r.ms[rpm]) < (int32_t)LOWARM_RPM_FRESH_MS;
    lowArmTick(r.linkUp && rpmFresh && r.v[rpm] >= LOWARM_RPM, now);
  }
  for (int i = 0; i < STAT_COUNT; i++) {
    if (!r.valid[i]) {                              // never read -> "--"
      s.g[i].valid = false; s.g[i].stale = false;
      continue;
    }
    // SIGNED delta: core-0's poll stamps r.ms[i] from its own millis(), which can be a
    // few ms AHEAD of the `now` core-1 captured at the top of loop(). An unsigned
    // (now - ms) would underflow to ~4.29e9 there and falsely mark the PID stale,
    // flickering it grey every other frame. Signed keeps a just-ahead stamp fresh.
    bool inWindow = (int32_t)(now - r.ms[i]) < (int32_t)staleMsForTier(READOUTS[i].tier);
    if (r.linkUp) {
      gaugeUpdate(s.g[i], r.v[i]);                  // hold last-good, never blank
      s.g[i].stale = !inWindow;                     // dead PID -> grey + no alarms
    } else if (inWindow) {
      gaugeUpdate(s.g[i], r.v[i]);
      s.g[i].stale = false;
    } else {                                        // link down + past window -> "--"
      s.g[i].valid = false; s.g[i].stale = false;
    }
  }
}
