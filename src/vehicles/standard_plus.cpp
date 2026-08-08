// ============================================================================
//  STANDARD+ -- a richer profile for vehicles we can NAME but have never
//  SCANNED (the Ford, Ram and Chevrolet/GMC pickups identified by VIN).
//
//  ⚠️  READ THIS BEFORE ADDING A ROW.
//  Every PID here is legislated SAE J1979 Mode-01, with scaling defined by the
//  standard -- NOT a manufacturer-specific DID and NOT a guess. That is the
//  whole basis on which this profile is allowed to exist without an on-car
//  scan: the other profiles in this directory were each built from a real
//  vehicle (see the header of jeep_ws.cpp), and inventing enhanced DIDs for a
//  truck nobody has plugged into would put WRONG NUMBERS on real gauges.
//
//  The failure mode here is safe by construction: a vehicle that does not
//  support one of these PIDs answers NO DATA and the tile stays blank. Blank is
//  honest; a wrong number is not. So the cost of listing a PID the truck lacks
//  is an empty tile, never a misleading reading.
//
//  ALARMS ARE OFF on every tile except COOLANT and VOLTS, inherited from the
//  Generic profile -- no thresholds have been sourced for any of these trucks,
//  the same "no verified thresholds -> alarms off" doctrine the BMW, Audi and
//  Jeep skeletons follow.
//
//  TWO PROFILES, ONE READOUT TABLE. The PID set is identical; only the PAGES
//  differ. A gas truck simply never answers EGT/NOx, so those tiles are left
//  off its pages rather than shown permanently blank.
//
//  Selected by the engine code at vin[7], which src/vin.cpp already decodes --
//  see docs/VEHICLES.md. An unrecognised engine falls back to the gas layout,
//  because diesel-only tiles on a gas truck look broken while the reverse
//  merely omits data the truck does not have.
// ============================================================================
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

// --- decoders. Standard Mode-01 scaling, shared with the other profiles via
// pid_decode.h; each profile TU keeps its own thin wrappers (same convention as
// generic_obd.cpp / jeep_ws.cpp).
static float gEgt(const uint8_t* d, int n, const DecodeCtx&)      { if (n<3) return NAN; float f=decodeEgtMaxF(d,n); return (f<-60.0f||f>2500.0f)?NAN:f; }
static float gFuelRate(const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeFuelRateGph(d[0],d[1]) : NAN; }
static float gRail(const uint8_t* d, int n, const DecodeCtx&)     { return n>1 ? decodeRailPsi(d[0],d[1]) : NAN; }
static float gActTq(const uint8_t* d, int n, const DecodeCtx&)    { return n>0 ? decodeTorquePct(d[0]) : NAN; }
static float gRefTq(const uint8_t* d, int n, const DecodeCtx&)    { return n>1 ? decodeRefTorqueNm(d[0],d[1]) : NAN; }
static float gBaro(const uint8_t* d, int n, const DecodeCtx&)     { return n>0 ? decodeMapKpa(d[0]) : NAN; }
static float gNox(const uint8_t* d, int n, const DecodeCtx&)      { return n>2 ? decodeNoxPpm(d[1],d[2]) : NAN; }

