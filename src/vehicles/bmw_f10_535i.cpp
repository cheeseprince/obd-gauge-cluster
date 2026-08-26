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
// Oil pressure 22586F@7DF — 16-BIT MILLIBAR, big-endian. The 2026-07-25 scan
// proved this DID answers on the functional broadcast (the 6F1/612 physical
// path is gateway-blocked and silent).
//
// ⚠️ THIS WAS DECODED BYTE-0-ONLY UNTIL 2026-08-15, AND THAT WAS WRONG.
// The old reading assumed a second module NAKs every 7DF Mode-22 read, making
// the payload [value, 0x7F, 0x22, 0x22]. Re-checked against the drive log
// (obd-display/bmw_drive.csv, 159 samples): every payload is EXACTLY 2 bytes,
// NONE contains 7F2222, and the second byte is spread across 0x87-0xF9. A NAK
// tail is a fixed constant; a byte taking 100+ distinct values is data.
//
// THE INSTRUCTIVE PART: the old note justified itself with "byte0 rose
// monotonically with RPM (9.2 idle -> ~12 at 2000 rpm) — the oil-pressure
// signature". That is true and useless, because THE HIGH BYTE OF A RISING u16
// ALSO RISES. The shape check passed while the magnitude was wrong ~4x, and
// byte-0 put a running engine at ~10 psi, which is a warning-light condition.
//
// As u16 millibar the same drive reads 2324-4776 mbar = 33.7-69.3 psi, median
// 40.4 — textbook for an N55's map-controlled variable-displacement pump — and
// rises monotonically by RPM band (36.7 / 40.4 / 45.3 psi), Pearson r 0.653.
// Non-linear against RPM is expected: the pump regulates to a demand target,
// it does not track engine speed.
//
// Confidence HIGH on the magnitude. Still no cluster oil-pressure readout on
// the F10 to calibrate against, so ALARMS STAY OFF — a low-pressure limit needs
// a sourced N55 minimum and a hot-idle sample, which is the lowest-pressure
// state and is not in this warm-drive log. Any refinement lives in THIS decoder.
// ⚠️ THE SENSOR IS ABSOLUTE, so the raw value must have barometric subtracted.
// Measured three ways on 2026-08-23: ignition-on/engine-OFF it reads 1057-1058
// mbar against a baro of 1000; the engine-off rows inside the drive read 0.3 psi
// once corrected (14.7 psi uncorrected, which is impossible with the engine
// stopped); and the final row as the engine died reads 982 mbar, straight back to
// atmospheric. Median overstatement without this correction: 14.5 psi -- which is
// exactly the offset that would mask a genuine loss of pressure.
static float decBmwOilPress(const uint8_t* d, int n, const DecodeCtx& ctx) {
  if (n < 2) return NAN;                              // needs the full 16 bits
  const uint16_t mbar = (uint16_t)((d[0] << 8) | d[1]);
  if (mbar == 0xFFFF) return NAN;                     // all-ones = not available
  float baroMbar = 1013.25f;                          // std-atmosphere fallback
  if (ctx.values) {
    const float kpa = ctx.values[IDX_BARO];           // NaN fails both comparisons
    if (kpa >= 60.0f && kpa <= 115.0f) baroMbar = kpa * 10.0f;
  }
  float gauge = (float)mbar - baroMbar;
  if (gauge < 0.0f) gauge = 0.0f;                     // clamp, as fBoostPsi does
  return gauge * 0.0145038f;                          // mbar -> psi
}
// Boost from standard MAP (010B, byte A = kPa absolute; PID confirmed present in
// the F10 census). Gauge boost against a fixed sea-level baseline (101.325 kPa),
// the same no-baro-reference approach as the Audi skeleton. Clean Mode-01
// single-frame reply — only Mode-22 draws the 7DF NAK tail — so a normal byte-0
// read is safe. boostPsi() converts (map - baro) kPa to psi.
static float decBmwBoostPsi(const uint8_t* d, int n, const DecodeCtx& ctx) {
  if (n < 1 || d[0] == 0xFF) return NAN;   // 0xFF = ELM "no data" pad; hold last good, not a bogus ~22 psi
  // Was a hardcoded 101.325 kPa sea-level baseline. The 2026-08-23 drive logged
  // baro at 99-100 kPa, so the fixed value overstated boost by ~0.2 psi at rest.
  float baroKpa = 101.325f;
  if (ctx.values) {
    const float kpa = ctx.values[IDX_BARO];
    if (kpa >= 60.0f && kpa <= 115.0f) baroKpa = kpa;
  }
  return boostPsi(decodeMapKpa(d[0]), baroKpa);
}

