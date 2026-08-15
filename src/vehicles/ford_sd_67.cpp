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
//  ADDRESSING IS 11-BIT, and this matters: the census found ALL ELEVEN 29-bit
//  headers silent while 7DF/7E0/7E1 answered -- the exact inverse of the
//  Sierra. 7DF is the functional broadcast and returns the union of 7E0+7E1,
//  so it is deliberately NOT used here; polling the physical ECUs keeps one
//  request to one responder instead of relying on which module replies first.
//
//  WHY SO MANY ROWS ARE INACTIVE: a legislated PID is listed ONLY where the
//  census's supported-PID bitmap says this truck answers it. 010F (intake) is
//  NOT in the ECM's bitmap but IS in the TCM's, so it is read at 7E1.
//
//  2026-08-09 CORRECTION: the original pass checked the WRONG PID numbers for
//  several enhanced diesel parameters and concluded them unavailable. A
//  re-check against the same supported-PID bitmap found the ECM DOES answer
//  standard SAE J1979 codes for boost (0187), rail pressure (016D), fuel rate
//  (019D), DEF level (019B), DPF differential pressure (017A), pedal position
//  (0149), charge-air-cooler temp (0177) and EGR (0169) -- eight rows filled
//  below, all decoded from a real 64-sample capture (see the ROWS TO FILL
//  table in the implementation task; formulas were supplied, not guessed).
//  OIL P remains genuinely absent: no PID for it was ever identified.
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

// --- 2026-08-09 addition: eight SAE J1979 rows confirmed on the census's
// supported-PID bitmap and decoded against the same 64-sample capture as the
// rest of this file. Byte offsets and scale factors below are exactly the
// ones supplied with the task -- nothing here was reverse-engineered on the
// spot. Each wrapper follows the file's guard convention: too-short frame,
// or the used bytes pegged at 0xFF (sensor-not-ready sentinel), or a result
// outside a generous physically-sensible range -> NaN, never a reading.

// 0187 @ 7E0 -- BOOST. bytes[1..2] as uint16 * 0.03125 = manifold absolute
// pressure in kPa; subtract barometric (from ctx, when a plausible reading is
// available) or a std-atmosphere fallback of 101.3 kPa; convert to psi;
// clamp negative (below-baro) to zero rather than showing vacuum.
static float fBoostPsi(const uint8_t* d, int n, const DecodeCtx& ctx) {
  if (n < 3) return NAN;                            // need bytes[1..2]
  if (d[1] == 0xFF && d[2] == 0xFF) return NAN;      // sensor-not-ready sentinel
  uint16_t raw = (uint16_t)((d[1] << 8) | d[2]);
  float mapKpa = raw * 0.03125f;
  // 400 kPa abs is far beyond anything a truck-sized single turbo sees
  // (~350 kPa abs at max boost) -- generous headroom, not a real ceiling.
  if (mapKpa <= 0.0f || mapKpa > 400.0f) return NAN;
  float baroKpa = 101.3f;                            // std-atmosphere fallback
  if (ctx.values) {
    float b = ctx.values[IDX_BARO];
    // Plausible-baro range also rejects NaN/0/garbage without needing
    // <cmath>: any comparison against NaN is false, so an unset BARO simply
    // falls through to the fallback above.
    if (b >= 60.0f && b <= 115.0f) baroKpa = b;
  }
  float diffKpa = mapKpa - baroKpa;
  if (diffKpa < 0.0f) diffKpa = 0.0f;
  return kpaToPsi(diffKpa);
}

// 016D @ 7E0 -- RAIL pressure. bytes[3..4] as uint16 * 10 = kPa -> psi.
static float fRailPsi(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 5) return NAN;                             // need bytes[3..4]
  if (d[3] == 0xFF && d[4] == 0xFF) return NAN;       // sensor-not-ready sentinel
  uint16_t raw = (uint16_t)((d[3] << 8) | d[4]);
  float psi = kpaToPsi(raw * 10.0f);
  // fullScale for this tile is 30000 psi (measured max 23717 psi); 35000 is
  // headroom above that, not a claim about a real ceiling.
  return (psi < 0.0f || psi > 35000.0f) ? NAN : psi;
}

