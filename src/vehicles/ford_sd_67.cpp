// ============================================================================
//  FORD / 2021 F-350 Super Duty 6.7L Power Stroke (10R140) vehicle profile.
//
//  PROVENANCE: built from a real scan on 2026-08-09 -- census + 187-unique-PID
//  sweep + a 29-minute drive log, all four artefacts retained. Every row below
//  is either a legislated SAE J1979 PID the census MEASURED this truck to
//  support, or an enhanced DID confirmed on the drive log. Nothing here is a
//  community claim: a 2026-07 research pass put 25 Ford claims through
//  three-vote adversarial verification and REFUTED 19, and the most-cited
//  source (a 2014 forum post about a 2013 truck) turned out to be republishing
//  the standard J1979 F4xx block as though it were proprietary. See
//  docs/FORD-STATUS.md.
//
//  ADDRESSING IS 11-BIT, and this matters: the census found ALL FIFTEEN 29-bit
//  headers silent while 7DF/7E0/7E1 answered -- the exact inverse of the
//  Sierra. 7DF is the functional broadcast and returns the union of 7E0+7E1,
//  so it is deliberately NOT used here; polling the physical ECUs keeps one
//  request to one responder instead of relying on which module replies first.
//
//  WHY SO MANY ROWS ARE INACTIVE: a legislated PID is listed ONLY where the
//  census's supported-PID bitmap says this truck answers it. 010F (intake),
//  0111 (pedal), 0123 (rail), 012C (EGR) and 015E (fuel rate) are NOT in the
//  ECM's bitmap and so are left dark rather than polled hopefully -- except
//  010F, which the TCM does support and is therefore read at 7E1.
//
//  ALARMS ARE OFF except COOLANT and VOLTS, inherited from Generic/Standard+.
//  No 10R140 transmission thresholds have been sourced. The Sierra's 240/260
//  came from a specific review of the 10L80 and must NOT be borrowed here --
//  a different transmission, and inventing a number to fill the column is how
//  a gauge ends up lying about a truck that is perfectly fine.
//
//  NOT YET VALIDATED ON HARDWARE. No dash has been plugged into this truck.
//  The decodes below are confirmed against captured bytes; the polling path is
//  exercised only against the HIL replay of that same capture.
// ============================================================================
#include "readouts.h"
#include "pid_decode.h"
#include "vehicle_profile.h"
#include <cstdio>

// --- decoders: thin wrappers over host-tested pid_decode, same convention as
// gm_sierra_lz0.cpp / audi_q5.cpp. Every wrapper returns NaN on a short,
// sentinel or implausible frame so pollQuery keeps the last good value rather
// than poisoning a gauge or tripping a false alarm.
static float fTempF(const uint8_t* d, int n, const DecodeCtx&)  { return (n>0 && d[0]!=0xFF) ? cToF(decodeTempC(d[0])) : NAN; }
static float fVolts(const uint8_t* d, int n, const DecodeCtx&)  { if (n<1) return NAN; float v=decodeModuleVolts(d[0], n>1?d[1]:0); return (v<6.0f||v>18.0f)?NAN:v; }
static float fRpm(const uint8_t* d, int n, const DecodeCtx&)    { return n>1 ? decodeRpm(d[0], d[1]) : NAN; }
static float fSpeed(const uint8_t* d, int n, const DecodeCtx&)  { return n>0 ? kphToMph(decodeSpeedKph(d[0])) : NAN; }
static float fLoad(const uint8_t* d, int n, const DecodeCtx&)   { return n>0 ? decodeLoadPct(d[0]) : NAN; }
static float fMaf(const uint8_t* d, int n, const DecodeCtx&)    { return n>1 ? decodeMafGps(d[0], d[1]) : NAN; }
static float fBaro(const uint8_t* d, int n, const DecodeCtx&)   { return (n>0 && d[0]) ? decodeMapKpa(d[0]) : NAN; }
static float fEgt(const uint8_t* d, int n, const DecodeCtx&)    { if (n<3) return NAN; float f=decodeEgtMaxF(d,n); return (f<-60.0f||f>2500.0f)?NAN:f; }
static float fActTq(const uint8_t* d, int n, const DecodeCtx&)  { return n>0 ? decodeTorquePct(d[0]) : NAN; }
static float fRefTq(const uint8_t* d, int n, const DecodeCtx&)  { return n>1 ? decodeRefTorqueNm(d[0], d[1]) : NAN; }
static float fFuelLevel(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeFuelLevelPct(d[0]) : NAN; }
// 0183 NOx: 0xFFFF = sensor "not ready". Directly observed on this truck --
// the cold-start rows read 33FF FFFF... and only produced real ppm once the
// sensor reached light-off temperature, so this guard is load-bearing here,
// not defensive boilerplate.
static float fNox(const uint8_t* d, int n, const DecodeCtx&)    { return (n>2 && !(d[1]==0xFF && d[2]==0xFF)) ? decodeNoxPpm(d[1], d[2]) : NAN; }
static float fNone(const uint8_t*, int, const DecodeCtx&)       { return NAN; }