// Oil temperature 224402@7DF — the community "oil temp after filter" DID, in
// block 0x2244. That block had NEVER BEEN SWEPT: BMW_F10.blocks covered 22DAxx,
// 2258xx, 2242xx and 2245xx only, so the candidate docs/BMW-STATUS.md itself
// names was never in any candidate list. Probed directly 2026-08-23, it answers.
//
// Scale is the community BMW temperature form, raw*0.75-48 (C) — and unlike every
// previous use of that formula on this car, it is now ANCHORED rather than assumed.
// Two DIDs from the same newly-swept blocks land on physically forced values in the
// same probe:
//   224300 ("coolant")      -> 94.5 C, against legislated 0105 reading 99 C
//   224AB0 (boost setpoint) -> 1004 hPa at idle, where it MUST be ~atmospheric
// Both confirm the family. At the probe moment this read 91.5 C with coolant at
// 99 C — oil 7.5 C BELOW coolant at warm idle, the correct sign and a plausible
// magnitude for an N55.
//
// ⚠️ ONE WARM-IDLE SAMPLE. There is no cold->hot ramp for it yet, which is the
// shape evidence that would make this an identification rather than a strong
// candidate. ALARMS STAY OFF until a cold-start drive logs it. Note the lesson
// from decBmwOilPress directly above: a shape check cannot validate a scale, and
// the converse holds too — an anchored scale is not a confirmed identity.
//
// Read as u16: the raws show the second module's 7F2222 arriving on its OWN line
// BEFORE the positive frame (7F2222 62440200BA), not appended to it, so the
// payload handed to this decoder is the clean two bytes.
static float decBmwOilTempF(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;
  const uint16_t raw = (uint16_t)((d[0] << 8) | d[1]);
  if (raw == 0xFFFF) return NAN;
  return cToF((float)raw * 0.75f - 48.0f);
}

// Fuel rate DERIVED FROM MAF (0110), because this DME does not publish one.
//
// 015E and 019D both return NO DATA, and 015E is absent from the DME's own
// supported-PID bitmap — so that is the ECU stating it, not a probe failure.
// Without a fuel rate the four computed economy tiles have nothing to integrate,
// which is why they have always been blank on this car.
//
// MAF gives one anyway: at stoichiometry, fuel mass = air mass / 14.7. Converting
// to US gal/hr is all Economy::update() needs (it takes gal/hr and mph).
// Validated on the 2026-08-23 cold drive: 24.0 mpg over 13.6 mixed miles against
// an EPA 19 city / 29 hwy car, with idle fuel rate 1.57 L/h for a 3.0 L six.
//
// ⚠️ THE ASSUMPTION IS lambda = 1, AND IT BREAKS EXACTLY WHERE IT MATTERS. Under
// boost the DME enriches for knock and thermal protection, so this UNDER-reads
// fuel and OVER-reads mpg when the most fuel is being used. It is a good cruise
// and trip-average number, not a wide-open-throttle one.
//
// The correction exists and is reachable: 0144 EQ_RAT answers on this car
// (lambda 0.9978 at idle, textbook closed loop). Applying it needs a decoder that
// can see a second PID's value, which the one-row-one-command model does not
// provide — so it is left as a follow-up rather than bodged in here.
static float decBmwFuelGphFromMaf(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;
  const float gps = decodeMafGps(d[0], d[1]);          // reuse: 0110 == ((A*256)+B)/100
  if (!(gps > 0.0f) || gps > 655.0f) return NAN;
  const float fuelGps = gps / 14.7f;                   // stoichiometric gasoline
  return fuelGps * 3600.0f / 2830.0f;                  // g/s -> g/hr -> US gal/hr
}

