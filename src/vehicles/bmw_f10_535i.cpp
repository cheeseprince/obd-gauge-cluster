// ============================================================================
//  BMW / 2013 535i (F10, pre-LCI) N55 turbo I6 gasoline, ZF 8HP.
//  Mapped from an on-car scan (census + sweep + drive, 2026-07-25). The scan
//  OVERTURNED the pre-scan addressing premise:
//    * The BMW-specific 6F1 tester addressing (physical 612 = DME, 618 = EGS)
//      is SILENT to a plain ELM327 — those modules sit behind the gateway. The
//      community DA-block DIDs (oil temp DA25@618, ATF DA12@618, oil pressure
//      586F@612) therefore DO NOT ANSWER on this car.
//    * BUT the DME answers BOTH legislated Mode-01 AND enhanced Mode-22 on the
//      standard 7DF FUNCTIONAL broadcast. The enhanced values live in a DIFFERENT
//      block (58xx) than the community DA map.
//  What the drive confirmed (see docs/BMW-STATUS.md):
//    * OIL PRESSURE = 22586F byte 0 — rose monotonically with RPM (9.2@idle ->
//      12@2000rpm), the oil-pressure signature. Scale UNVERIFIED (identity psi).
//    * A rich standard Mode-01 tier (coolant, RPM, speed, MAP/boost, IAT, load,
//      timing, ambient, fuel, baro) — the guaranteed-working part of this dash.
//    * BOOST from standard MAP (010B), gauge vs a fixed sea-level baseline.
//  Still open (need a COLD-START focused drive — the warm 2026-07-25 log had no
//  temperature ramp to pin them): oil temp (58xx byte-0 candidates 225817/2258EB
//  sat 91-98C), ATF/gearbox temp (EGS unreachable — likely never via a plain
//  ELM327). Those tiles are STUBBED. Alarms OFF everywhere: no verified N55
//  thresholds, and every enhanced formula here is a hypothesis.
//
//  Addressing: a SINGLE header — the 7DF functional broadcast — is the only
//  reachable path on this car; every active row uses header 0. Enhanced Mode-22
//  reads on 7DF draw a second module's "7F2222" NAK appended after the positive
//  frame (obd_parse.cpp concatenates them), so enhanced decoders here read
//  BYTE 0 ONLY — a u16 read would ingest the NAK bytes.
// ============================================================================
#include "readouts.h"
#include "pid_decode.h"
#include "vehicle_profile.h"
#include <cstdio>

// --- standard Mode-01 decoders (self-contained; BMW build excludes the GM file) --
static float decTempF(const uint8_t* d, int n, const DecodeCtx&) { return (n>0 && d[0]!=0xFF && d[0]!=0x00) ? cToF(decodeTempC(d[0])) : NAN; }
static float decVolts(const uint8_t* d, int n, const DecodeCtx&) { if (n<1) return NAN; float v=decodeModuleVolts(d[0], n>1?d[1]:0); return (v<6.0f||v>18.0f)?NAN:v; }
static float decRpm(const uint8_t* d, int n, const DecodeCtx&)   { return n>1 ? decodeRpm(d[0], d[1]) : NAN; }
static float decSpeed(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? kphToMph(decodeSpeedKph(d[0])) : NAN; }
static float decLoad(const uint8_t* d, int n, const DecodeCtx&)  { return n>0 ? decodeLoadPct(d[0]) : NAN; }
static float decBaro(const uint8_t* d, int n, const DecodeCtx&)  { return (n>0 && d[0]) ? decodeMapKpa(d[0]) : NAN; }
static float decFuelLevel(const uint8_t* d, int n, const DecodeCtx&) { return n>0 ? decodeFuelLevelPct(d[0]) : NAN; }
static float decNone(const uint8_t*, int, const DecodeCtx&)      { return 0.0f; }

