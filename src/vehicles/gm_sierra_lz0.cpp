// ============================================================================
//  GM / 2025 Sierra 1500 3.0L Duramax (LZ0, Global B) vehicle profile.
//  Everything vehicle-specific lives here: the readout table (PIDs, headers,
//  decoders, thresholds), the default display layout, tank capacities, and
//  the splash identity. The rest of the firmware is vehicle-blind and reads
//  it through VEHICLE (vehicle_profile.h). Comments are the provenance record
//  (range scans + drive-log correlation) -- keep them with their rows.
// ============================================================================
#include "readouts.h"
#include "pid_decode.h"
#include "vehicle_profile.h"
#include "can29_ecm_addr.h"
#include <cstdio>

// --- decode functions: thin wrappers over host-tested pid_decode ---
// Every wrapper returns NaN on a short / sentinel / implausible frame; pollQuery
// skips NaN and keeps the last good value, so one corrupt packet can't poison a
// gauge or trip a false alarm. (0xFF / 0xFFFF are common ELM "no data" pads.)
// 0xFF (215C=419F) and 0x00 (-40C/-40F) are both dropout sentinels, not readings:
// the 2026-07-17 drive logged -40F on 10% of INTAKE (22000F) rows from 0x00 frames.
static float decTempF(const uint8_t* d, int n, const DecodeCtx&)  { return (n>0 && d[0]!=0xFF && d[0]!=0x00) ? cToF(decodeTempC(d[0])) : NAN; }
static float decVolts(const uint8_t* d, int n, const DecodeCtx&) { if (n<1) return NAN; float v=decodeModuleVolts(d[0], n>1?d[1]:0); return (v<6.0f||v>18.0f)?NAN:v; }  // reject impossible 0V/65V
static float decRpm(const uint8_t* d, int n, const DecodeCtx&)   { return n>1 ? decodeRpm(d[0], d[1]) : NAN; }
static float decSpeed(const uint8_t* d, int n, const DecodeCtx&)   { return n>0 ? kphToMph(decodeSpeedKph(d[0])) : NAN; }
static float decBaro(const uint8_t* d, int n, const DecodeCtx&)    { return (n>0 && d[0]) ? decodeMapKpa(d[0]) : NAN; }  // 0 kPa would poison boost calc
static float decBoost(const uint8_t* d, int n, const DecodeCtx& c) { return n>0 ? boostPsi(decodeMapKpa(d[0]), c.values[IDX_BARO]) : NAN; }
static float decFuelRate(const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeFuelRateGph(d[0], d[1]) : NAN; }
static float decEgt(const uint8_t* d, int n, const DecodeCtx&)      { if (n<2) return NAN; float f=decodeEgtMaxF(d, n); return (f<-60.0f||f>2500.0f)?NAN:f; }  // clamp sentinel -> no false EGT critical
static float decDpfDp(const uint8_t* d, int n, const DecodeCtx&)    { return n>2 ? decodeDpfDeltaKpa(d[1], d[2]) : NAN; }
static float decLoad(const uint8_t* d, int n, const DecodeCtx&)      { return n>0 ? decodeLoadPct(d[0]) : NAN; }
static float decRail(const uint8_t* d, int n, const DecodeCtx&)   { return n>1 ? decodeRailPsi(d[0], d[1]) : NAN; }
// 9B byte[3]=tank level (byte[1] is concentration). NaN on a short/zero frame so
// pollQuery keeps the last good value instead of flashing 0% (which would trip the
// low-DEF alarm during the long inter-poll gap).
static float decDef(const uint8_t* d, int n, const DecodeCtx&)    { return (n>3 && d[3]) ? decodeDefLevelPct(d[3]) : NAN; }
static float decTorque(const uint8_t* d, int n, const DecodeCtx&)   { return n>0 ? decodeTorquePct(d[0]) : NAN; }
static float decRefTq(const uint8_t* d, int n, const DecodeCtx&)  { return n>1 ? decodeRefTorqueNm(d[0], d[1]) : NAN; }
static float decFuelLevel(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeFuelLevelPct(d[0]) : NAN; }
static float decMaf(const uint8_t* d, int n, const DecodeCtx&)   { return n>1 ? decodeMafGps(d[0], d[1]) : NAN; }
static float decCac(const uint8_t* d, int n, const DecodeCtx&)   { return (n>1 && d[1]!=0xFF) ? cToF(decodeTempC(d[1])) : NAN; }
// 0xFFFF = NOx sensor "not ready" (cold/idle/no-load) -> NaN so the tile shows
// "--" instead of 65535; pollQuery skips NaN (keeps last good ppm once warm).
static float decNox(const uint8_t* d, int n, const DecodeCtx&)   { return (n>2 && !(d[1]==0xFF && d[2]==0xFF)) ? decodeNoxPpm(d[1], d[2]) : NAN; }
// Computed rows are filled by Economy/HP (BleObdSource); their decode is never called.
static float decNone(const uint8_t*, int, const DecodeCtx&)        { return 0.0f; }
// Oil pressure 22115C: psi = (raw - 36) * 0.6, clamped >= 0. The offset is
// DATA-PROVEN: engine-off rows (auto stop-start, fuel = 0.00 gph) read raw 36
// — that is the sensor's zero-pressure floor, not 18 psi (the original raw/2
// guess displayed 18 psi with the engine OFF, and its raw-30 alarm point sat
// below the raw-35 physical floor, so a real pressure loss could NEVER alarm).
// The 0.6 slope is the remaining hypothesis (hot idle ~20 psi, 47 @2500 rpm,
// 50 @redline — all textbook); no on-vehicle psi readout exists to calibrate
// against, so correct the slope HERE only if a reference gauge ever appears.
static float decOilPsi(const uint8_t* d, int n, const DecodeCtx&)   { if (n<1 || d[0]==0xFF) return NAN; float p=(d[0]-36)*0.6f; return p<0 ? 0.0f : p; }
static float decRaw1(const uint8_t* d, int n, const DecodeCtx&)     { return n>0 ? (float)d[0] : NAN; }

