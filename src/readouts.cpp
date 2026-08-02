// Vehicle-blind facade over the active VehicleProfile: layout accessors,
// alarm-zone gating, display-time unit conversion. Everything vehicle-specific
// (the READOUTS table, decoders, default layout) lives in src/vehicles/*.cpp.
#include "readouts.h"
#include "vehicle_profile.h"
#include "vehicle_active.h"

// Layout selection: the profile's default, unless the env defines USER_LAYOUT
// and provides src/user_layout.h (a LayoutDef named USER_LAYOUT_DEF) — the
// "my screens, any vehicle" override (see vehicle_profile.h).
#if defined(USER_LAYOUT)
#include "user_layout.h"     // must define: extern const LayoutDef USER_LAYOUT_DEF
static const LayoutDef& activeLayout() { return USER_LAYOUT_DEF; }
#else
static const LayoutDef& activeLayout() { return VEHICLE.defaultLayout; }
#endif

// --- Alarm-zone helper with cross-stat gating ---
// Alarms flagged RF_LOW_NEEDS_ENGINE (OIL P on the GM profile) are meaningless
// when the engine isn't turning the pump (auto stop-start parks pressure at a
// legitimate 0 psi). Gated on the gauge_model arm-delay (20s of sustained fresh
// RPM — see lowArmTick): a fuel-flow gate was tried first and failed on log
// replay — FUEL is slow-tier (~35s stale), so it missed shutdown transitions
// and key-on windows (11 false fires reconstructed from the 2026-07-17 logs).
// Flagged stats carry low-only thresholds, so gating every non-green zone is
// equivalent to gating the low bounds.
Zone zoneForStat(const GaugeSet& gs, int idx) {
  Zone z = zoneFor(gs.g[idx].value, READOUTS[idx].thr);
  if ((READOUTS[idx].flags & RF_LOW_NEEDS_ENGINE) && z != Zone::Green && !lowAlarmArmed())
    return Zone::Green;                       // engine not confirmed running
  return z;
}

// --- Display-time unit conversion ---

float toDisplayValue(Quantity q, float canonical, bool metric) {
  if (!metric) return canonical;
  switch (q) {
    case Quantity::Temp:    return (canonical - 32.0f) / 1.8f;   // °F -> °C
    case Quantity::Speed:   return canonical * 1.609344f;         // mph -> km/h
    case Quantity::Press:   return canonical * 6.89476f;          // psi -> kPa
    case Quantity::PressHi: return canonical * 0.00689476f;       // psi -> MPa
    case Quantity::Vol:     return canonical * 3.78541f;          // gal -> L
    case Quantity::Flow:    return canonical * 3.78541f;          // gph -> L/h
    default:                return canonical;
  }
}

const char* displayUnit(const ReadoutDef& r, bool metric) {
  if (metric) {
    switch (r.quantity) {
      case Quantity::Temp:    return "\xC2\xB0""C";
      case Quantity::Speed:   return "km/h";
      case Quantity::Press:   return "kPa";
      case Quantity::PressHi: return "MPa";
      case Quantity::Vol:     return "L";
      case Quantity::Flow:    return "L/h";
      default: break;
    }
  }
  return r.unit;
}

// --- layout-derived helpers (backed by the active profile's LayoutDef) ---

int readoutPageCount() { return activeLayout().pageCount; }

// Index of the stat at (page,cell), or -1 if out of range / empty cell.
int readoutAt(int page, int cell) {
  const LayoutDef& L = activeLayout();
  if (page < 0 || page >= L.pageCount || cell < 0 || cell >= 4) return -1;
  StatId s = L.pages[page][cell];
  return s == StatId::COUNT ? -1 : (int)s;
}

// Page holding the stat, or 0 if not displayed (preserves prior hidden->0 behavior).
int readoutPageOf(int idx) {
  const LayoutDef& L = activeLayout();
  for (int p = 0; p < L.pageCount; p++)
    for (int c = 0; c < 4; c++)
      if (L.pages[p][c] == (StatId)idx) return p;
  return 0;
}

// True if the stat occupies a cell in the active layout.
bool isDisplayed(int idx) {
  const LayoutDef& L = activeLayout();
  for (int p = 0; p < L.pageCount; p++)
    for (int c = 0; c < 4; c++)
      if (L.pages[p][c] == (StatId)idx) return true;
  return false;
}