// 221E1C @ 7E1 -- TRANSMISSION FLUID TEMPERATURE. degC = int16 / 16, then degF.
//
// CONFIRMED on the 2026-08-09 drive: 26.8 -> 90.6 degC, monotonic over a
// 29-minute warm-up from a genuinely cold start (coolant opened at 30 degC
// against ~32 degC ambient). It LAGS coolant -- coolant reached 78 degC while
// this read 56.6 degC -- which is the signature of a separate thermal mass on
// a separate circuit, and is why the correlation report scored it only 0.851
// and labelled it "weak". The decoded engineering range, not r, is the
// evidence.
//
// MUST decode as SIGNED int16. The common unsigned form reads ~4096 degC on a
// sub-zero cold start, which would render as a nonsense temperature exactly
// when someone is most likely to be watching a cold truck warm up.
static float fAtfF(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;
  int16_t raw = (int16_t)((d[0] << 8) | d[1]);
  if ((uint16_t)raw == 0xFFFF) return NAN;              // dropout pad, not a reading
  float c = raw / 16.0f;
  return (c < -60.0f || c > 200.0f) ? NAN : cToF(c);    // reject impossible ATF temps
}

// 221E60 @ 7E1 -- CURRENT GEAR, raw byte, 0x01..0x0A.
//
// CONFIRMED by physics rather than correlation: the engine-speed-to-road-speed
// ratio is constant within a gear and falls monotonically across gears --
// gear 07 gave 22.1 and 22.3 on two independent samples, gear 08 gave 19.1 and
// 18.9, gear 0A gave 14.0 and 13.9. Ten distinct values on a ten-speed 10R140,
// reading 01 whenever road speed is zero. No generic OBD-II PID exposes gear.
static float fGear(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 1 || d[0] == 0xFF) return NAN;
  return (d[0] >= 1 && d[0] <= 10) ? (float)d[0] : NAN;  // out-of-range => not a gear
}

#define T(wh,ch,wl,cl) Thresholds{wh,ch,wl,cl}
static const float NA = NAN;