// 019D @ 7E0 -- FUEL RATE. bytes[0..1] as uint16 * 0.02 = g/s; g/s -> mL/s via
// diesel density (0.832 g/mL) -> mL/hr -> US gal/hr.
static float fFuelRateGph(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;                             // need bytes[0..1]
  if (d[0] == 0xFF && d[1] == 0xFF) return NAN;       // sensor-not-ready sentinel
  uint16_t raw = (uint16_t)((d[0] << 8) | d[1]);
  float gPerS = raw * 0.02f;
  float gph = (gPerS / 0.832f) * 3600.0f / 3785.41f;
  // A 6.7L diesel at WOT is nowhere near 100 gph -- generous ceiling to
  // reject a corrupt frame, not a claim about real peak consumption.
  return (gph < 0.0f || gph > 100.0f) ? NAN : gph;
}

// 019B @ 7E0 -- DEF level. byte[1] * 100/255 = %.
static float fDefPct(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;                             // need byte[1]
  if (d[1] == 0xFF) return NAN;                       // sensor-not-ready sentinel
  float pct = d[1] * 100.0f / 255.0f;
  return (pct < 0.0f || pct > 100.0f) ? NAN : pct;    // byte math can't exceed this; guard kept explicit
}

// 017A @ 7E0 -- DPF differential pressure. bytes[3..4] as UNSIGNED uint16 *
// 0.01 = kPa -> psi. NOTE this truck's byte offset/sign differ from the
// generic decodeDpfDeltaKpa() in pid_decode.cpp (that one reads bytes[1..2]
// SIGNED) -- do not "simplify" this to reuse that helper, it decodes a
// different manufacturer's layout for the same PID number.
static float fDpfDpPsi(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 5) return NAN;                             // need bytes[3..4]
  if (d[3] == 0xFF && d[4] == 0xFF) return NAN;       // sensor-not-ready sentinel
  uint16_t raw = (uint16_t)((d[3] << 8) | d[4]);
  float psi = kpaToPsi(raw * 0.01f);
  // fullScale for this tile is 15 psi (~103 kPa); 20 psi is headroom above
  // that, not a claim about real DPF dP limits.
  return (psi < 0.0f || psi > 20.0f) ? NAN : psi;
}

// 0149 @ 7E0 -- accelerator PEDAL position. byte[0] * 100/255 = %.
static float fPedalPct(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 1) return NAN;                             // need byte[0]
  if (d[0] == 0xFF) return NAN;                       // sensor-not-ready sentinel
  float pct = d[0] * 100.0f / 255.0f;
  return (pct < 0.0f || pct > 100.0f) ? NAN : pct;
}

// 0177 @ 7E0 -- CAC (charge air cooler) outlet temp. byte[1] - 40 = degC -> degF.
static float fCacF(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;                             // need byte[1]
  if (d[1] == 0xFF) return NAN;                       // sensor-not-ready sentinel
  float c = (float)d[1] - 40.0f;
  return (c < -60.0f || c > 200.0f) ? NAN : cToF(c);  // same plausible-temp bound as ATF
}