// True if displayed OR a queried HELPER (else deactivated -> not polled).
bool isActive(int idx) {
  if (isDisplayed(idx)) return true;
  const LayoutDef& L = activeLayout();
  for (int h = 0; h < L.helperCount; h++)
    if (L.helpers[h] == (StatId)idx) return true;
  return false;
}

// Short page name for the header bar ("" out of range).
const char* pageName(int page) {
  const LayoutDef& L = activeLayout();
  return (page >= 0 && page < L.pageCount) ? L.pageNames[page] : "";
}

// --- On-screen stat labels ---
// Plain English, not abbreviations: the dash is read at a glance from the
// driver's seat by someone who did not write the firmware. Kept here rather
// than in the per-vehicle tables because a stat is the same physical quantity
// on every vehicle — one table means "coolant" cannot be COOLANT on the Sierra
// and CLNT on the BMW. ReadoutDef::name stays the short CSV log key.
//
// WIDTH IS A HARD CONSTRAINT — these strings are drawn in four places, and the
// alarm overlay (not the tile) is the tightest. Measured budgets on the 480x320
// panel, using LVGL's own text engine against the real widget geometry:
//   quad tile name   montserrat_20, 204 px  (236 px grid column - 32 px theme pad)
//   focus title      montserrat_28, 298 px  (must clear the SD status label)
//   page band        montserrat_20, 298 px  (must clear the clock and SD labels)
//   alarm line       montserrat_28, 464 px  for the whole "<label> CRITICAL 1450°F"
// A label that overflows is clipped, not wrapped. "EXHAUST TEMP" was rejected
// for exactly this reason: it fits the tile at 159 px but its critical line
// renders 467 px, 3 px past the panel edge. Re-measure before widening any of
// these — tools/ui_snapshot builds the real UI on the host.
//
// No "TEMP" suffix where the tile already prints °F/°C: the unit says it, and
// the space buys plain words elsewhere.
static const char* const STAT_LABELS[] = {
  "TRANSMISSION",   // Trans      (was TRANS)
  "OIL",            // Oil        — °F distinguishes it from OIL PRESSURE
  "BOOST",          // Boost
  "COOLANT",        // Coolant
  "VOLTAGE",        // Volts      (was VOLTS)
  "INTAKE",         // Intake
  "RPM",            // Rpm        — expanding this reads worse than the acronym
  "SPEED",          // Speed
  "EXHAUST GAS",    // Egt        (was EGT)
  "DPF PRESSURE",   // DpfDp      (was DPF dP)
  "FUEL RATE",      // FuelRate   (was FUEL / FUEL RATE — unified)
  "ENGINE LOAD",    // Load       (was LOAD)
  "MPG",            // MpgInst    — the unit is the name
  "AVERAGE MPG",    // MpgAvg     (was MPG AVG / AVG MPG — unified)
  "GAL/100 MI",     // Gal100mi   (was GAL/100)
  "L/100 KM",       // L100km     (was L/100km)
  "RAIL PRESSURE",  // Rail       (was RAIL)
  "HORSEPOWER",     // Hp         (was HP)
  "DEF LEVEL",      // Def        — DEF stays: it is the fluid's actual name
  "FUEL LEVEL",     // FuelLevel  (was FUEL%)
  "DIESEL FILL",    // DslFill    (was DSL+ / DSL FILL — unified)
  "DEF FILL",       // DefFill    (was DEF+ / DEF FILL — unified)
  "ACTUAL TORQUE",  // ActTq      (was TORQUE / ACT TQ — unified)
  "REF TORQUE",     // RefTq      (was RefTq / REF TQ — unified)
  "BAROMETRIC",     // Baro       (was BARO)
  "AIR FLOW",       // Maf        (was MAF)
  "AMBIENT",        // Ambient
  "EGR VALVE",      // Egr        (was EGR — no short plain-English expansion)
  "ACCEL PEDAL",    // Pedal      (was PEDAL)
  "CHARGE AIR",     // Cac        (was CAC)
  "NOx",            // Nox        — chemical formula, not an abbreviation
  "OIL PRESSURE",   // OilP       (was OIL P)
  "GEAR",           // Gear       — logged only, never displayed
};
// A label added or removed without matching StatId desyncs every label from its
// stat — silently, since both are just indices. Catch it at compile time.
static_assert(sizeof(STAT_LABELS)/sizeof(STAT_LABELS[0]) == (size_t)STAT_COUNT,
              "STAT_LABELS must have one entry per StatId, in StatId order");

const char* statLabel(int idx) {
  return (idx >= 0 && idx < STAT_COUNT) ? STAT_LABELS[idx] : "";
}
