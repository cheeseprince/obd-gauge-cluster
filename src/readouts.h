#pragma once
#include <cstdint>
#include "gauge_model.h"   // Thresholds
#include "app_types.h"     // StatId, STAT_COUNT

// Physical quantity a readout measures — drives optional metric conversion at
// display time. Only Temp and Speed convert; everything else is None.
// Metric-conversion classes. Economy tiles (MPG/GAL·100/L·100km) are Quantity::None
// BY DESIGN — the TRIP page shows both systems as separate tiles, so the Units
// toggle must not touch them. DPF dP is kPa-native in both modes (also None).
enum class Quantity : uint8_t { None, Temp, Speed,
                                Press,    // psi -> kPa   (OIL P, BOOST)
                                PressHi,  // psi -> MPa   (RAIL — 5-digit kPa would be unreadable)
                                Vol,      // gal -> L     (DSL+, DEF+)
                                Flow };   // gph -> L/h   (FUEL rate)

// Current value of every readout, by index (StatId). Lets a decode read another
// readout's value (e.g. boost = MAP - baro). Stale-but-valid is fine.
struct DecodeCtx { const float* values; };

// Per-stat behavior flags (profile-owned).
inline constexpr uint8_t RF_COMPUTED         = 1 << 0; // filled by updateComputedReadouts(); decode never called; cmd==nullptr
inline constexpr uint8_t RF_LOW_NEEDS_ENGINE = 1 << 1; // low alarm armed only after sustained engine running (gauge_model lowArm)

struct ReadoutDef {
  // STABLE LOG KEY, not the on-screen text — e.g. "TRANS". This string is the
  // SD-log CSV column header (sd_log.cpp buildHeaderRow), so tools/analyze_logs.py
  // and every card already recorded depend on it: changing it forks the log
  // schema. The on-screen text is statLabel() below and may be changed freely.
  const char* name;
  const char* unit;        // "\xC2\xB0""F", "psi", "V", "mph", "" (none)
  uint8_t     decimals;    // display precision
  Thresholds  thr;         // alarm limits (NAN disables a bound)
  float       fullScale;   // trend/bar full-scale
  const char* cmd;         // ELM command: "0105","221940","22000B"; nullptr = computed (RF_COMPUTED) or unsupported
  uint8_t     header;      // 0=7DF, 1=7E2, 2=7E0
  uint8_t     tier;        // 0=fast, 1=slow, 2=rare (scheduler)
  float (*decode)(const uint8_t* d, int n, const DecodeCtx& ctx);
  Quantity quantity;       // metric-conversion class (None = no conversion)
  uint8_t  flags = 0;      // RF_* bits; rows that omit it get 0 (default member init keeps -Wextra quiet)
};

constexpr int           IDX_BARO = (int)StatId::Baro;

// Layout-derived helpers (backed by the active profile's LayoutDef — see
// vehicle_profile.h).
int readoutPageCount();             // number of display pages
int readoutAt(int page, int cell);  // displayed readout index at (page,cell), or -1
int readoutPageOf(int idx);         // display page holding readout idx (0 if not displayed)
bool isDisplayed(int idx);          // true if the stat occupies a cell in the layout
bool isActive(int idx);             // true if displayed OR a queried HELPER (else deactivated)
const char* pageName(int page);     // page name for the header bar ("" out of range)

// On-screen text for a stat, by StatId index — the quad cell, the focus title
// and the alarm overlay. Deliberately NOT ReadoutDef::name: that string is the
// CSV log key and must stay stable, while this one is free to read as plain
// English. Shared across every vehicle profile (StatId is a global enum in
// StatId order), so a stat cannot end up worded two ways on two vehicles.
// Every string is width-checked against the panel — see readouts.cpp.
const char* statLabel(int idx);

// Display-time unit conversion. Canonical values are imperial (°F, mph); when
// metric is true, Temp converts °F->°C and Speed mph->km/h. Everything else is
// returned unchanged. Alarms/thresholds/history stay canonical — these affect
// only the printed number and unit label.
// Per-stat alarm zone with cross-stat gating (OIL P low suppressed engine-off).
Zone zoneForStat(const GaugeSet& gs, int idx);
float       toDisplayValue(Quantity q, float canonical, bool metric);
const char* displayUnit(const ReadoutDef& r, bool metric);