#define T(wh,ch,wl,cl) Thresholds{wh,ch,wl,cl}
static const float NA = NAN;

// Per-stat DEFINITIONS only (display layout lives in layout.h). Row order MUST
// match StatId enum order (Trans=0 ... Baro=24, ... Nox=30).
static const ReadoutDef GM_READOUTS[] = {
  // name      unit            dec  thr                      full    cmd        hdr tier  decode            quantity
  // TRANS 240/260: 10L80 tows at 200-230F normally (old 235 warn ambered on hot
  // tows); 240 = Banks ATF-oxidation onset, 260 = seal damage + 10F lead on GM's
  // 270F "Trans Hot" DIC. (2026-07 alarm review)
  {"TRANS",   "\xC2\xB0""F",   0,   T(240,260,NA,NA),         300,  "221940",    1,   1,  decTempF,         Quantity::Temp},
  // OIL 250/265: towing normal 235-240F; the ECM's own thermal management
  // intervenes ~257F, so sustained past that = cooling losing — crit before the
  // old 275. (2026-07 alarm review)
  {"OIL",     "\xC2\xB0""F",   0,   T(250,265,NA,NA),         300,  "015C",      0,   1,  decTempF,         Quantity::Temp},
  {"BOOST",   "psi",           1,   T(NA,NA,NA,NA),            22,  "22000B",    2,   0,  decBoost,         Quantity::Press},
  // COOLANT 235/245: LZ0 has NO thermostat (Active Thermal Mgmt targets ~200F)
  // and 200-230F towing is normal BY DESIGN — the old 225 warn ambered routinely.
  // GM's P0217 overtemp sets 240-260F. (2026-07 alarm review)
  {"COOLANT", "\xC2\xB0""F",   0,   T(235,245,NA,NA),         300,  "0105",      0,   1,  decTempF,         Quantity::Temp},
  // VOLTS low 11.0/10.2: GM RVC sat at 11.4V for 11s after a key-start on the
  // 2026-07-17 drive (blew past holdoff + startup grace at the old 11.5 warn).
  {"VOLTS",   "V",             1,   T(15.8f,16.2f,11.0f,10.2f),18,  "0142",      0,   1,  decVolts,         Quantity::None},
  {"INTAKE",  "\xC2\xB0""F",   0,   T(NA,NA,NA,NA),           300,  "22000F",    2,   1,  decTempF,         Quantity::Temp},
  {"RPM",     "",              0,   T(NA,NA,NA,NA),           4000,  "010C",      0,   0,  decRpm,           Quantity::None},
  {"SPEED",   "mph",           0,   T(NA,NA,NA,NA),            90,  "010D",      0,   0,  decSpeed,         Quantity::Speed},
  // EGT 1300/1450: this tile is the MAX of 3 aftertreatment sensors, and DPF
  // regen intentionally runs the DPF inlet at 1100-1150F (old 1250 warn had
  // only 100F of regen headroom). GM sustained-tow rating 1300-1350F, turbo
  // burst limit 1450F (<=5 min), factory limp 1475F. (2026-07 alarm review)
  {"EGT",     "\xC2\xB0""F",   0,   T(1300,1450,NA,NA),     1500,  "220078",    2,   1,  decEgt,           Quantity::Temp},
  {"DPF dP",  "kPa",           1,   T(50,60,NA,NA),           70,  "22007A",    2,   1,  decDpfDp,         Quantity::None},
  {"FUEL",    "gph",           1,   T(NA,NA,NA,NA),           15,  "22005E",    2,   1,  decFuelRate,      Quantity::Flow},
  {"LOAD",    "%",             0,   T(NA,NA,NA,NA),          100,  "0104",      0,   1,  decLoad,          Quantity::None},
  {"MPG",     "",              1,   T(NA,NA,NA,NA),           40,  nullptr,     0,   1,  decNone,          Quantity::None, RF_COMPUTED},
  {"MPG AVG", "",              1,   T(NA,NA,NA,NA),           40,  nullptr,     0,   1,  decNone,          Quantity::None, RF_COMPUTED},
  {"GAL/100", "",              1,   T(NA,NA,NA,NA),           10,  nullptr,     0,   1,  decNone,          Quantity::None, RF_COMPUTED},
  {"L/100km", "",              1,   T(NA,NA,NA,NA),           15,  nullptr,     0,   1,  decNone,          Quantity::None, RF_COMPUTED},
  {"RAIL",    "psi",           0,   T(NA,NA,NA,NA),        36000,  "220023",    2,   0,  decRail,          Quantity::PressHi},
  {"HP",      "hp",            0,   T(NA,NA,NA,NA),          300,  nullptr,     0,   1,  decNone,          Quantity::None, RF_COMPUTED},
  {"DEF",     "%",             0,   T(NA,NA,10.0f,5.0f),     100,  "22009B",    2,   1,  decDef,           Quantity::None},
  {"FUEL%",   "%",             0,   T(NA,NA,NA,NA),          100,  "22002F",    2,   1,  decFuelLevel,     Quantity::None},
  {"DSL+",    "gal",           1,   T(NA,NA,NA,NA),           24,  nullptr,     0,   1,  decNone,          Quantity::Vol,  RF_COMPUTED},
  {"DEF+",    "gal",           1,   T(NA,NA,NA,NA),            6,  nullptr,     0,   1,  decNone,          Quantity::Vol,  RF_COMPUTED},
  {"TORQUE",  "%",             0,   T(NA,NA,NA,NA),          100,  "220062",    2,   0,  decTorque,        Quantity::None},
  {"RefTq",   "",              0,   T(NA,NA,NA,NA),         1000,  "220063",    2,   2,  decRefTq,         Quantity::None},
  {"BARO",    "kPa",           0,   T(NA,NA,NA,NA),           110,  "0133",      0,   2,  decBaro,          Quantity::None},
  {"MAF",     "g/s",           0,   T(NA,NA,NA,NA),          300,  "220010",    2,   0,  decMaf,           Quantity::None},
  {"AMBIENT", "\xC2\xB0""F",   0,   T(NA,NA,NA,NA),          150,  "220046",    2,   1,  decTempF,         Quantity::Temp},
  {"EGR",     "%",             0,   T(NA,NA,NA,NA),          100,  "22002C",    2,   1,  decLoad,          Quantity::None},
  {"PEDAL",   "%",             0,   T(NA,NA,NA,NA),          100,  "22004A",    2,   0,  decLoad,          Quantity::None},
  {"CAC",     "\xC2\xB0""F",   0,   T(NA,NA,NA,NA),          400,  "220077",    2,   1,  decCac,           Quantity::Temp},
  {"NOx",     "ppm",           0,   T(NA,NA,NA,NA),         2000,  "220083",    2,   1,  decNox,           Quantity::None},
  // OIL P low 12/8: true hot idle is 19-22 psi (raw 68-72; the earlier "18 psi
  // idle" was engine-off contamination) — warn below 12 / crit below 8 sits
  // clear of hot idle, and a genuine loss now reads 0 psi and fires. The low
  // alarm is ARM-DELAYED (zoneForStat + lowArmTick): only live after 20s of
  // sustained running, so key-on / auto-stop / restarts never nuisance-fire.
  {"OIL P",   "psi",           0,   T(NA,NA,12,8),            80,  "22115C",    2,   0,  decOilPsi,        Quantity::Press, RF_LOW_NEEDS_ENGINE},
  // Current gear (confirmed vs 10L80 ratio ladder). Hidden HELPER: CSV-logged
  // for towing shift analysis, not displayed (the DIC already shows gear).
  {"GEAR",    "",              0,   T(NA,NA,NA,NA),           10,  "22199A",    1,   0,  decRaw1,          Quantity::None},
};
// A row added without a matching StatId (or vice-versa) would desync every
// StatId-indexed array (order[]/hot[]/cur_/values_) -> OOB. Catch it at compile time.
static_assert(sizeof(GM_READOUTS)/sizeof(GM_READOUTS[0]) == (size_t)STAT_COUNT,
              "READOUTS row count must equal StatId::COUNT");