// Per-stat DEFINITIONS only (display layout lives below). Row order MUST match
// StatId enum order, same convention as every other profile in this directory.
// hdr: 0 = 7E0 (ECM), 1 = 7E1 (TCM).
static const ReadoutDef FORD_READOUTS[] = {
  // name       unit            dec thr                    full   cmd       hdr tier decode      quantity
  // TRANS: alarms OFF. See the header comment -- no sourced 10R140 limits.
  {"TRANS",    "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        300,  "221E1C",  1,  1,  fAtfF,      Quantity::Temp},
  {"OIL",      "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        300,  "015C",    0,  1,  fTempF,     Quantity::Temp},
  {"BOOST",    "psi",           1,  T(NA,NA,NA,NA),        30,   nullptr,   0,  0,  fNone,      Quantity::Press},  // no MAP PID in the ECM bitmap
  // COOLANT 242/255 degF -- the sole intentional alarm, inherited from
  // Standard+ with its reasoning intact (0105 is legislated and well defined;
  // losing coolant is the one failure where an early warning is worth a false
  // positive). Not re-derived for this truck.
  {"COOLANT",  "\xC2\xB0""F",   0,  T(242,255,NA,NA),      300,  "0105",    0,  1,  fTempF,     Quantity::Temp},
  {"VOLTS",    "V",             1,  T(NA,NA,11.0f,10.2f),  18,   "0142",    0,  1,  fVolts,     Quantity::None},
  // INTAKE: the ECM does NOT list 010F, but the TCM DOES -- read it at 7E1.
  {"INTAKE",   "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        300,  "010F",    1,  1,  fTempF,     Quantity::Temp},
  {"RPM",      "",              0,  T(NA,NA,NA,NA),        4000, "010C",    0,  0,  fRpm,       Quantity::None},
  {"SPEED",    "mph",           0,  T(NA,NA,NA,NA),        120,  "010D",    0,  0,  fSpeed,     Quantity::Speed},
  {"EGT",      "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        1600, "0178",    0,  1,  fEgt,       Quantity::Temp},
  {"DPF dP",   "psi",           1,  T(NA,NA,NA,NA),        15,   nullptr,   0,  2,  fNone,      Quantity::Press},  // 22116C absent -- that claim was bogus
  {"FUEL RATE","gph",           1,  T(NA,NA,NA,NA),        20,   nullptr,   0,  1,  fNone,      Quantity::Flow},   // 015E not in the ECM bitmap
  {"LOAD",     "%",             0,  T(NA,NA,NA,NA),        100,  "0104",    0,  0,  fLoad,      Quantity::None},
  {"MPG",      "mpg",           1,  T(NA,NA,NA,NA),        40,   nullptr,   0,  1,  fNone,      Quantity::None},
  {"AVG MPG",  "mpg",           1,  T(NA,NA,NA,NA),        40,   nullptr,   0,  2,  fNone,      Quantity::None},
  {"GAL/100",  "",              1,  T(NA,NA,NA,NA),        20,   nullptr,   0,  2,  fNone,      Quantity::None},
  {"L/100km",  "",              1,  T(NA,NA,NA,NA),        30,   nullptr,   0,  2,  fNone,      Quantity::None},
  {"RAIL",     "psi",           0,  T(NA,NA,NA,NA),        30000,nullptr,   0,  1,  fNone,      Quantity::PressHi},// 0123 not in the ECM bitmap
  {"HP",       "hp",            0,  T(NA,NA,NA,NA),        500,  nullptr,   0,  0,  fNone,      Quantity::None},
  {"DEF",      "%",             0,  T(NA,NA,NA,NA),        100,  nullptr,   0,  2,  fNone,      Quantity::None},   // see the DEF note below
  {"FUEL%",    "%",             0,  T(NA,NA,NA,NA),        100,  "012F",    0,  1,  fFuelLevel, Quantity::None},
  {"DSL FILL", "gal",           1,  T(NA,NA,NA,NA),        48,   nullptr,   0,  2,  fNone,      Quantity::None},   // tank capacity unknown for this truck
  {"DEF FILL", "gal",           1,  T(NA,NA,NA,NA),        8,    nullptr,   0,  2,  fNone,      Quantity::None},
  {"ACT TQ",   "%",             0,  T(NA,NA,NA,NA),        100,  "0162",    0,  1,  fActTq,     Quantity::None},
  {"REF TQ",   "",              0,  T(NA,NA,NA,NA),        1500, "0163",    0,  2,  fRefTq,     Quantity::None},
  {"BARO",     "kPa",           0,  T(NA,NA,NA,NA),        110,  "0133",    0,  2,  fBaro,      Quantity::Press},
  {"MAF",      "g/s",           0,  T(NA,NA,NA,NA),        400,  "0110",    0,  1,  fMaf,       Quantity::None},
  {"AMBIENT",  "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        150,  "0146",    0,  2,  fTempF,     Quantity::Temp},
  {"EGR",      "%",             0,  T(NA,NA,NA,NA),        100,  nullptr,   0,  2,  fNone,      Quantity::None},   // 012C not in the ECM bitmap
  {"PEDAL",    "%",             0,  T(NA,NA,NA,NA),        100,  nullptr,   0,  1,  fNone,      Quantity::None},   // 0111 not in the ECM bitmap
  {"CAC",      "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        300,  nullptr,   0,  1,  fNone,      Quantity::Temp},
  {"NOx",      "ppm",           0,  T(NA,NA,NA,NA),        400,  "0183",    0,  2,  fNox,       Quantity::None},
  {"OIL P",    "psi",           0,  T(NA,NA,NA,NA),        80,   nullptr,   0,  0,  fNone,      Quantity::Press},  // not identified in this scan
  {"GEAR",     "",              0,  T(NA,NA,NA,NA),        10,   "221E60",  1,  0,  fGear,      Quantity::None},
};
static_assert(sizeof(FORD_READOUTS)/sizeof(FORD_READOUTS[0]) == (size_t)STAT_COUNT,
              "FORD_READOUTS row count must equal StatId::COUNT");

// DEF is deliberately dark. 22F485 answered but returned a CONSTANT 10-byte
// payload across all 64 samples -- which is expected (DEF level cannot move
// measurably in 29 minutes), not a fault, and therefore NOT enough to identify
// which byte carries level. The Sierra taught this exact lesson the hard way:
// its byte[1] is concentration and reads stuck, while byte[3] is the real
// level. Identifying Ford's requires a log spanning a DEF consumption
// interval. Guessing a byte here would put a wrong percentage against a
// low-DEF alarm.

// Four pages, diesel. GEAR earns a tile: it is a confirmed enhanced parameter
// with no generic equivalent, and unlike the Sierra (whose DIC already shows
// gear) nothing else on this dash would display it.
static const StatId FORD_PAGES[][4] = {
  { StatId::Rpm,    StatId::Speed,  StatId::Coolant, StatId::Load    },  // ENGINE
  { StatId::Trans,  StatId::Oil,    StatId::Egt,     StatId::Ambient },  // THERMAL
  { StatId::ActTq,  StatId::RefTq,  StatId::Gear,    StatId::Volts   },  // POWER
  { StatId::Maf,    StatId::Baro,   StatId::Nox,     StatId::Intake  },  // AIR
};
static const char* const FORD_PAGE_NAMES[] = { "ENGINE", "THERMAL", "POWER", "AIR" };

// Fuel level has a live command but no free tile, so it is scheduled as a
// helper -- same reason FUEL% is a helper on Generic and Standard+.
static const StatId FORD_HELPERS[] = { StatId::FuelLevel };

static constexpr int FORD_PAGE_COUNT   = (int)(sizeof(FORD_PAGES)   / sizeof(FORD_PAGES[0]));
static constexpr int FORD_HELPER_COUNT = (int)(sizeof(FORD_HELPERS) / sizeof(FORD_HELPERS[0]));
static_assert(sizeof(FORD_PAGE_NAMES)/sizeof(FORD_PAGE_NAMES[0]) == (size_t)FORD_PAGE_COUNT,
              "FORD_PAGE_NAMES must have one entry per FORD_PAGES row");

// --- addressing: plain 11-bit, one AT SH per ECU. No extended addressing, no
// priority byte, no CAN RX filter -- Ford's enhanced DIDs answer on the same
// physical headers as Mode-01, so this is the Audi shape, not the BMW one.
static const char* fordEmitEcm(int step, bool, char*, size_t) {
  if (step != 0) return nullptr;
  return "AT SH 7E0\r";
}
static const char* fordEmitTcm(int step, bool, char*, size_t) {
  if (step != 0) return nullptr;
  return "AT SH 7E1\r";
}
static const AddressingDef FORD_ADDRESSING[] = {
  { "7E0/ecm", fordEmitEcm },   // header 0
  { "7E1/tcm", fordEmitTcm },   // header 1
};
static constexpr int FORD_ADDRESSING_COUNT =
  (int)(sizeof(FORD_ADDRESSING) / sizeof(FORD_ADDRESSING[0]));

// `extern` on the definition itself is required for external linkage (same
// reasoning as gm_sierra_lz0.cpp / audi_q5.cpp).
extern const VehicleProfile FORD_SD_67_PROFILE = {
  "Ford Super Duty",
  "6.7L Power Stroke",
  0.0f,                   // fuel tank: UNKNOWN for this truck (29/34/48 gal are
                          // all factory options and the VIN does not say which)
  0.0f,                   // DEF tank: unknown, and DEF level is not decoded anyway
  FORD_READOUTS,
  { FORD_PAGES, FORD_PAGE_COUNT, FORD_PAGE_NAMES, FORD_HELPERS, FORD_HELPER_COUNT },
  FORD_ADDRESSING, FORD_ADDRESSING_COUNT,
};