// --- enhanced / turbo decoders ----------------------------------------------
// Oil pressure 22586F@7DF, BYTE 0. The 2026-07-25 scan proved this DID answers
// on the functional broadcast (the 6F1/612 physical path is gateway-blocked and
// silent). On 7DF a second module NAKs every Mode-22 read, so the assembled
// payload is [real value byte, 0x7F, 0x22, 0x22] — decode d[0] ONLY. byte0 rose
// monotonically with RPM over the drive (9.2 idle -> ~12 at 2000 rpm): the
// oil-pressure signature, and 586F is the community-named oil-pressure DID.
// SCALE UNVERIFIED — shown as raw byte == psi (identity), the same "shape
// confirmed, magnitude is a hypothesis" doctrine as the GM decOilPsi. No cluster
// oil-pressure readout exists on the F10 to calibrate against; a cold-start +
// high-RPM drive refines it. Alarms OFF. Any fix lives in THIS decoder alone.
static float decBmwOilPress(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 1 || d[0] == 0xFF) return NAN;
  return (float)d[0];            // psi, IDENTITY scale — UNVERIFIED (shape only)
}
// Boost from standard MAP (010B, byte A = kPa absolute; PID confirmed present in
// the F10 census). Gauge boost against a fixed sea-level baseline (101.325 kPa),
// the same no-baro-reference approach as the Audi skeleton. Clean Mode-01
// single-frame reply — only Mode-22 draws the 7DF NAK tail — so a normal byte-0
// read is safe. boostPsi() converts (map - baro) kPa to psi.
static float decBmwBoostPsi(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 1 || d[0] == 0xFF) return NAN;   // 0xFF = ELM "no data" pad; hold last good, not a bogus ~22 psi
  return boostPsi(decodeMapKpa(d[0]), 101.325f);
}

#define T(wh,ch,wl,cl) Thresholds{wh,ch,wl,cl}
static const float NA = NAN;

