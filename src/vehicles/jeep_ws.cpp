// ============================================================================
//  Jeep Wagoneer (2022, WS platform; 5.7L Hemi eTorque), ZF 8HP75.
//  SKELETON profile built from a REAL on-car scan (2026-08-01): a census plus a
//  1024-probe sweep, recorded in docs/JEEP-STATUS.md. Fourteen legislated
//  Mode-01 parameters are confirmed live on this platform, plus two enhanced
//  DIDs that ANSWER but whose meaning is only partly pinned down.
//
//  Alarms OFF on every tile except VOLTS (T(NA,NA,NA,NA)) -- no Hemi/8HP75
//  thresholds have been sourced, the same "no verified thresholds -> alarms
//  off" doctrine the BMW F10 and Audi Q5 skeletons follow.
//
//  ADDRESSING IS THE UNUSUAL PART, and it is why this file does not look like
//  the others. The Wagoneer's 11-bit path is ENTIRELY DEAD: the census probed
//  7DF and 7E0 and both returned NO DATA with zero supported PIDs. Everything
//  lives on 29-bit -- functional 18DB33F1 (53 Mode-01 PIDs) and the TCM at
//  18DA18F1. So these emitters send the 29-bit headers UNCONDITIONALLY and
//  ignore the `can29` argument, unlike gm_sierra_lz0.cpp which switches on it.
//  That is deliberate: no caller in this firmware ever passes can29=true
//  (obdBuildQuery defaults it false in obd_query.h and nothing overrides it),
//  so a GM-style ternary here would always emit 7DF and this profile would read
//  nothing at all on the one platform it was written for.
// ============================================================================
#include "readouts.h"
#include "pid_decode.h"
#include "vehicle_profile.h"
#include <cstdio>

// --- standard Mode-01 decoders (self-contained; verbatim math from
// gm_sierra_lz0.cpp / audi_q5.cpp -- every profile TU keeps its own copy) -----
static float decTempF(const uint8_t* d, int n, const DecodeCtx&) { return (n>0 && d[0]!=0xFF && d[0]!=0x00) ? cToF(decodeTempC(d[0])) : NAN; }
static float decVolts(const uint8_t* d, int n, const DecodeCtx&) { if (n<1) return NAN; float v=decodeModuleVolts(d[0], n>1?d[1]:0); return (v<6.0f||v>18.0f)?NAN:v; }
static float decRpm  (const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeRpm(d[0], d[1]) : NAN; }
static float decSpeed(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? kphToMph(decodeSpeedKph(d[0])) : NAN; }
static float decLoad (const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeLoadPct(d[0]) : NAN; }  // also used for PEDAL (0111): same A*100/255 formula
static float decFuelLevel(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeFuelLevelPct(d[0]) : NAN; }
static float decBaroKpa(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeMapKpa(d[0]) : NAN; }  // 0133: A kPa, same form as 010B
static float decFuelRate(const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeFuelRateGph(d[0], d[1]) : NAN; }
static float decActTq(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeTorquePct(d[0]) : NAN; }
static float decRefTq(const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeRefTorqueNm(d[0], d[1]) : NAN; }
static float decNone (const uint8_t*, int, const DecodeCtx&)    { return 0.0f; }  // computed rows / unsupported placeholders

// --- enhanced decoders: the DIDs are CONFIRMED to answer on a real Wagoneer;
// the byte->meaning mapping below is what remains unverified. ----------------
//
// ATF temp 2204FE@DA18: the scan returned 6204FE 6F 75 76 -- THREE bytes, not
// the one byte the Grand Cherokee signalset implies. Applying that signalset's
// degC = A-40 to each gives 71 / 77 / 78 degC, so all three read as plausible
// ZF 8HP temperatures; sump / converter-out / cooler-out is the usual
// three-sensor arrangement on this gearbox. WHICH byte is the sump is
// UNDETERMINED from a single warm, stationary sample.
//
// Byte A is wired here by deliberate choice, alarms OFF, matching the Audi
// 222104 precedent (ship the best candidate, flag it, confirm on a drive). A
// cold-start warm-up log settles it: the sump lags the others on warm-up and
// leads them under load. Bytes B and C are intentionally NOT discarded from
// the reply -- they are simply not displayed yet.
static float decJeepAtfTempF(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 1 || d[0] == 0xFF) return NAN;
  float c = decodeTempC(d[0]);                       // A - 40 degC
  return (c < -40.0f || c > 180.0f) ? NAN : cToF(c);
}
// Gear 22051A@DA18: 1 byte. The scan returned 62051A DD, byte-exact with the
// 2024 Wagoneer capture, which recorded DD while the vehicle was in Park.
// That is ONE sample of ONE gear -- not enough to build a gear enum, so this
// decoder deliberately reports the RAW byte rather than inventing a mapping.
// Shown as a raw number it is still useful: drive through P-R-N-D and every
// gear, watch the tile, and the enum writes itself. Replace this decoder with
// the real mapping once that log exists.
static float decJeepGearRaw(const uint8_t* d, int n, const DecodeCtx&) {
  return n > 0 ? (float)d[0] : NAN;
}

