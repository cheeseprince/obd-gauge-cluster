// ============================================================================
//  Generic OBD-II profile — standard SAE Mode-01 PIDs only, usable on any car.
//  This is the DEFAULT profile and the unknown-VIN fallback: no enhanced/
//  manufacturer addressing, no diesel-specific PIDs. Everything vehicle-blind
//  the firmware reads through VEHICLE (vehicle_profile.h). Row order MUST
//  match StatId enum order, same convention as gm_sierra_lz0.cpp/bmw_f10_535i.cpp.
// ============================================================================
#include "readouts.h"
#include "pid_decode.h"
#include "vehicle_profile.h"
#include <cstdio>

// --- decode functions: thin wrappers over host-tested pid_decode ---
// Every wrapper returns NaN on a short/sentinel/implausible frame; pollQuery
// skips NaN and keeps the last good value (same dropout-sentinel policy as
// the GM/BMW profiles: 0xFF is the common ELM "no data" pad).
static float gTempF(const uint8_t* d, int n, const DecodeCtx&) { return (n>0 && d[0]!=0xFF) ? cToF(decodeTempC(d[0])) : NAN; }
static float gVolts(const uint8_t* d, int n, const DecodeCtx&) { if (n<1) return NAN; float v=decodeModuleVolts(d[0], n>1?d[1]:0); return (v<6.0f||v>18.0f)?NAN:v; }
static float gRpm  (const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeRpm(d[0], d[1]) : NAN; }
static float gSpeed(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? kphToMph(decodeSpeedKph(d[0])) : NAN; }
static float gLoad (const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeLoadPct(d[0]) : NAN; }
static float gMaf  (const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeMafGps(d[0], d[1]) : NAN; }
static float gNone (const uint8_t*, int, const DecodeCtx&)     { return NAN; }

// Verified against gm_sierra_lz0.cpp / bmw_f10_535i.cpp: T()/NA are NOT
// provided by readouts.h — every profile TU defines them locally.
#define T(wh,ch,wl,cl) Thresholds{wh,ch,wl,cl}
static const float NA = NAN;