// 0169 @ 7E0 -- EGR position/duty. byte[1] * 100/255 = %.
static float fEgrPct(const uint8_t* d, int n, const DecodeCtx&) {
  if (n < 2) return NAN;                             // need byte[1]
  if (d[1] == 0xFF) return NAN;                       // sensor-not-ready sentinel
  float pct = d[1] * 100.0f / 255.0f;
  return (pct < 0.0f || pct > 100.0f) ? NAN : pct;
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
  {"BOOST",    "psi",           1,  T(NA,NA,NA,NA),        30,   "0187",    0,  0,  fBoostPsi,  Quantity::Press},  // MAP - baro; census-confirmed 0187
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
  {"DPF dP",   "psi",           1,  T(NA,NA,NA,NA),        15,   "017A",    0,  2,  fDpfDpPsi,  Quantity::Press},  // census-confirmed 017A (NOT 22116C, which is absent)
  {"FUEL RATE","gph",           1,  T(NA,NA,NA,NA),        20,   "019D",    0,  1,  fFuelRateGph, Quantity::Flow}, // census-confirmed 019D (not the generic-mode 015E form)
  {"LOAD",     "%",             0,  T(NA,NA,NA,NA),        100,  "0104",    0,  0,  fLoad,      Quantity::None},
  {"MPG",      "mpg",           1,  T(NA,NA,NA,NA),        40,   nullptr,   0,  1,  fNone,      Quantity::None},
  {"AVG MPG",  "mpg",           1,  T(NA,NA,NA,NA),        40,   nullptr,   0,  2,  fNone,      Quantity::None},
  {"GAL/100",  "",              1,  T(NA,NA,NA,NA),        20,   nullptr,   0,  2,  fNone,      Quantity::None},
  {"L/100km",  "",              1,  T(NA,NA,NA,NA),        30,   nullptr,   0,  2,  fNone,      Quantity::None},
  {"RAIL",     "psi",           0,  T(NA,NA,NA,NA),        30000,"016D",    0,  1,  fRailPsi,   Quantity::PressHi},// census-confirmed 016D (not the generic-mode 0123 form)
  {"HP",       "hp",            0,  T(NA,NA,NA,NA),        500,  nullptr,   0,  0,  fNone,      Quantity::None},
  {"DEF",      "%",             0,  T(NA,NA,NA,NA),        100,  "019B",    0,  2,  fDefPct,    Quantity::None},   // census-confirmed 019B (not the enhanced 22F485 DID -- see the note below)
  {"FUEL%",    "%",             0,  T(NA,NA,NA,NA),        100,  "012F",    0,  1,  fFuelLevel, Quantity::None},
  {"DSL FILL", "gal",           1,  T(NA,NA,NA,NA),        48,   nullptr,   0,  2,  fNone,      Quantity::None},   // tank capacity unknown for this truck
  {"DEF FILL", "gal",           1,  T(NA,NA,NA,NA),        8,    nullptr,   0,  2,  fNone,      Quantity::None},
  {"ACT TQ",   "%",             0,  T(NA,NA,NA,NA),        100,  "0162",    0,  1,  fActTq,     Quantity::None},
  // 2000, not 1500: this is the engine's reference-torque CONSTANT, and the
  // 2026-08-09 scan read 1615 Nm on all 63 samples (0x064F). A full-scale below
  // the reported value pinned the bar at 100% permanently -- and because the
  // value never moves, it could never un-pin. 2000 also clears the High-Output
  // 6.7L (1200 lb-ft = 1627 Nm).
  {"REF TQ",   "",              0,  T(NA,NA,NA,NA),        2000, "0163",    0,  2,  fRefTq,     Quantity::None},
  {"BARO",     "kPa",           0,  T(NA,NA,NA,NA),        110,  "0133",    0,  2,  fBaro,      Quantity::Press},
  {"MAF",      "g/s",           0,  T(NA,NA,NA,NA),        400,  "0110",    0,  1,  fMaf,       Quantity::None},
  {"AMBIENT",  "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        150,  "0146",    0,  2,  fTempF,     Quantity::Temp},
  {"EGR",      "%",             0,  T(NA,NA,NA,NA),        100,  "0169",    0,  2,  fEgrPct,    Quantity::None},   // census-confirmed 0169 (not the generic-mode 012C form)
  {"PEDAL",    "%",             0,  T(NA,NA,NA,NA),        100,  "0149",    0,  1,  fPedalPct,  Quantity::None},   // census-confirmed 0149 (not the generic-mode 0111 form)
  {"CAC",      "\xC2\xB0""F",   0,  T(NA,NA,NA,NA),        300,  "0177",    0,  1,  fCacF,      Quantity::Temp},   // census-confirmed 0177
  {"NOx",      "ppm",           0,  T(NA,NA,NA,NA),        400,  "0183",    0,  2,  fNox,       Quantity::None},
  {"OIL P",    "psi",           0,  T(NA,NA,NA,NA),        80,   nullptr,   0,  0,  fNone,      Quantity::Press},  // not identified in this scan
  {"GEAR",     "",              0,  T(NA,NA,NA,NA),        10,   "221E60",  1,  0,  fGear,      Quantity::None},
};
static_assert(sizeof(FORD_READOUTS)/sizeof(FORD_READOUTS[0]) == (size_t)STAT_COUNT,
              "FORD_READOUTS row count must equal StatId::COUNT");

// 2026-08-09: DEF is no longer dark. The earlier note here was about the
// enhanced DID 22F485, which answered but returned a CONSTANT 10-byte payload
// across all 64 samples and so could not be attributed to a byte -- that
// finding stands, 22F485 is still not used. DEF is instead read from the
// standard SAE PID 019B byte[1], which is a DIFFERENT signal path the census
// separately confirmed the ECM supports; it produced a plausible, non-stuck
// 49.8% on the same capture (3F 7F 40 95 -> byte[1]=0x7F -> 127*100/255).

// Four pages, diesel. GEAR earns a tile: it is a confirmed enhanced parameter
// with no generic equivalent, and unlike the Sierra (whose DIC already shows
// gear) nothing else on this dash would display it.
//
// POWER swaps ActTq/RefTq for BOOST/RAIL (2026-08-09): both are now
// census-confirmed live psi gauges -- the two parameters a diesel owner
// actually wants to watch -- and are a clearly-better use of two tiles than
// raw torque %/N*m, which exist mainly to feed the HP calc. ActTq and RefTq
// keep polling as helpers below so HP still computes; this mirrors GM
// Sierra's identical POWER-page trade (gm_sierra_lz0.cpp:143).
//
// The other six newly-live rows (FUEL RATE, DEF, DPF dP, PEDAL, CAC, EGR) are
// NOT added to a page here. All 16 tile slots across the existing 4 pages
// were already occupied by rows this profile had already committed to
// showing, and the task asked to keep 4 pages and swap conservatively. They
// are scheduled as helpers instead: polled every cycle, recorded in the SD
// log, but not shown as a tile. DEF (low-DEF derates the engine) and DPF dP
// (regen indicator) are the two of these six most worth a dash tile if this
// layout gets revisited -- see the implementation report for the trade-off.
// SEVEN pages, matching the Sierra's depth. This is only possible because the
// 2026-08-09 re-reading of the census found that this truck serves nine more
// parameters as STANDARD J1979 PIDs than the original profile believed -- the
// original checked the wrong PID numbers (0111/0123/012C/015E) and concluded
// the parameters were unavailable, when the truck serves them at
// 0149/016D/0169/019D. See the per-row comments.
//
// PAGE ORDER IS DELIBERATE. Pages 1-5 contain ONLY tier-0 and tier-1 stats plus
// computed rows that depend on them; pages 6-7 hold every stat that is tier 2 OR
// depends on a tier-2 value (HP needs RefTq). Keep it that way: grouping the
// rare tier makes its slower fill rate legible instead of scattering one
// late-populating tile across three pages.
//
// This grouping was originally a diagnostic for FORD-BUG-1 ("the whole tier-2
// set never renders"). THAT BUG WAS NEVER IN THIS FIRMWARE and is closed:
// root-caused 2026-08-10 to a leak in the bench BLE shim, which desynced the
// reply stream and fed the dash stale/mismatched frames (REF TQ handed the
// Mode-09 VIN reply, BARO handed RPM's). parseObdResponse() correctly rejected
// every mismatch, so valid[] was never set and the tiles read "--". Fixing the
// shim fixed the tiles. Re-verified 2026-08-15 at every layer -- wire, parse,
// decode, valid[], gauge state, layout, rendered pixels -- on this commit and
// on the one the bug was filed against: all clean, and nothing in the
// query-to-display path is tier-dependent in the first place.
//
// Two things worth keeping from it: blank tiles are evidence about the LINK,
// not about the rare tier; and tier-2 genuinely fills slowly (first reading
// 18-51 s after link-up on the bench rig), so pages 6-7 read "--" for the first
// minute by design. Do not re-open without a fresh observation AND a serial log.
// A stat must appear on EXACTLY ONE page. nav_model's readoutPageOf() returns
// the FIRST page holding a stat and cursorStep() finds its FIRST slot in the
// reading order, so a duplicate makes the knob teleport backwards -- putting
// FuelLevel on both FUEL and RANGE sent page 8 to page 4 on the bench. `_` is a
// deliberately empty cell, the same placeholder gm_sierra_lz0 uses.
#define _ StatId::COUNT
static const StatId FORD_PAGES[][4] = {
  { StatId::Rpm,       StatId::Speed,    StatId::Coolant,  StatId::Load     },  // ENGINE
  { StatId::Trans,     StatId::Oil,      StatId::Egt,      StatId::Cac      },  // THERMAL
  { StatId::Boost,     StatId::Rail,     StatId::ActTq,    StatId::Gear     },  // POWER
  { StatId::FuelRate,  StatId::MpgInst,  StatId::MpgAvg,   StatId::Gal100mi },  // TRIP
  { StatId::Maf,       StatId::Intake,   StatId::Pedal,    StatId::Volts    },  // AIR
  { StatId::DpfDp,     StatId::Egr,      StatId::Nox,      _                },  // EMISSIONS
  { StatId::Ambient,   StatId::Baro,     StatId::RefTq,    StatId::Hp       },  // AMBIENT
  // RANGE: each tank level beside its gallons-to-fill partner -- byte-for-byte
  // the same page gm_sierra_lz0 ships, which is why FuelLevel and Def live HERE
  // and not on TRIP/EMISSIONS. Nothing was lost in the move: FUEL became TRIP
  // (economy) and EMISSIONS keeps all three of its real parameters.
  { StatId::FuelLevel, StatId::DslFill,  StatId::Def,      StatId::DefFill  },  // RANGE
};
#undef _
static const char* const FORD_PAGE_NAMES[] = { "ENGINE", "THERMAL", "POWER", "TRIP",
                                               "AIR", "EMISSIONS", "AMBIENT", "RANGE" };

// Every polled row now has a tile, so the helper list is empty of *display*
// duties. It is NOT empty: OilP has no command at all on this truck (no
// standard PID exists for engine oil pressure and the scan found no enhanced
// DID), so it stays dark by design rather than by omission.
//
// DslFill/DefFill are no longer in that company. DEF FILL works from the 7.4
// gal constant below; DIESEL FILL reads SET UP until the owner picks a
// capacity in Settings -> Fuel tank, because this truck's tank size is a
// wheelbase option the vehicle cannot be asked about.
//
// MPG/AVG MPG are live because updateComputedReadouts() derives them from
// FuelRate + Speed, both of which now read. HP is on page 7 rather than POWER
// because computeHorsepower() needs values[RefTq], which is tier 2 -- so HP is
// dark for exactly as long as FORD-BUG-1 is open, and belongs with the other
// casualties rather than stranded on a working page.
static const StatId FORD_HELPERS[] = { StatId::L100km };

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
  // Fuel tank: UNKNOWABLE from the truck itself, so it is a user setting
  // (Settings -> Fuel tank). The Owner's Manual, "Capacities and
  // Specifications - 6.7L Diesel", keys it to WHEELBASE:
  //     142/148 in -> 29.0 gal      160/164 in -> 34.0 gal
  //     176 in     -> 48.0 gal
  // (plus 26.5 / 40 / 66.5 gal on an "incomplete vehicle" -- Ford's term for a
  // cab-and-chassis, which is why those three never appear on a pickup).
  //
  // The VIN carries neither wheelbase nor bed length: NHTSA vPIC returns Bed
  // Length, Bed Type and Wheel Base empty for a Super Duty, and its Cab Type is
  // one lumped "Crew/Super Crew/Crew Max" bucket. There is no lookup to do here
  // and no runtime way to ask -- WiFi is OTA-only and suspends OBD -- so 0.0f
  // means "ask the user", which the DIESEL FILL tile renders as SET UP.
  0.0f,
  // DEF tank: 7.4 gal, quoted from the same table --
  //     "Diesel Exhaust Fluid (DEF) (complete vehicle)   28 L (7.4 gal)"
  //     "Diesel Exhaust Fluid (DEF) (incomplete vehicle) 27.3 L (7.2 gal)"
  // 7.4 is the pickup figure; 7.2 is the cab-and-chassis. Unlike the fuel tank
  // this does NOT vary by cab or bed, so it is a constant rather than a
  // setting. (docs/FORD-STATUS.md previously said 7.5 -- a transcription slip,
  // corrected in the same change as this line.)
  7.4f,
  FORD_READOUTS,
  { FORD_PAGES, FORD_PAGE_COUNT, FORD_PAGE_NAMES, FORD_HELPERS, FORD_HELPER_COUNT },
  FORD_ADDRESSING, FORD_ADDRESSING_COUNT,
};