// ============================================================================
//  DISPLAY LAYOUT — edit this file to rearrange the screen.
//  Each PAGES row is one page (exactly 4 cells). Use `_` for an empty cell.
//  Reorder/add/remove rows for any number of pages.
//  To DEACTIVATE a stat: remove it from BOTH lists below -> it is neither shown
//  nor polled. HELPERS are queried but never displayed (needed for computed/
//  derived values, e.g. baro for boost, ActTq/RefTq for HP).
// ============================================================================
// Pages are grouped by SITUATION (what you need in one glance), not subsystem:
// p0 TOWING is the page that lives on-screen while towing — every alarmed temp
// at once. Pages wrap, so p6 (SPEED/VOLTAGE) is one detent CCW from p0.
// Dropped from display (2026-07-05): BARO + TORQUE (still polled as HELPERS —
// boost needs baro, HP needs ActTq/RefTq); PEDAL + AMBIENT (deactivated, not
// polled: pedal is the driver's own foot, ambient is on the dash DIC).
#define _ StatId::COUNT
static const StatId PAGES[][4] = {
  { StatId::Trans,   StatId::Coolant, StatId::OilP,     StatId::Egt     }, // TOWING: the four alarmed tow gauges (oil PRESSURE; oil temp lives on MISCELLANEOUS)
  { StatId::Boost,   StatId::Hp,      StatId::Rpm,      StatId::Load    }, // POWER (gear tile: swap LOAD when 22199A confirmed)
  { StatId::DpfDp,   StatId::FuelRate, StatId::Nox,     StatId::Rail    }, // REGENERATION: dP + gph up at cruise = regen
  { StatId::FuelLevel, StatId::DslFill, StatId::Def,    StatId::DefFill }, // RANGE
  { StatId::MpgInst, StatId::MpgAvg,  StatId::Gal100mi, StatId::L100km  }, // TRIP / efficiency
  { StatId::Maf,     StatId::Egr,     StatId::Cac,      StatId::Intake  }, // DIAGNOSTICS
  { StatId::Speed,   StatId::Volts,   StatId::Oil,      _               }, // MISCELLANEOUS (adjacent to TOWING via wrap)
};
#undef _
// Gear is logged-only (towing shift analysis; the DIC already shows gear).
static const StatId HELPERS[] = { StatId::RefTq, StatId::Baro, StatId::ActTq, StatId::Gear };