// Rows are in StatId order. Only standard-PID stats carry a cmd; the rest are
// inactive (cmd nullptr, no flags) and never appear in the layout.
static const ReadoutDef GENERIC_READOUTS[] = {
  // name      unit           dec thr              full   cmd     hdr tier decode  quantity
  {"TRANS",   "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  nullptr, 0,  1, gNone,  Quantity::Temp},   // Trans — not standard PID
  {"OIL",     "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  nullptr, 0,  1, gNone,  Quantity::Temp},   // Oil — not standard PID
  {"BOOST",   "psi",          1,  T(NA,NA,NA,NA),  30,   nullptr, 0,  0, gNone,  Quantity::Press},  // Boost — not standard PID
  // Coolant (0105). Threshold kept on the "any car" profile deliberately: 0105
  // is a standard, well-defined SAE PID (not an unverified manufacturer DID), and
  // 235/250 F is a conservative universal overheat line (most gas/diesel engines
  // run 195-220 F; sustained >235 F is trouble on essentially any engine). Warn-
  // only, no low thresholds. This is the sole intentional alarm on Generic.
  {"COOLANT", "\xC2\xB0""F",  0,  T(235,250,NA,NA),300,  "0105",  0,  1, gTempF, Quantity::Temp},
  {"VOLTS",   "V",            1,  T(NA,NA,11.0f,10.2f),18,"0142", 0,  1, gVolts, Quantity::None},   // Volts (0142)
  {"INTAKE",  "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  "010F",  0,  1, gTempF, Quantity::Temp},   // Intake air temp (010F)
  {"RPM",     "",             0,  T(NA,NA,NA,NA),  7000, "010C",  0,  0, gRpm,   Quantity::None},   // Rpm (010C)
  {"SPEED",   "mph",          0,  T(NA,NA,NA,NA),  120,  "010D",  0,  0, gSpeed, Quantity::Speed},  // Speed (010D)
  {"EGT",     "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  1600, nullptr, 0,  1, gNone,  Quantity::Temp},   // Egt — diesel, not standard PID
  {"DPF dP",  "psi",          1,  T(NA,NA,NA,NA),  15,   nullptr, 0,  2, gNone,  Quantity::Press},  // DpfDp — diesel, not standard PID
  {"FUEL RATE","gph",         1,  T(NA,NA,NA,NA),  20,   nullptr, 0,  1, gNone,  Quantity::Flow},   // FuelRate — not standard PID
  {"LOAD",    "%",            0,  T(NA,NA,NA,NA),  100,  "0104",  0,  0, gLoad,  Quantity::None},   // Load (0104 calculated engine load)
  {"MPG",     "mpg",          1,  T(NA,NA,NA,NA),  40,   nullptr, 0,  1, gNone,  Quantity::None},   // MpgInst — not computed here
  {"AVG MPG", "mpg",          1,  T(NA,NA,NA,NA),  40,   nullptr, 0,  2, gNone,  Quantity::None},   // MpgAvg
  {"GAL/100", "",             1,  T(NA,NA,NA,NA),  20,   nullptr, 0,  2, gNone,  Quantity::None},   // Gal100mi
  {"L/100km", "",             1,  T(NA,NA,NA,NA),  30,   nullptr, 0,  2, gNone,  Quantity::None},   // L100km
  {"RAIL",    "psi",          0,  T(NA,NA,NA,NA),  30000,nullptr, 0,  1, gNone,  Quantity::PressHi},// Rail — not standard PID
  {"HP",      "hp",           0,  T(NA,NA,NA,NA),  400,  nullptr, 0,  0, gNone,  Quantity::None},   // Hp — not computed here
  {"DEF",     "%",            0,  T(NA,NA,NA,NA),  100,  nullptr, 0,  2, gNone,  Quantity::None},   // Def — diesel, not standard PID
  {"FUEL%",   "%",            0,  T(NA,NA,NA,NA),  100,  "012F",  0,  1, gLoad,  Quantity::None},   // FuelLevel (012F = A*100/255) — scheduled via GEN_HELPERS (pages full)
  {"DSL FILL","gal",          1,  T(NA,NA,NA,NA),  40,   nullptr, 0,  2, gNone,  Quantity::None},   // DslFill — no known tank capacity
  {"DEF FILL","gal",          1,  T(NA,NA,NA,NA),  8,    nullptr, 0,  2, gNone,  Quantity::None},   // DefFill — no DEF tank
  {"ACT TQ",  "%",            0,  T(NA,NA,NA,NA),  100,  nullptr, 0,  1, gNone,  Quantity::None},   // ActTq — not standard PID
  {"REF TQ",  "",             0,  T(NA,NA,NA,NA),  1000, nullptr, 0,  2, gNone,  Quantity::None},   // RefTq — not standard PID
  {"BARO",    "kPa",          0,  T(NA,NA,NA,NA),  110,  nullptr, 0,  2, gNone,  Quantity::Press},  // Baro — not wired (no decoder wrapper here)
  {"MAF",     "g/s",          0,  T(NA,NA,NA,NA),  400,  "0110",  0,  1, gMaf,   Quantity::None},   // Maf (0110) — None: g/s is not the gph->L/h Flow conversion
  {"AMBIENT", "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  150,  nullptr, 0,  2, gNone,  Quantity::Temp},   // Ambient — not standard PID here
  {"EGR",     "%",            0,  T(NA,NA,NA,NA),  100,  nullptr, 0,  2, gNone,  Quantity::None},   // Egr — not standard PID
  {"PEDAL",   "%",            0,  T(NA,NA,NA,NA),  100,  "0111",  0,  1, gLoad,  Quantity::None},   // Pedal (0111 throttle position)
  {"CAC",     "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  nullptr, 0,  1, gNone,  Quantity::Temp},   // Cac — not standard PID
  {"NOx",     "ppm",          0,  T(NA,NA,NA,NA),  400,  nullptr, 0,  2, gNone,  Quantity::None},   // Nox — not standard PID
  {"OIL P",   "psi",          0,  T(NA,NA,NA,NA),  80,   nullptr, 0,  0, gNone,  Quantity::Press},  // OilP — not standard PID
  {"GEAR",    "",             0,  T(NA,NA,NA,NA),  10,   nullptr, 0,  0, gNone,  Quantity::None},   // Gear — not standard PID
};
static_assert(sizeof(GENERIC_READOUTS)/sizeof(GENERIC_READOUTS[0]) == (size_t)STAT_COUNT,
              "GENERIC_READOUTS row count must equal StatId::COUNT");

// Two pages of the standard live stats. StatId::COUNT marks an empty cell.
static const StatId GEN_PAGES[][4] = {
  { StatId::Rpm,     StatId::Speed,   StatId::Coolant, StatId::Load },
  { StatId::Intake,  StatId::Pedal,   StatId::Maf,     StatId::Volts },
};
static const char* const GEN_PAGE_NAMES[] = { "ENGINE", "AIR" };
// FUEL% (012F) has a live cmd but the two pages above are full, so schedule it
// as a helper — this keeps values[FuelLevel] live (it isn't left at a stale 0)
// without forcing a sparse third page. A future generic layout can promote it
// to a visible tile.
static const StatId GEN_HELPERS[] = { StatId::FuelLevel };

static constexpr int LAYOUT_PAGE_COUNT   = (int)(sizeof(GEN_PAGES)   / sizeof(GEN_PAGES[0]));
static constexpr int LAYOUT_HELPER_COUNT = (int)(sizeof(GEN_HELPERS) / sizeof(GEN_HELPERS[0]));
static_assert(sizeof(GEN_PAGE_NAMES)/sizeof(GEN_PAGE_NAMES[0]) == (size_t)LAYOUT_PAGE_COUNT,
              "GEN_PAGE_NAMES must have one entry per GEN_PAGES row");

// ---- Addressing: standard functional/ECM addressing only (7DF broadcast).
// Header 0 is the ONLY header this profile uses. genEmit() always returns
// nullptr at step 0 -- unlike GM's gmEmitFunctional (which explicitly sends
// "AT SH 7DF\r"), the generic profile relies on the adapter's power-on default
// header (7DF for every ELM327 in the standard OBD-II protocols) and never
// issues an AT SH command; pidQueryStep's "no setup commands: fall through
// and send the query immediately" path (obd_query.h) handles this cleanly.
static const char* genEmit(int, bool, char*, size_t) { return nullptr; }
static const AddressingDef GEN_ADDRESSING[] = { { "7DF", genEmit } };
static constexpr int GEN_ADDRESSING_COUNT =
  (int)(sizeof(GEN_ADDRESSING) / sizeof(GEN_ADDRESSING[0]));

// --- The profile instance -------------------------------------------------
// `extern` on the definition itself is required: a namespace-scope `const` in
// C++ defaults to internal linkage unless previously declared extern IN THIS
// TU (same reasoning as gm_sierra_lz0.cpp / bmw_f10_535i.cpp) — without it the
// symbol would be invisible to main.cpp / every test that links this file.
extern const VehicleProfile GENERIC_PROFILE = {
  "Generic OBD-II",
  "Standard Mode-01",
  0.0f,                 // no known diesel tank capacity
  0.0f,                 // no known DEF tank capacity
  GENERIC_READOUTS,
  { GEN_PAGES, LAYOUT_PAGE_COUNT, GEN_PAGE_NAMES, GEN_HELPERS, LAYOUT_HELPER_COUNT },
  GEN_ADDRESSING, GEN_ADDRESSING_COUNT,
};
