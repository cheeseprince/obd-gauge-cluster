// ============================================================================
//  Audi Q5 (2018, typ FY; 2.0T TFSI EA888 gen-3), DL382 dual-clutch (S tronic).
//  SKELETON profile: three enhanced DIDs (trans/oil/boost) confirmed to ANSWER
//  on the 2026-07-24 drive but with UNVERIFIED scaling (one drive, no cold-start
//  correlation pass yet — see docs/AUDI-STATUS.md), plus legislated Mode-01.
//  Alarms OFF on every enhanced tile (T(NA,NA,NA,NA)) and on Coolant (EA888
//  thresholds not sourced yet, same "no verified thresholds -> alarms off"
//  doctrine as the BMW F10 skeleton).
//
//  Addressing: standard 11-bit UDS, NO BMW-style extended-addressing dance.
//  header 0 = engine ECM 7E0 (Mode-01 + most enhanced 22xxxx DIDs), header 1 =
//  trans/TCM 7E1 (222104 ATF temp only). Both reachable by a plain ELM327 —
//  confirmed by the OBDb Audi-Q5 command_support.yaml capture (primary source,
//  docs/AUDI-STATUS.md), not just this firmware's own drive.
// ============================================================================
#include "readouts.h"
#include "pid_decode.h"
#include "vehicle_profile.h"
#include <cstdio>

// --- standard Mode-01 decoders (self-contained; verbatim math from
// gm_sierra_lz0.cpp / bmw_f10_535i.cpp — every profile TU keeps its own copy) --
static float decTempF(const uint8_t* d, int n, const DecodeCtx&) { return (n>0 && d[0]!=0xFF && d[0]!=0x00) ? cToF(decodeTempC(d[0])) : NAN; }
static float decVolts(const uint8_t* d, int n, const DecodeCtx&) { if (n<1) return NAN; float v=decodeModuleVolts(d[0], n>1?d[1]:0); return (v<6.0f||v>18.0f)?NAN:v; }
static float decRpm  (const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeRpm(d[0], d[1]) : NAN; }
static float decSpeed(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? kphToMph(decodeSpeedKph(d[0])) : NAN; }
static float decLoad (const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeLoadPct(d[0]) : NAN; }  // also used for PEDAL (0111): same A*100/255 formula
static float decMaf  (const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeMafGps(d[0], d[1]) : NAN; }
static float decFuelLevel(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeFuelLevelPct(d[0]) : NAN; }
static float decNone (const uint8_t*, int, const DecodeCtx&)    { return 0.0f; }  // computed rows / unsupported placeholders

// --- enhanced decoders: UNVERIFIED scaling from the single 2026-07-24 drive.
// The DIDs themselves are HIGH confidence (OBDb capture proves they answer);
// only the byte->engineering-value formula below is unconfirmed. A cold-start
// correlation drive is the next step (docs/AUDI-STATUS.md "what remains
// unknown"): flag every one of these three decoders until that lands.
//
// Oil temp 2220A1@7E0: 16-bit big-endian, raw*0.1 = degC. UNVERIFIED — single
// data point 0x03A2=930 -> 93.0C, rose to ~97C over the drive; no cold-start
// point to confirm the offset/slope against.
static float decAudiOilTempF(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;
  float c = (float)(((unsigned)d[0] << 8) | d[1]) * 0.1f;
  return (c < -40.0f || c > 180.0f) ? NAN : cToF(c);
}
// ATF/gearbox temp 222104@7E1: 1 byte. UNVERIFIED — BEST-GUESS raw degC
// (0x5C=92 -> 92C, rose to 96C on-drive). The SAE A-40 alternative would read
// 52-56C instead; only a drive with a known reference point resolves which.
static float decAudiAtfTempF(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 1 || d[0] == 0xFF) return NAN;
  float c = (float)d[0];
  return (c < -40.0f || c > 180.0f) ? NAN : cToF(c);
}
// Charge-air ABSOLUTE pressure 22202A@7E0: 16-bit big-endian mbar
// (0x03EB=1003 ~ atmospheric). UNVERIFIED — shown as GAUGE boost above a
// ~1013 mbar sea-level baseline (no on-car baro reference to subtract, unlike
// GM's MAP-minus-BARO boostPsi()); mbar->psi = 0.0145038.
static float decAudiBoostPsi(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;
  unsigned raw = ((unsigned)d[0] << 8) | d[1];
  if (raw == 0xFFFF) return NAN;
  float psi = ((float)raw - 1013.0f) * 0.014504f;
  return psi < 0.0f ? 0.0f : psi;   // gauge boost: clamp vacuum to 0, matching GM/BMW boostPsi()
}

#define T(wh,ch,wl,cl) Thresholds{wh,ch,wl,cl}
static const float NA = NAN;