// --- TORQUE, and horsepower from it ----------------------------------------
// 2258BA is CRANK TORQUE. It is named in none of the six community BMW engine
// tables (n55/b48/b58/n63/s55/s63) -- it was found by sweeping 0x2258xx and then
// identified by behaviour, on the 2026-08-24 drive (486 rows, 28 min, to 6354 rpm).
//
// THE IDENTIFICATION IS SCALE-INDEPENDENT, which is what makes it trustworthy:
//   * It reads EXACTLY ZERO on 168 rows, every one of them moving at 22-119 km/h
//     with load 7-18% and MAF 3.6-47 g/s -- closed throttle, deceleration fuel cut.
//     Crank torque collapses to zero on overrun. Nothing else logged does this:
//     load bottoms out at 7%, MAF at 3.6, neither reaches zero.
//   * Peak POWER (torque x rpm) falls at 5940 rpm. Peak TORQUE falls at 3800 rpm,
//     inside the turbo plateau. Where a peak OCCURS does not depend on any scale.
//   * r = +0.938 against an independent MAF-derived power model, on shape alone.
//
// THE SCALE IS 0.1 Nm/count, FROM THE DME'S OWN CALIBRATION.
//
// This file previously carried 0.125, fitted from vehicle physics. That was wrong,
// and the correction comes from a source that needs no assumptions at all: the
// MEVD17.2 XDF -- TunerPro's definition of the DME's own flash binary, naming BMW's
// internal calibration variables (KF_EDA_ANZ_SPORT_MDK_IST, K_ZK_MRVVTOFF and the
// like). In it:
//   * EVERY torque quantity is "X*0.1" with units Nm. 21 uses. "X*0.125" appears on
//     no torque quantity anywhere in the file.
//   * The calibration's own torque ceilings read 410-425 Nm at that scale
//     (Torque request ceiling Auto/Manual, Modeled torque limit 1). At 0.125 the same
//     bytes would be 513-531 Nm, which no N55 ceiling is.
//   * It independently confirms the temperature scale used elsewhere in this file:
//     the rev-limit-vs-engine-temperature axis is "X*0.75-48", units tmot -- exactly
//     the scale anchored on-car against the legislated coolant PID.
//
// THE PHYSICS AGREES, and did all along. A dedicated pull log (891 rows @ 510 ms, two
// WOT pulls to the 6800 rpm limiter) analysed by ENERGY BALANCE -- which integrates
// rather than differentiating, and so is immune to the 1 km/h quantisation of PID
// 010D that destabilised the first estimate -- gives k = 0.1023. One pull was uphill
// and one downhill, so the gravity term this form omits biases them oppositely and
// cancels in the harmonic mean. That is 2% from 0.1 and 18% from 0.125.
//
// AND 224517 LANDS ON THE STOCK FIGURE: 3989 counts x 0.1 = 398.9 Nm, against a stock
// N55 reference of 400 Nm, and inside the calibration's own 410-425 Nm ceiling. At
// 0.125 it would read 498.6 Nm -- ABOVE the ceiling the ECU permits, i.e. impossible.
//
// WHY THE WRONG VALUE SURVIVED SO LONG: 0.125 puts peak power at 356 hp on a car whose
// tune is advertised at ~350, while 0.1 gives 285 hp against a 300 hp stock rating. The
// wrong answer looked plausible and the right one looked impossible. Plausibility is
// not evidence.
//
// ⚠️ WITHDRAWN: an earlier revision of this comment claimed the tune had raised the
// DME's reference torque map to 501.6 Nm. That was an artefact of the wrong scale, not
// an observation. At 0.1 the reference is the stock figure.
//
// ⚠️ 224517 IS NOT CONSTANT. It moves 3985-3989 within a single drive (r = +0.862
// against coolant) and read 4013 on 2026-08-24 against 3989 on 2026-08-25 -- a 0.6%
// drift, sensible since maximum torque capability depends on engine temperature. Far
// too small to matter numerically, but it is why decBmwTorquePct must read the
// reference LIVE rather than hardcode it.
//
// ⚠️ THE DONOR CAR IS TUNED. The scale is a DME unit convention and should transfer to
// any MEVD17.2, but the magnitudes are this car's. ALARMS STAY OFF on all three rows:
// a threshold needs a sourced N55 limit, which no measurement of ours supplies.
//
static constexpr float BMW_TQ_NM_PER_COUNT = 0.1f;

