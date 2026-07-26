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