// Row order MUST match StatId. Active = cmd set; computed = nullptr + RF_COMPUTED
// (updateComputedReadouts() fills these unconditionally in the real/BLE OBD
// sources regardless of profile — same convention as GM/BMW, NOT the pre-merge
// staged draft, which omitted the flag); unsupported/not-yet-identified =
// nullptr, no flag (never scheduled).
static const ReadoutDef AUDI_READOUTS[] = {
  // name        unit           dec  thr             full   cmd        hdr tier decode           quantity        flags
  {"TRANS",     "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   "222104",  1,  1,  decAudiAtfTempF, Quantity::Temp},                 // ACTIVE 222104@7E1 — UNVERIFIED scaling
  {"OIL",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   "2220A1",  0,  1,  decAudiOilTempF, Quantity::Temp},                 // ACTIVE 2220A1@7E0 — UNVERIFIED scaling
  {"BOOST",     "psi",          1,   T(NA,NA,NA,NA), 30,    "22202A",  0,  0,  decAudiBoostPsi, Quantity::Press},                // ACTIVE 22202A@7E0 — UNVERIFIED scaling/baseline
  {"COOLANT",   "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   "0105",    0,  1,  decTempF,        Quantity::Temp},                 // ACTIVE Mode-01; EA888 thresholds not sourced yet
  {"VOLTS",     "V",            1,   T(NA,NA,11.0f,10.2f),18,"0142",   0,  1,  decVolts,        Quantity::None},                 // ACTIVE Mode-01; universal low-12V floor (generic_obd.cpp precedent)
  {"INTAKE",    "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   "010F",    0,  1,  decTempF,        Quantity::Temp},                 // ACTIVE Mode-01 IAT
  {"RPM",       "",             0,   T(NA,NA,NA,NA), 7000,  "010C",    0,  0,  decRpm,          Quantity::None},                 // ACTIVE Mode-01
  {"SPEED",     "mph",          0,   T(NA,NA,NA,NA), 140,   "010D",    0,  0,  decSpeed,        Quantity::Speed},                // ACTIVE Mode-01
  {"EGT",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 1600,  nullptr,   0,  1,  decNone,         Quantity::Temp},                 // gasoline — no EGT
  {"DPF dP",    "kPa",          1,   T(NA,NA,NA,NA), 70,    nullptr,   0,  1,  decNone,         Quantity::None},                 // gasoline — no DPF
  {"FUEL RATE", "gph",          1,   T(NA,NA,NA,NA), 20,    nullptr,   0,  1,  decNone,         Quantity::Flow},                 // not identified on this drive
  {"LOAD",      "%",            0,   T(NA,NA,NA,NA), 100,   "0104",    0,  1,  decLoad,         Quantity::None},                 // ACTIVE Mode-01
  {"MPG",       "",             1,   T(NA,NA,NA,NA), 40,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"MPG AVG",   "",             1,   T(NA,NA,NA,NA), 40,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"GAL/100",   "",             1,   T(NA,NA,NA,NA), 10,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"L/100km",   "",             1,   T(NA,NA,NA,NA), 15,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"RAIL",      "psi",          0,   T(NA,NA,NA,NA), 30000, nullptr,   0,  0,  decNone,         Quantity::PressHi},              // direct-injection rail — scan target
  {"HP",        "hp",           0,   T(NA,NA,NA,NA), 400,   nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"DEF",       "%",            0,   T(NA,NA,NA,NA), 100,   nullptr,   0,  1,  decNone,         Quantity::None},                 // gasoline — no DEF
  {"FUEL%",     "%",            0,   T(NA,NA,NA,NA), 100,   "012F",    0,  1,  decFuelLevel,    Quantity::None},                 // ACTIVE Mode-01
  {"DSL+",      "gal",          1,   T(NA,NA,NA,NA), 24,    nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},
  {"DEF+",      "gal",          1,   T(NA,NA,NA,NA), 6,     nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},
  {"TORQUE",    "%",            0,   T(NA,NA,NA,NA), 100,   nullptr,   0,  0,  decNone,         Quantity::None},                 // scan target
  {"RefTq",     "",             0,   T(NA,NA,NA,NA), 1000,  nullptr,   0,  2,  decNone,         Quantity::None},                 // scan target
  {"BARO",      "kPa",          0,   T(NA,NA,NA,NA), 110,   nullptr,   0,  2,  decNone,         Quantity::None},                 // not identified on this drive — scan target
  {"MAF",       "g/s",          0,   T(NA,NA,NA,NA), 400,   "0110",    0,  1,  decMaf,          Quantity::None},                 // ACTIVE Mode-01 (None: g/s is not the gph->L/h Flow conversion)
  {"AMBIENT",   "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 150,   nullptr,   0,  1,  decNone,         Quantity::Temp},                 // not identified on this drive — scan target
  {"EGR",       "%",            0,   T(NA,NA,NA,NA), 100,   nullptr,   0,  1,  decNone,         Quantity::None},                 // scan target
  {"PEDAL",     "%",            0,   T(NA,NA,NA,NA), 100,   "0111",    0,  0,  decLoad,         Quantity::None},                 // ACTIVE Mode-01 (A*100/255, same as decLoad)
  {"CAC",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 400,   nullptr,   0,  1,  decNone,         Quantity::Temp},                 // scan target
  {"NOx",       "ppm",          0,   T(NA,NA,NA,NA), 2000,  nullptr,   0,  1,  decNone,         Quantity::None},                 // gasoline — no NOx sensor
  {"OIL P",     "psi",          0,   T(NA,NA,NA,NA), 80,    nullptr,   0,  0,  decNone,         Quantity::Press},                // scan target (no RF_LOW_NEEDS_ENGINE yet)
  {"GEAR",      "",             0,   T(NA,NA,NA,NA), 10,    nullptr,   0,  0,  decNone,         Quantity::None},                 // scan target
};
static_assert(sizeof(AUDI_READOUTS)/sizeof(AUDI_READOUTS[0]) == (size_t)STAT_COUNT,
              "AUDI_READOUTS row count must equal StatId::COUNT");

// --- addressing: standard 11-bit UDS, single AT SH per header (no CEA/CRA
// dance -- that's a BMW-specific requirement to unblock 7DF after an extended-
// addressing enhanced read; Audi's enhanced DIDs answer directly on 7E0/7E1
// alongside Mode-01, per the OBDb capture in docs/AUDI-STATUS.md). ---
static const char* audiEmitEcm(int step, bool, char*, size_t) {
  if (step != 0) return nullptr;
  return "AT SH 7E0\r";
}
static const char* audiEmitTcm(int step, bool, char*, size_t) {
  if (step != 0) return nullptr;
  return "AT SH 7E1\r";
}
static const AddressingDef AUDI_ADDRESSING[] = {
  { "7E0/ecm", audiEmitEcm },   // header 0
  { "7E1/tcm", audiEmitTcm },   // header 1
};
static constexpr int AUDI_ADDRESSING_COUNT =
  (int)(sizeof(AUDI_ADDRESSING) / sizeof(AUDI_ADDRESSING[0]));

// --- layout: 3-page SUV layout (temps+boost / drive / air). No TOW/REGEN/
// RANGE pages (gasoline, no DEF/EGT/DPF). ---
static const StatId AUDI_PAGES[][4] = {
  { StatId::Trans,  StatId::Oil,   StatId::Coolant, StatId::Boost },   // TEMPS
  { StatId::Rpm,    StatId::Speed, StatId::Load,    StatId::Volts },   // DRIVE
  { StatId::Intake, StatId::Pedal, StatId::Maf,     StatId::FuelLevel }, // AIR (FUEL% here so 012F is actually scheduled/shown)
};
static const char* const AUDI_PAGE_NAMES[] = { "TEMPS", "DRIVE", "AIR" };
// No HELPERS: unlike GM's boost (MAP - BARO, needs a polled BARO helper),
// Audi's boost decode uses a fixed sea-level baseline (no baro dependency),
// and no other computed tile is active in this skeleton -- deviates from the
// staged draft, which carried over a StatId::Baro helper that would poll
// nothing (BARO has no cmd here).
static const StatId AUDI_HELPERS[] = {};

static constexpr int LAYOUT_PAGE_COUNT   = (int)(sizeof(AUDI_PAGES)   / sizeof(AUDI_PAGES[0]));
static constexpr int LAYOUT_HELPER_COUNT = (int)(sizeof(AUDI_HELPERS) / sizeof(AUDI_HELPERS[0]));
static_assert(sizeof(AUDI_PAGE_NAMES)/sizeof(AUDI_PAGE_NAMES[0]) == (size_t)LAYOUT_PAGE_COUNT,
              "AUDI_PAGE_NAMES must have one entry per AUDI_PAGES row");

// --- The profile instance -------------------------------------------------
// `extern` on the definition itself is required: a namespace-scope `const` in
// C++ defaults to internal linkage unless previously declared extern IN THIS
// TU (same reasoning as gm_sierra_lz0.cpp / bmw_f10_535i.cpp) — without it the
// symbol would be invisible to main.cpp / every test that links this file.
extern const VehicleProfile AUDI_Q5_PROFILE = {
  "Audi Q5",
  "2.0T TFSI",
  18.5f,                  // fuel tank, gal (typical Q5 FY capacity; unused by this layout)
  0.0f,                   // no DEF (gasoline)
  AUDI_READOUTS,
  { AUDI_PAGES, LAYOUT_PAGE_COUNT, AUDI_PAGE_NAMES, AUDI_HELPERS, LAYOUT_HELPER_COUNT },
  AUDI_ADDRESSING, AUDI_ADDRESSING_COUNT,
};