static float decBmwTorquePct(const uint8_t* d, int n, const DecodeCtx& ctx) {
  if (n < 2) return NAN;
  const uint16_t raw = (uint16_t)((d[0] << 8) | d[1]);
  if (raw == 0xFFFF) return NAN;
  // Percent of THIS DME's OWN reference torque, read live from 224517 rather than
  // hardcoded. The donor car is tuned and reports 501.6 Nm where a stock N55
  // reference is ~400 Nm (raw ~3200); a fixed 4013 denominator would make a stock
  // car under-read by ~25%, and computeHorsepower then multiplies that by the LIVE
  // reference -- so the two errors COMPOUND rather than cancel. This PR's own
  // framing is that the scale transfers and the magnitudes do not, so the
  // denominator must not be a magnitude measured on one car.
  float refCounts = 3989.0f;                     // fallback: this car, 2026-08-25
  if (ctx.values) {
    const float refNm = ctx.values[(int)StatId::RefTq];
    // Round-trips Nm back to raw counts. NOTE the scale CANCELS here -- the
    // percentage is raw_act/raw_ref and is scale-invariant. The constant appears
    // only to undo the display conversion, which is why it must be the shared one.
    if (refNm > 100.0f && refNm < 1000.0f) refCounts = refNm / BMW_TQ_NM_PER_COUNT;
  }
  return (float)raw * (100.0f / refCounts);
}
static float decBmwRefTorqueNm(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;
  const uint16_t raw = (uint16_t)((d[0] << 8) | d[1]);
  if (raw == 0xFFFF || raw == 0) return NAN;
  return (float)raw * BMW_TQ_NM_PER_COUNT;    // same unit convention as 2258BA
}