static constexpr int LAYOUT_PAGE_COUNT   = (int)(sizeof(PAGES)   / sizeof(PAGES[0]));
static constexpr int LAYOUT_HELPER_COUNT = (int)(sizeof(HELPERS) / sizeof(HELPERS[0]));

// One name per PAGES row (shown in the quad view's header band, as "NAME  n/m").
// Budget is 298 px at montserrat_20 including the " n/m" suffix — the longest
// here, MISCELLANEOUS, measures 213 px. See statLabel() in readouts.cpp.
static const char* const PAGE_NAMES[] = { "TOWING", "POWER", "REGENERATION", "RANGE",
                                          "TRIP", "DIAGNOSTICS", "MISCELLANEOUS" };
static_assert(sizeof(PAGE_NAMES)/sizeof(PAGE_NAMES[0]) == (size_t)LAYOUT_PAGE_COUNT,
              "PAGE_NAMES must have one entry per PAGES row");

// ---- Addressing: GM Global B. Header index matches the READOUTS[].header field
// (0=functional, 1=trans, 2=ECM). The 29-bit forms (WiFi adapter) mirror the
// legacy atShFor(); the ECM 29-bit target comes from can29EcmAddr() (default
// 0x10, runtime-mutable). Each GM header is a single AT SH command (step 1 ends
// the sequence).
static const char* gmEmitFunctional(int step, bool can29, char*, size_t) {
  if (step != 0) return nullptr;
  return can29 ? "AT SH DB33F1\r" : "AT SH 7DF\r";
}
static const char* gmEmitTrans(int step, bool can29, char*, size_t) {
  if (step != 0) return nullptr;
  return can29 ? "AT SH DA18F1\r" : "AT SH 7E2\r";
}
static const char* gmEmitEcm(int step, bool can29, char* buf, size_t n) {
  if (step != 0) return nullptr;
  if (!can29) return "AT SH 7E0\r";
  snprintf(buf, n, "AT SH DA%02XF1\r", (unsigned)can29EcmAddr());   // runtime-mutable ECM addr
  return buf;
}
static const AddressingDef GM_ADDRESSING[] = {
  { "7DF/func",  gmEmitFunctional },   // header 0
  { "7E2/trans", gmEmitTrans },        // header 1
  { "7E0/ecm",   gmEmitEcm },          // header 2
};
static constexpr int GM_ADDRESSING_COUNT =
  (int)(sizeof(GM_ADDRESSING) / sizeof(GM_ADDRESSING[0]));

// --- The profile instance -------------------------------------------------
// Tank capacities: 2025 Sierra 1500 3.0L Duramax (AT4/4WD).
// `extern` on the definition itself is required: a namespace-scope `const` in
// C++ defaults to internal linkage unless previously declared extern IN THIS
// TU. vehicle_profile.h no longer carries that forward decl (Task 1 dropped
// it in favor of the vehicle_active.h macros), so without this keyword the
// symbol would be invisible to main.cpp / every test that links this file.
extern const VehicleProfile GM_SIERRA_LZ0_PROFILE = {
  "GMC Sierra 1500",
  "Duramax 3.0L Diesel",
  24.0f,                  // diesel tank, gal
  5.4f,                   // DEF tank, gal
  GM_READOUTS,
  { PAGES, LAYOUT_PAGE_COUNT, PAGE_NAMES, HELPERS, LAYOUT_HELPER_COUNT },
  GM_ADDRESSING, GM_ADDRESSING_COUNT,
};