// Row order MUST match StatId. Active = cmd set; computed = nullptr + RF_COMPUTED;
// unsupported = nullptr, no flag (never scheduled). decNone keeps check 2 happy.
// ---- Alarm thresholds (VEH-3, researched 2026-07-26) ----------------------
// COOLANT is the ONLY row that gets a threshold. The N55 runs deliberately hot:
// the DME targets 108C/226F in Economy, 104C/219F Normal, 95C/203F High and
// 90C/194F High with the characteristic-map thermostat. So 226F is NORMAL here,
// not a fault — the generic GM-style 235F warn leaves only 9F of headroom and
// would nuisance-fire on a hot day in Economy mode.
//   warn 239F (115C) - clearly above every DME target; owners report this band
//                      as outside normal operation.
//   crit 248F (120C) - deliberately BELOW the ~125C at which the car raises its
//                      own warning and drops into limp. This display exists to
//                      show what the cluster hides, so it should lead the OEM
//                      warning rather than echo it.
// Confidence: HIGH on the DME target range (BMW service documentation for the
// N55 cooling system). MEDIUM on warn/crit themselves - derived from that range
// plus owner reports, NOT from a published OEM fault threshold. Revisit if an
// ISTA/TIS threshold table or a cold-start drive becomes available.
//
// EVERY OTHER ROW STAYS AT T(NA,NA,NA,NA), deliberately:
//   OIL P  - reachable (22586F byte 0) but its SCALE IS UNVERIFIED. A threshold
//            on a guessed scale is worse than none: it either cries wolf or stays
//            silent through a real loss of pressure. Needs the cold-start drive.
//   TRANS  - ATF lives on the EGS module, gateway-blocked on this car.
//   OIL    - oil-temp candidates were never pinned (warm-start drive only).
//   BOOST / LOAD / INTAKE / AMBIENT - decode is sound, but no N55-specific limits
//            are established, and inventing one buys nothing.
// --------------------------------------------------------------------------
static const ReadoutDef BMW_READOUTS[] = {
  // name        unit           dec  thr              full   cmd        hdr tier decode           quantity        flags
  {"TRANS",     "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  300,   nullptr,   0,  1,  decNone,         Quantity::Temp},                // ATF: EGS unreachable via plain ELM327 (gateway) — stub
  {"OIL",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  300,   nullptr,   0,  1,  decNone,         Quantity::Temp},                // DA25@618 silent; 7DF candidates 225817/2258EB — cold-start pending
  {"BOOST",     "psi",          1,   T(NA,NA,NA,NA),  30,    "010B",    0,  0,  decBmwBoostPsi,  Quantity::Press},               // ACTIVE — MAP (010B) gauge vs fixed baseline
  {"COOLANT",   "\xC2\xB0""F",  0,   T(239,248,NA,NA),300,   "0105",    0,  1,  decTempF,        Quantity::Temp},                // ACTIVE Mode-01; thresholds sourced 2026-07-26, see note above
  {"VOLTS",     "V",            1,   T(NA,NA,11.0f,10.2f),18,"0142",    0,  1,  decVolts,        Quantity::None},                // ACTIVE Mode-01; universal low-12V floor (generic_obd.cpp precedent)
  {"INTAKE",    "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  300,   "010F",    0,  1,  decTempF,        Quantity::Temp},                // ACTIVE Mode-01 IAT
  {"RPM",       "",             0,   T(NA,NA,NA,NA),  7000,  "010C",    0,  0,  decRpm,          Quantity::None},                // ACTIVE Mode-01
  {"SPEED",     "mph",          0,   T(NA,NA,NA,NA),  160,   "010D",    0,  0,  decSpeed,        Quantity::Speed},               // ACTIVE Mode-01
  {"EGT",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  1500,  nullptr,   0,  1,  decNone,         Quantity::Temp},                // diesel — unsupported
  {"DPF dP",    "kPa",          1,   T(NA,NA,NA,NA),  70,    nullptr,   0,  1,  decNone,         Quantity::None},                // diesel
  {"FUEL",      "gph",          1,   T(NA,NA,NA,NA),  15,    nullptr,   0,  1,  decNone,         Quantity::Flow},                // gas fuel rate deferred
  {"LOAD",      "%",            0,   T(NA,NA,NA,NA),  100,   "0104",    0,  1,  decLoad,         Quantity::None},                // ACTIVE Mode-01
  {"MPG",       "",             1,   T(NA,NA,NA,NA),  40,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},   // computed (deferred; invalid w/o fuel rate)
  {"MPG AVG",   "",             1,   T(NA,NA,NA,NA),  40,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"GAL/100",   "",             1,   T(NA,NA,NA,NA),  10,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"L/100km",   "",             1,   T(NA,NA,NA,NA),  15,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"RAIL",      "psi",          0,   T(NA,NA,NA,NA),  36000, nullptr,   0,  0,  decNone,         Quantity::PressHi},             // scan target (HPFP)
  {"HP",        "hp",           0,   T(NA,NA,NA,NA),  400,   nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},   // needs torque PIDs — invalid for now
  {"DEF",       "%",            0,   T(NA,NA,NA,NA),  100,   nullptr,   0,  1,  decNone,         Quantity::None},                // diesel
  {"FUEL%",     "%",            0,   T(NA,NA,NA,NA),  100,   "012F",    0,  1,  decFuelLevel,    Quantity::None},                // ACTIVE Mode-01
  {"DSL+",      "gal",          1,   T(NA,NA,NA,NA),  24,    nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},   // computed (not shown)
  {"DEF+",      "gal",          1,   T(NA,NA,NA,NA),  6,     nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},
  {"TORQUE",    "%",            0,   T(NA,NA,NA,NA),  100,   nullptr,   0,  0,  decNone,         Quantity::None},                // scan target
  {"RefTq",     "",             0,   T(NA,NA,NA,NA),  1000,  nullptr,   0,  2,  decNone,         Quantity::None},                // scan target
  {"BARO",      "kPa",          0,   T(NA,NA,NA,NA),  110,   "0133",    0,  2,  decBaro,         Quantity::None},                // ACTIVE Mode-01
  {"MAF",       "g/s",          0,   T(NA,NA,NA,NA),  300,   nullptr,   0,  0,  decNone,         Quantity::None},                // N55 uses MAP for boost; MAF tile off
  {"AMBIENT",   "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  150,   "0146",    0,  1,  decTempF,        Quantity::Temp},                // ACTIVE Mode-01
  {"EGR",       "%",            0,   T(NA,NA,NA,NA),  100,   nullptr,   0,  1,  decNone,         Quantity::None},                // unsupported
  {"PEDAL",     "%",            0,   T(NA,NA,NA,NA),  100,   nullptr,   0,  0,  decNone,         Quantity::None},                // unsupported
  {"CAC",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  400,   nullptr,   0,  1,  decNone,         Quantity::Temp},                // scan target (charge-air temp)
  {"NOx",       "ppm",          0,   T(NA,NA,NA,NA),  2000,  nullptr,   0,  1,  decNone,         Quantity::None},                // diesel
  {"OIL P",     "psi",          0,   T(NA,NA,NA,NA),  80,    "22586F",  0,  0,  decBmwOilPress,  Quantity::Press},               // ACTIVE 22586F@7DF byte0 — UNVERIFIED scale, alarms off
  {"GEAR",      "",             0,   T(NA,NA,NA,NA),  10,    nullptr,   0,  0,  decNone,         Quantity::None},                // scan target
};
static_assert(sizeof(BMW_READOUTS)/sizeof(BMW_READOUTS[0]) == (size_t)STAT_COUNT,
              "READOUTS row count must equal StatId::COUNT");

// --- addressing: BMW F10 — ONE reachable path ------------------------------
// The 6F1 physical headers (612 DME / 618 EGS) the pre-scan skeleton carried are
// GATEWAY-BLOCKED on this car (census: both silent). Everything reachable —
// Mode-01 and the enhanced 58xx Mode-22 block — answers on the 7DF functional
// broadcast, so there is a single header and every active row uses header 0.
static const char* bmwEmitStd(int step, bool, char*, size_t) {
  if (step == 0) return "AT CEA\r";     // disable extended (BMW/29-bit) addressing
  if (step == 1) return "AT CRA\r";     // clear any stale RX filter
  if (step == 2) return "AT SH 7DF\r";  // functional broadcast: DME answers Mode-01 AND Mode-22 58xx here
  return nullptr;
}
static const AddressingDef BMW_ADDRESSING[] = {
  { "std-7DF", bmwEmitStd },   // header 0 — the only reachable path on this car
};
static constexpr int BMW_ADDRESSING_COUNT =
  (int)(sizeof(BMW_ADDRESSING) / sizeof(BMW_ADDRESSING[0]));

// --- layout -----------------------------------------------------------------
// Sedan, gasoline: no TOWING/REGENERATION/RANGE, no DEF/EGT/DPF. Three pages of the tiles
// the scan proved live. Oil temp / ATF land here once a cold-start drive pins
// them; a PERF page (rail/VANOS/lambda) comes with a combustion sweep.
#define _ StatId::COUNT
static const StatId PAGES[][4] = {
  { StatId::Coolant, StatId::OilP,     StatId::Boost, StatId::Load },    // ENGINE
  { StatId::Rpm,     StatId::Speed,    StatId::Intake, StatId::Ambient },// DRIVE
  { StatId::Baro,    StatId::FuelLevel, StatId::Volts, _ },              // MISCELLANEOUS
};
#undef _
static const StatId HELPERS[] = {};   // boost uses a fixed baseline — no BARO helper needed

static constexpr int LAYOUT_PAGE_COUNT   = (int)(sizeof(PAGES) / sizeof(PAGES[0]));
static constexpr int LAYOUT_HELPER_COUNT = (int)(sizeof(HELPERS) / sizeof(HELPERS[0]));

static const char* const PAGE_NAMES[] = { "ENGINE", "DRIVE", "MISCELLANEOUS" };
static_assert(sizeof(PAGE_NAMES)/sizeof(PAGE_NAMES[0]) == (size_t)LAYOUT_PAGE_COUNT,
              "PAGE_NAMES must have one entry per PAGES row");

// `extern` on the definition itself is required: a namespace-scope `const` in
// C++ defaults to internal linkage unless previously declared extern IN THIS
// TU. vehicle_profile.h no longer carries that forward decl (the framework
// dropped it in favor of the vehicle_active.h macros), so without this keyword
// the symbol would be invisible to main.cpp / every test that links this file.
extern const VehicleProfile BMW_F10_535I_PROFILE = {
  "BMW 535i",
  "N55 3.0L Turbo",
  18.5f,                  // fuel tank, gal (unused by this layout)
  0.0f,                   // no DEF tank
  BMW_READOUTS,
  { PAGES, LAYOUT_PAGE_COUNT, PAGE_NAMES, HELPERS, LAYOUT_HELPER_COUNT },
  BMW_ADDRESSING, BMW_ADDRESSING_COUNT,
};