static const ReadoutDef STDPLUS_READOUTS[] = {
  // name      unit           dec thr              full   cmd     hdr tier decode  quantity
  {"TRANS",   "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  nullptr, 0,  1, gNone,  Quantity::Temp},   // Trans — not standard PID
  {"OIL",     "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  "015C", 0,  1, gTempF,  Quantity::Temp},   // Oil temperature (015C) — SAE J1979 standard
  {"BOOST",   "psi",          1,  T(NA,NA,NA,NA),  30,   nullptr, 0,  0, gNone,  Quantity::Press},  // Boost — not standard PID
  // Coolant (0105) — the sole intentional alarm on Generic. Warn-only, no low
  // thresholds. Worth having here because 0105 is a standard, well-defined SAE PID
  // (not an unverified manufacturer DID), and losing coolant is the one failure
  // where an early warning on an unknown engine is clearly worth a false positive.
  //
  // 242/255 F, RAISED from 235/250 on 2026-07-26. The old values came with the
  // claim that "sustained >235 F is trouble on essentially any engine". That is
  // wrong, and the counter-example is a vehicle this project already supports:
  // BMW's N55 DME deliberately TARGETS 226 F (108 C) in Economy mode — normal
  // operation, not a fault. A 235 F warn leaves nine degrees of headroom above a
  // factory setpoint, so any hot-running engine that lands on Generic (an
  // unrecognised WMI, or a VIN read that failed) would raise an amber warning
  // while running exactly as designed.
  //
  // Nuisance alarms are worse than no alarm: they teach the driver the display
  // cries wolf, and then the real one gets ignored too. 242 F clears the hottest
  // factory target this project has documented with real margin, and 255 F is
  // still far below anything that reads as healthy on any engine.
  //
  // Confidence: this is anchored on ONE well-documented hot-running engine (N55,
  // see docs/BMW-STATUS.md). If a vehicle turns up that idles hotter than 242 F by
  // design, raise it again rather than assuming the number is universal — the
  // mistake being corrected here was exactly that assumption.
  {"COOLANT", "\xC2\xB0""F",  0,  T(242,255,NA,NA),300,  "0105",  0,  1, gTempF, Quantity::Temp},
  {"VOLTS",   "V",            1,  T(NA,NA,11.0f,10.2f),18,"0142", 0,  1, gVolts, Quantity::None},   // Volts (0142)
  {"INTAKE",  "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  "010F",  0,  1, gTempF, Quantity::Temp},   // Intake air temp (010F)
  {"RPM",     "",             0,  T(NA,NA,NA,NA),  7000, "010C",  0,  0, gRpm,   Quantity::None},   // Rpm (010C)
  {"SPEED",   "mph",          0,  T(NA,NA,NA,NA),  120,  "010D",  0,  0, gSpeed, Quantity::Speed},  // Speed (010D)
  {"EGT",     "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  1600, "0178", 0,  1, gEgt,  Quantity::Temp},   // Exhaust gas temp (0178) — SAE J1979 standard
  {"DPF dP",  "psi",          1,  T(NA,NA,NA,NA),  15,   nullptr, 0,  2, gNone,  Quantity::Press},  // DpfDp — diesel, not standard PID
  {"FUEL RATE","gph",         1,  T(NA,NA,NA,NA),  20,   "015E", 0,  1, gFuelRate,  Quantity::Flow},   // Engine fuel rate (015E) - SAE J1979 standard
  {"LOAD",    "%",            0,  T(NA,NA,NA,NA),  100,  "0104",  0,  0, gLoad,  Quantity::None},   // Load (0104 calculated engine load)
  {"MPG",     "mpg",          1,  T(NA,NA,NA,NA),  40,   nullptr, 0,  1, gNone,  Quantity::None},   // MpgInst — not computed here
  {"AVG MPG", "mpg",          1,  T(NA,NA,NA,NA),  40,   nullptr, 0,  2, gNone,  Quantity::None},   // MpgAvg
  {"GAL/100", "",             1,  T(NA,NA,NA,NA),  20,   nullptr, 0,  2, gNone,  Quantity::None},   // Gal100mi
  {"L/100km", "",             1,  T(NA,NA,NA,NA),  30,   nullptr, 0,  2, gNone,  Quantity::None},   // L100km
  {"RAIL",    "psi",          0,  T(NA,NA,NA,NA),  30000,"0123", 0,  1, gRail,  Quantity::PressHi},// Fuel rail gauge pressure (0123) — SAE J1979 standard
  {"HP",      "hp",           0,  T(NA,NA,NA,NA),  400,  nullptr, 0,  0, gNone,  Quantity::None},   // Hp — not computed here
  {"DEF",     "%",            0,  T(NA,NA,NA,NA),  100,  nullptr, 0,  2, gNone,  Quantity::None},   // Def — diesel, not standard PID
  {"FUEL%",   "%",            0,  T(NA,NA,NA,NA),  100,  "012F",  0,  1, gLoad,  Quantity::None},   // FuelLevel (012F = A*100/255) — scheduled via GEN_HELPERS (pages full)
  {"DSL FILL","gal",          1,  T(NA,NA,NA,NA),  40,   nullptr, 0,  2, gNone,  Quantity::None},   // DslFill — no known tank capacity
  {"DEF FILL","gal",          1,  T(NA,NA,NA,NA),  8,    nullptr, 0,  2, gNone,  Quantity::None},   // DefFill — no DEF tank
  {"ACT TQ",  "%",            0,  T(NA,NA,NA,NA),  100,  "0162", 0,  1, gActTq,  Quantity::None},   // Actual engine torque (0162) — SAE J1979 standard
  {"REF TQ",  "",             0,  T(NA,NA,NA,NA),  1000, "0163", 0,  2, gRefTq,  Quantity::None},   // Reference torque (0163) — SAE J1979 standard
  {"BARO",    "kPa",          0,  T(NA,NA,NA,NA),  110,  "0133", 0,  2, gBaro,  Quantity::Press},  // Barometric pressure (0133) — SAE J1979 standard
  {"MAF",     "g/s",          0,  T(NA,NA,NA,NA),  400,  "0110",  0,  1, gMaf,   Quantity::None},   // Maf (0110) — None: g/s is not the gph->L/h Flow conversion
  {"AMBIENT", "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  150,  "0146", 0,  2, gTempF,  Quantity::Temp},   // Ambient air temp (0146) — SAE J1979 standard
  {"EGR",     "%",            0,  T(NA,NA,NA,NA),  100,  "012C", 0,  2, gLoad,  Quantity::None},   // Commanded EGR (012C) — SAE J1979 standard
  {"PEDAL",   "%",            0,  T(NA,NA,NA,NA),  100,  "0111",  0,  1, gLoad,  Quantity::None},   // Pedal (0111 throttle position)
  {"CAC",     "\xC2\xB0""F",  0,  T(NA,NA,NA,NA),  300,  nullptr, 0,  1, gNone,  Quantity::Temp},   // Cac — not standard PID
  {"NOx",     "ppm",          0,  T(NA,NA,NA,NA),  400,  "0183", 0,  2, gNox,  Quantity::None},   // NOx sensor (0183) — SAE J1979 standard
  {"OIL P",   "psi",          0,  T(NA,NA,NA,NA),  80,   nullptr, 0,  0, gNone,  Quantity::Press},  // OilP — not standard PID
  {"GEAR",    "",             0,  T(NA,NA,NA,NA),  10,   nullptr, 0,  0, gNone,  Quantity::None},   // Gear — not standard PID
};
static_assert(sizeof(STDPLUS_READOUTS)/sizeof(STDPLUS_READOUTS[0]) == (size_t)STAT_COUNT,
              "STDPLUS_READOUTS row count must equal StatId::COUNT");

// Diesel: four pages. EGT and NOx are diesel-only -- a gas engine never answers
// them, so they do not appear on the gas layout below.
static const StatId STD_DIESEL_PAGES[][4] = {
  { StatId::Rpm,     StatId::Speed,    StatId::Coolant,  StatId::Load },
  { StatId::Oil,     StatId::Egt,      StatId::Intake,   StatId::Ambient },
  { StatId::ActTq,   StatId::RefTq,    StatId::FuelRate, StatId::Rail },
  { StatId::Baro,    StatId::Egr,      StatId::Nox,      StatId::Volts },
};
static const char* const STD_DIESEL_PAGE_NAMES[] = { "ENGINE", "THERMAL", "POWER", "AIR" };

// Gas: three pages, same PID table, EGT/NOx/Rail omitted.
static const StatId STD_GAS_PAGES[][4] = {
  { StatId::Rpm,     StatId::Speed,    StatId::Coolant,  StatId::Load },
  { StatId::Oil,     StatId::Intake,   StatId::Ambient,  StatId::Baro },
  { StatId::ActTq,   StatId::FuelRate, StatId::Maf,      StatId::Volts },
};
static const char* const STD_GAS_PAGE_NAMES[] = { "ENGINE", "THERMAL", "POWER" };

// Pedal and fuel level have live commands but no free tile, so they are
// scheduled as helpers (same reason FUEL% is a helper on Generic).
static const StatId STD_HELPERS[] = { StatId::FuelLevel, StatId::Pedal };

static constexpr int DIESEL_PAGE_COUNT = (int)(sizeof(STD_DIESEL_PAGES)/sizeof(STD_DIESEL_PAGES[0]));
static constexpr int GAS_PAGE_COUNT    = (int)(sizeof(STD_GAS_PAGES)   /sizeof(STD_GAS_PAGES[0]));
static constexpr int STD_HELPER_COUNT  = (int)(sizeof(STD_HELPERS)     /sizeof(STD_HELPERS[0]));
static_assert(sizeof(STD_DIESEL_PAGE_NAMES)/sizeof(STD_DIESEL_PAGE_NAMES[0]) == (size_t)DIESEL_PAGE_COUNT,
              "STD_DIESEL_PAGE_NAMES must have one entry per STD_DIESEL_PAGES row");
static_assert(sizeof(STD_GAS_PAGE_NAMES)/sizeof(STD_GAS_PAGE_NAMES[0]) == (size_t)GAS_PAGE_COUNT,
              "STD_GAS_PAGE_NAMES must have one entry per STD_GAS_PAGES row");

// Addressing: functional 7DF broadcast, exactly as Generic -- these are
// legislated PIDs on the legislated address, so no AT SH is issued and the
// adapter's power-on default header is used.
static const char* stdEmit(int, bool, char*, size_t) { return nullptr; }
static const AddressingDef STD_ADDRESSING[] = { { "7DF", stdEmit } };
static constexpr int STD_ADDRESSING_COUNT = (int)(sizeof(STD_ADDRESSING)/sizeof(STD_ADDRESSING[0]));

// `extern` on the definition is required for external linkage (see the note in
// generic_obd.cpp).
extern const VehicleProfile STANDARD_PLUS_DIESEL_PROFILE = {
  "Standard+ Diesel",
  "SAE J1979 Mode-01",
  0.0f, 0.0f,           // no known tank capacities -- unscanned vehicle
  STDPLUS_READOUTS,
  { STD_DIESEL_PAGES, DIESEL_PAGE_COUNT, STD_DIESEL_PAGE_NAMES, STD_HELPERS, STD_HELPER_COUNT },
  STD_ADDRESSING, STD_ADDRESSING_COUNT,
};

extern const VehicleProfile STANDARD_PLUS_GAS_PROFILE = {
  "Standard+ Gas",
  "SAE J1979 Mode-01",
  0.0f, 0.0f,
  STDPLUS_READOUTS,
  { STD_GAS_PAGES, GAS_PAGE_COUNT, STD_GAS_PAGE_NAMES, STD_HELPERS, STD_HELPER_COUNT },
  STD_ADDRESSING, STD_ADDRESSING_COUNT,
};