// Straight legislated reads; the helpers already exist in pid_decode.cpp.
static float decBmwRailPsi(const uint8_t* d, int n, const DecodeCtx&) { return n>1 ? decodeRailPsi(d[0], d[1]) : NAN; }
static float decBmwPedalPct(const uint8_t* d, int n, const DecodeCtx&){ return n>0 ? decodeLoadPct(d[0]) : NAN; }  // 49: A*100/255, same form as load

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
  {"OIL",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  300,   "224402",  0,  1,  decBmwOilTempF,  Quantity::Temp},                // ACTIVE 224402@7DF — block 0x2244, never swept until 2026-08-23; ONE warm-idle sample, alarms off
  {"BOOST",     "psi",          1,   T(NA,NA,NA,NA),  30,    "010B",    0,  0,  decBmwBoostPsi,  Quantity::Press},               // ACTIVE — MAP (010B) gauge vs fixed baseline
  {"COOLANT",   "\xC2\xB0""F",  0,   T(239,248,NA,NA),300,   "0105",    0,  1,  decTempF,        Quantity::Temp},                // ACTIVE Mode-01; thresholds sourced 2026-07-26, see note above
  {"VOLTS",     "V",            1,   T(NA,NA,11.0f,10.2f),18,"0142",    0,  1,  decVolts,        Quantity::None},                // ACTIVE Mode-01; universal low-12V floor (generic_obd.cpp precedent)
  {"INTAKE",    "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  300,   "010F",    0,  1,  decTempF,        Quantity::Temp},                // ACTIVE Mode-01 IAT
  {"RPM",       "",             0,   T(NA,NA,NA,NA),  7000,  "010C",    0,  0,  decRpm,          Quantity::None},                // ACTIVE Mode-01
  {"SPEED",     "mph",          0,   T(NA,NA,NA,NA),  160,   "010D",    0,  0,  decSpeed,        Quantity::Speed},               // ACTIVE Mode-01
  {"EGT",       "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  1500,  nullptr,   0,  1,  decNone,         Quantity::Temp},                // diesel — unsupported
  {"DPF dP",    "kPa",          1,   T(NA,NA,NA,NA),  70,    nullptr,   0,  1,  decNone,         Quantity::None},                // diesel
  {"FUEL",      "gph",          1,   T(NA,NA,NA,NA),  15,    "0110",    0,  1,  decBmwFuelGphFromMaf, Quantity::Flow},           // ACTIVE — DERIVED from MAF (no 015E/019D on this DME); lambda=1, see decoder
  {"LOAD",      "%",            0,   T(NA,NA,NA,NA),  100,   "0104",    0,  1,  decLoad,         Quantity::None},                // ACTIVE Mode-01
  {"MPG",       "",             1,   T(NA,NA,NA,NA),  40,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},   // computed (deferred; invalid w/o fuel rate)
  {"MPG AVG",   "",             1,   T(NA,NA,NA,NA),  40,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"GAL/100",   "",             1,   T(NA,NA,NA,NA),  10,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"L/100km",   "",             1,   T(NA,NA,NA,NA),  15,    nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},
  {"RAIL",      "psi",          0,   T(NA,NA,NA,NA),  2500,  "0123",    0,  1,  decBmwRailPsi,   Quantity::PressHi},             // ACTIVE Mode-01; gasoline DI ~1076 psi at idle, so full-scale 2500 not the diesel 36000
  {"HP",        "hp",           0,   T(NA,NA,NA,NA),  400,   nullptr,   0,  1,  decNone,         Quantity::None, RF_COMPUTED},   // COMPUTED — now valid: computeHorsepower(ActTq%, RefTq Nm, rpm)
  {"DEF",       "%",            0,   T(NA,NA,NA,NA),  100,   nullptr,   0,  1,  decNone,         Quantity::None},                // diesel
  {"FUEL%",     "%",            0,   T(NA,NA,NA,NA),  100,   "012F",    0,  1,  decFuelLevel,    Quantity::None},                // ACTIVE Mode-01
  {"DSL+",      "gal",          1,   T(NA,NA,NA,NA),  24,    nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},   // computed (not shown)
  {"DEF+",      "gal",          1,   T(NA,NA,NA,NA),  6,     nullptr,   0,  1,  decNone,         Quantity::Vol,  RF_COMPUTED},
  {"TORQUE",    "%",            0,   T(NA,NA,NA,NA),  100,   "2258BA",  0,  0,  decBmwTorquePct, Quantity::None},                // ACTIVE 2258BA@7DF — crank torque, % of this DME reference; UNNAMED in every community table
  {"RefTq",     "",             0,   T(NA,NA,NA,NA),  1000,  "224517",  0,  2,  decBmwRefTorqueNm, Quantity::None},              // ACTIVE 224517@7DF — constant 4013 = 501.6 Nm (tuned car; stock ref ~400)
  {"BARO",      "kPa",          0,   T(NA,NA,NA,NA),  110,   "0133",    0,  2,  decBaro,         Quantity::None},                // ACTIVE Mode-01
  {"MAF",       "g/s",          0,   T(NA,NA,NA,NA),  300,   nullptr,   0,  1,  decNone,         Quantity::None},                // 0110 IS live, but FUEL RATE already polls it and obd_schedule does not dedupe by cmd — one PID, one request
  {"AMBIENT",   "\xC2\xB0""F",  0,   T(NA,NA,NA,NA),  150,   "0146",    0,  1,  decTempF,        Quantity::Temp},                // ACTIVE Mode-01
  {"EGR",       "%",            0,   T(NA,NA,NA,NA),  100,   nullptr,   0,  1,  decNone,         Quantity::None},                // unsupported
  {"PEDAL",     "%",            0,   T(NA,NA,NA,NA),  100,   "0149",    0,  0,  decBmwPedalPct,  Quantity::None},                // ACTIVE Mode-01 accelerator pedal D
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
  { StatId::MpgInst, StatId::MpgAvg,   StatId::Gal100mi, StatId::L100km },// TRIP — all four computed off FuelRate
  { StatId::Oil,     StatId::Rail,     StatId::FuelRate, _ },             // FLUIDS & FUEL
  { StatId::ActTq,   StatId::RefTq,    StatId::Hp,    StatId::Pedal },     // POWER — demand next to delivery
};
#undef _
static const StatId HELPERS[] = {};   // boost uses a fixed baseline — no BARO helper needed

static constexpr int LAYOUT_PAGE_COUNT   = (int)(sizeof(PAGES) / sizeof(PAGES[0]));
static constexpr int LAYOUT_HELPER_COUNT = (int)(sizeof(HELPERS) / sizeof(HELPERS[0]));

static const char* const PAGE_NAMES[] = { "ENGINE", "DRIVE", "MISCELLANEOUS", "TRIP", "FLUIDS & FUEL", "POWER" };
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