#define T(wh,ch,wl,cl) Thresholds{wh,ch,wl,cl}
static const float NA = NAN;

// Row order MUST match StatId. Active = cmd set; computed = nullptr +
// RF_COMPUTED; unsupported/not-yet-identified = nullptr, no flag.
//
// "CONFIRMED live" below means the PID appeared in the census's supported-PID
// bitmask on 18DB33F1 (53 PIDs). "NOT SUPPORTED" means it was absent from that
// same bitmask -- a measured negative, not an assumption.
static const ReadoutDef JEEP_READOUTS[] = {
  // name        unit           dec  thr             full   cmd        hdr tier decode           quantity        flags
  {"TRANS",     "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   "2204FE",  1,  1,  decJeepAtfTempF, Quantity::Temp},                 // ACTIVE 2204FE@DA18 — 3 bytes, byte A shown, UNVERIFIED
  {"OIL",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   nullptr,   0,  1,  decNone,         Quantity::Temp},                 // 015C NOT SUPPORTED on this platform (measured: PID 0x5C absent)
  {"BOOST",     "psi",          1,   T(NA,NA,NA,NA), 30,    nullptr,   0,  0,  decNone,         Quantity::Press},                // naturally aspirated 5.7L Hemi — no boost
  {"COOLANT",   "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   "0105",    0,  1,  decTempF,        Quantity::Temp},                 // ACTIVE Mode-01; Hemi thresholds not sourced yet
  {"VOLTS",     "V",            1,   T(NA,NA,11.0f,10.2f),18,"0142",   0,  1,  decVolts,        Quantity::None},                 // ACTIVE Mode-01; universal low-12V floor (generic_obd.cpp precedent)
  {"INTAKE",    "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 300,   "010F",    0,  1,  decTempF,        Quantity::Temp},                 // ACTIVE Mode-01 IAT
  {"RPM",       "",             0,   T(NA,NA,NA,NA), 6000,  "010C",    0,  0,  decRpm,          Quantity::None},                 // ACTIVE Mode-01 (Hemi redline ~5800)
  {"SPEED",     "mph",          0,   T(NA,NA,NA,NA), 120,   "010D",    0,  0,  decSpeed,        Quantity::Speed},                // ACTIVE Mode-01
  {"EGT",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 1600,  nullptr,   0,  1,  decNone,         Quantity::Temp},                 // gasoline — no EGT
  {"DPF dP",    "kPa",          1,   T(NA,NA,NA,NA), 70,    nullptr,   0,  1,  decNone,         Quantity::None},                 // gasoline — no DPF
  {"FUEL RATE", "gph",          1,   T(NA,NA,NA,NA), 30,    "015E",    0,  1,  decFuelRate,     Quantity::Flow},                 // ACTIVE Mode-01 (PID 0x5E) — POWER page; also drives the MPG integrator
  {"LOAD",      "%",            0,   T(NA,NA,NA,NA), 100,   "0104",    0,  1,  decLoad,         Quantity::None},                 // ACTIVE Mode-01
  {"MPG",       "",             1,   T(NA,NA,NA,NA), 30,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"MPG AVG",   "",             1,   T(NA,NA,NA,NA), 30,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"GAL/100",   "",             1,   T(NA,NA,NA,NA), 15,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"L/100km",   "",             1,   T(NA,NA,NA,NA), 25,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"RAIL",      "psi",          0,   T(NA,NA,NA,NA), 30000, nullptr,   0,  0,  decNone,         Quantity::PressHi},              // scan target — not in the 2204xx/2205xx blocks swept
  {"HP",        "hp",           0,   T(NA,NA,NA,NA), 400,   nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},    // ACTIVE (computed) — POWER page; ActTq * RefTq * Rpm, all live
  {"DEF",       "%",            0,   T(NA,NA,NA,NA), 100,   nullptr,   0,  1,  decNone,         Quantity::None},                 // gasoline — no DEF
  {"FUEL%",     "%",            0,   T(NA,NA,NA,NA), 100,   "012F",    0,  1,  decFuelLevel,    Quantity::None},                 // ACTIVE Mode-01
  {"DSL+",      "gal",          1,   T(NA,NA,NA,NA), 30,    nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},
  {"DEF+",      "gal",          1,   T(NA,NA,NA,NA), 6,     nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},
  {"TORQUE",    "%",            0,   T(NA,NA,NA,NA), 100,   "0162",    0,  0,  decActTq,        Quantity::None},                 // ACTIVE Mode-01 (PID 0x62) — POWER page; HP input
  {"RefTq",     "",             0,   T(NA,NA,NA,NA), 700,   "0163",    0,  2,  decRefTq,        Quantity::None},                 // ACTIVE Mode-01 (PID 0x63) — POWER page; HP input
  {"BARO",      "kPa",          0,   T(NA,NA,NA,NA), 110,   "0133",    0,  2,  decBaroKpa,      Quantity::None},                 // ACTIVE Mode-01 (PID 0x33)
  {"MAF",       "g/s",          0,   T(NA,NA,NA,NA), 400,   nullptr,   0,  1,  decNone,         Quantity::None},                 // 0110 NOT SUPPORTED — Hemi is speed-density (0x10 absent, 0x0B present)
  {"AMBIENT",   "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 150,   "0146",    0,  1,  decTempF,        Quantity::Temp},                 // ACTIVE Mode-01 (PID 0x46)
  {"EGR",       "%",            0,   T(NA,NA,NA,NA), 100,   nullptr,   0,  1,  decNone,         Quantity::None},                 // scan target
  {"PEDAL",     "%",            0,   T(NA,NA,NA,NA), 100,   "0111",    0,  0,  decLoad,         Quantity::None},                 // ACTIVE Mode-01 (A*100/255, same as decLoad)
  {"CAC",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA), 400,   nullptr,   0,  1,  decNone,         Quantity::Temp},                 // naturally aspirated — no charge-air cooler
  {"NOx",       "ppm",          0,   T(NA,NA,NA,NA), 2000,  nullptr,   0,  1,  decNone,         Quantity::None},                 // gasoline — no NOx sensor
  {"OIL P",     "psi",          0,   T(NA,NA,NA,NA), 80,    nullptr,   0,  0,  decNone,         Quantity::Press},                // scan target — no oil-pressure DID identified yet
  {"GEAR",      "",             0,   T(NA,NA,NA,NA), 255,   "22051A",  1,  0,  decJeepGearRaw,  Quantity::None},                 // ACTIVE 22051A@DA18 — RAW BYTE, enum not yet mapped (DD = Park)
};
static_assert(sizeof(JEEP_READOUTS)/sizeof(JEEP_READOUTS[0]) == (size_t)STAT_COUNT,
              "JEEP_READOUTS row count must equal StatId::COUNT");

// --- addressing: 29-bit ONLY, emitted unconditionally (see the file header for
// why `can29` is ignored here). Two steps per header, mirroring the AT sequence
// that was actually proven on the vehicle by tools/obd_scan:
//   AT CP 18   -- 29-bit priority/format byte. ELM327's power-on default is
//                 already 0x18, so this is belt-and-braces rather than strictly
//                 required; it costs one AT command per header switch and makes
//                 the sequence self-describing instead of relying on a default.
//   AT SH ...  -- 6-hex-digit header; combined with CP this forms 18DB33F1 /
//                 18DA18F1, exactly what the census and sweep used.
// No CEA/CRA dance -- that is BMW-specific extended addressing, not this.
static const char* jeepEmitFunctional(int step, bool, char*, size_t) {
  if (step == 0) return "AT CP 18\r";
  if (step == 1) return "AT SH DB33F1\r";
  return nullptr;
}
static const char* jeepEmitTcm(int step, bool, char*, size_t) {
  if (step == 0) return "AT CP 18\r";
  if (step == 1) return "AT SH DA18F1\r";
  return nullptr;
}
static const AddressingDef JEEP_ADDRESSING[] = {
  { "18DB33F1/func", jeepEmitFunctional },   // header 0 — all Mode-01 (53 PIDs)
  { "18DA18F1/tcm",  jeepEmitTcm },          // header 1 — ATF temp + gear
};
static constexpr int JEEP_ADDRESSING_COUNT =
  (int)(sizeof(JEEP_ADDRESSING) / sizeof(JEEP_ADDRESSING[0]));

// --- layout: 4-page full-size SUV layout. No TOWING/REGENERATION/RANGE pages (gasoline,
// no DEF/EGT/DPF, and naturally aspirated so no boost page either).
//
// The POWER page is what makes this profile richer than the BMW/Audi skeletons.
// Every input on it was MEASURED live in the census, none is a guess:
//   TORQUE 0162 + RefTq 0163 -> both in the supported-PID bitmask
//   FUEL RATE 015E           -> in the bitmask
//   HP                       -> RF_COMPUTED; updateComputedReadouts() derives it
//                               from ActTq * RefTq * Rpm, all three of which are
//                               now scheduled because they sit on a page.
// Putting FUEL RATE here also starts the Economy integrator (it consumes
// FuelRate + Speed), so the computed MPG rows become live as a side effect even
// though this layout does not display them.
static const StatId JEEP_PAGES[][4] = {
  { StatId::Trans, StatId::Coolant,   StatId::Intake, StatId::Ambient  },  // TEMPERATURES
  { StatId::Rpm,   StatId::Speed,     StatId::Load,   StatId::Pedal    },  // DRIVE
  { StatId::ActTq, StatId::RefTq,     StatId::Hp,     StatId::FuelRate },  // POWER
  { StatId::Gear,  StatId::FuelLevel, StatId::Volts,  StatId::Baro     },  // MISCELLANEOUS
};
static const char* const JEEP_PAGE_NAMES[] = { "TEMPERATURES", "DRIVE", "POWER", "MISCELLANEOUS" };
// No HELPERS: BOOST is absent on this NA engine, so unlike GM there is no
// MAP-minus-BARO computation needing a polled BARO helper -- BARO is a
// first-class tile on the MISCELLANEOUS page instead. No other computed tile is
// displayed in this skeleton.
static const StatId JEEP_HELPERS[] = {};

static constexpr int LAYOUT_PAGE_COUNT   = (int)(sizeof(JEEP_PAGES)   / sizeof(JEEP_PAGES[0]));
static constexpr int LAYOUT_HELPER_COUNT = (int)(sizeof(JEEP_HELPERS) / sizeof(JEEP_HELPERS[0]));
static_assert(sizeof(JEEP_PAGE_NAMES)/sizeof(JEEP_PAGE_NAMES[0]) == (size_t)LAYOUT_PAGE_COUNT,
              "JEEP_PAGE_NAMES must have one entry per JEEP_PAGES row");

// --- The profile instance -------------------------------------------------
// `extern` on the definition itself is required: a namespace-scope `const` in
// C++ defaults to internal linkage unless previously declared extern IN THIS
// TU (same reasoning as gm_sierra_lz0.cpp / audi_q5.cpp) -- without it the
// symbol would be invisible to main.cpp / every test that links this file.
extern const VehicleProfile JEEP_WS_PROFILE = {
  "Jeep Wagoneer",
  "5.7L Hemi eTorque",
  26.5f,                  // fuel tank, gal — APPROXIMATE, not confirmed against
                          // this VIN's build sheet. Only feeds the DSL+ computed
                          // row, which this layout does not display.
  0.0f,                   // no DEF (gasoline)
  JEEP_READOUTS,
  { JEEP_PAGES, LAYOUT_PAGE_COUNT, JEEP_PAGE_NAMES, JEEP_HELPERS, LAYOUT_HELPER_COUNT },
  JEEP_ADDRESSING, JEEP_ADDRESSING_COUNT,
};
