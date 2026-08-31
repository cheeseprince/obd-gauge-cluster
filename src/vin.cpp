#include "vin.h"
#include "vehicle_registry.h"
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

bool parseVinFromPayload(const uint8_t* payload, int len, char out[18]) {
  if (!payload || len < 17) return false;
  const uint8_t* p = payload + (len - 17);          // trailing 17 bytes
  for (int i = 0; i < 17; i++) {
    unsigned char c = p[i];
    if (!std::isalnum(c)) return false;
    out[i] = (char)std::toupper(c);
  }
  out[17] = '\0';
  return true;
}

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Assemble an ELM327 multi-frame ISO-TP reply ("LLL\r0:HEX\r1:HEX\r...>") into
// raw bytes.
//
// NOTE on why this duplicates obd_parse.cpp's (file-local, static)
// assembleMultiFrame() instead of reusing parseObdResponse(): verified via
// `grep -nE 'parseObdResponse' src/obd_parse.h src/obd_parse.cpp` that its
// signature is `bool parseObdResponse(const std::string&, uint8_t mode,
// uint16_t pid, std::vector<uint8_t>&)` and that it only validates two byte
// layouts: mode 0x01 (service 0x41, pid echoed low byte, data = rest) and
// mode 0x22 (service 0x62, 16-bit pid echoed, data = rest). Mode-09 (VIN)
// falls through to `return false` — there is no branch for it, so it cannot
// be called with mode=0x09. The ISO-TP frame *assembly* (the "LLL\r0:HEX\r..."
// stitching + declared-length padding strip) is mode-agnostic and identical
// to what Mode-22 already uses; only the post-assembly service/pid validation
// differs for Mode-09 (service 0x49, pid 0x02, then a message-count byte
// before the 17 VIN bytes — no such count byte exists in the mode-01/22
// layouts). Per the implementation plan (Task 1 Step 3), the sanctioned fix
// is to assemble+validate the Mode-09 layout locally here rather than modify
// obd_parse.cpp (out of scope for this task).
bool assembleIsoTpFrames(const std::string& resp, std::vector<uint8_t>& bytes) {
  bytes.clear();
  bool sawFrame = false;
  int declaredLen = -1;
  int expectFrame = 0;
  std::string acc;

  size_t start = 0;
  while (start <= resp.size()) {
    size_t end = resp.find_first_of("\r\n", start);
    std::string raw = (end == std::string::npos) ? resp.substr(start)
                                                   : resp.substr(start, end - start);
    // Normalize the line: uppercase, drop spaces/tabs (mirrors obd_parse.cpp).
    std::string t;
    for (char ch : raw)
      if (ch != ' ' && ch != '\t')
        t += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    size_t colon = t.find(':');
    if (colon != std::string::npos) {
      // Frame line "N:HEX". Validate the ISO-TP frame ordinal N (the ELM cycles
      // the sequence index in the low nibble 0..F): a reordered, duplicated or
      // dropped frame (a BLE-notify glitch) must be REJECTED, not silently
      // spliced into the accumulator — a wrong VIN maps to a wrong vehicle
      // profile, so this fails closed rather than hand back corrupt bytes.
      if (colon == 0) return false;                 // ":HEX" — no frame index
      // Masked to a byte as it accumulates. The unbounded version overflowed a
      // signed int on a long run of hex digits -- undefined behaviour, found by
      // the fuzzer against the identical code in obd_parse.cpp. Only the low
      // nibble is ever compared, so a byte is both bounded and correct.
      unsigned idx = 0;
      for (size_t k = 0; k < colon; k++) {
        int hv = hexVal(t[k]);
        if (hv < 0) return false;                    // non-hex frame index
        idx = ((idx << 4) | (unsigned)hv) & 0xFFu;
      }
      if ((idx & 0x0Fu) != ((unsigned)expectFrame & 0x0Fu)) return false;  // reorder/dup/miss
      expectFrame++;
      sawFrame = true;
      for (size_t k = colon + 1; k < t.size(); k++)
        if (hexVal(t[k]) >= 0) acc += t[k];
    } else if (t.size() == 3 &&
               hexVal(t[0]) >= 0 && hexVal(t[1]) >= 0 && hexVal(t[2]) >= 0) {
      // Length header line, e.g. "014" (20 decimal bytes).
      declaredLen = (hexVal(t[0]) << 8) | (hexVal(t[1]) << 4) | hexVal(t[2]);
    }
    // Any other line (OK / SEARCHING... / blank / ">" prompt) is ignored.

    if (end == std::string::npos) break;
    start = end + 1;
  }

  if (!sawFrame) return false;
  if (declaredLen < 0) {
    // No "LLL" length header: unlike obd_parse.cpp's assembleMultiFrame() (which
    // caps at 128 hex chars / 64 bytes and lets the caller's mode/pid check catch
    // anything nonsensical), VIN correctness depends on taking the TRAILING 17
    // bytes of the accumulated hex (see parseVinFromPayload). Any uncapped
    // trailing ISO-TP 0x55 pad bytes would silently shift that window and hand
    // back a wrong-but-plausible VIN instead of failing closed. Fail closed:
    // require the declared-length header for a multi-frame reply.
    return false;
  }
  size_t want = static_cast<size_t>(declaredLen) * 2;
  if (acc.size() < want) return false;                // dropped fragment -> reject
  if (acc.size() > want) acc.resize(want);             // strip ISO-TP 0x55 pad frame
  if (acc.size() % 2 != 0) return false;

  for (size_t i = 0; i + 1 < acc.size(); i += 2) {
    int hi = hexVal(acc[i]);
    int lo = hexVal(acc[i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

}  // namespace

bool parseVinReply(const char* elmReply, char out[18]) {
  if (!elmReply) return false;
  std::vector<uint8_t> bytes;
  if (!assembleIsoTpFrames(std::string(elmReply), bytes)) return false;
  // Mode-09 PID-02 layout: 49 02 <message-count> <17 VIN bytes...>
  if (bytes.size() < 2 || bytes[0] != 0x49 || bytes[1] != 0x02) return false;
  return parseVinFromPayload(bytes.data() + 2, (int)(bytes.size() - 2), out);
}

// A WMI alone is not always enough to pick a profile, so a row may carry an
// extra predicate over the FULL VIN. Rows without one stay pure WMI matches.
// Returning false makes the row fail CLOSED (nullptr) rather than falling
// through to a later row -- a WMI is owned by one manufacturer, so a failed
// discriminator means "we cannot tell", not "try the next entry".
//
// GM light-truck WMIs (1GT/3GT/1GC/3GC) cover far more than the LZ0 diesel this
// profile was scanned on: gasoline Silverado/Sierra 1500 (L84 5.3 V8, L87 6.2 V8,
// L3B 2.7 turbo-4) and Sierra/Silverado HD (L5P 6.6 Duramax) share them. Give any
// of those the LZ0 profile and the dash polls Mode-22 PIDs (221940 trans temp,
// 220078 EGT, 220023 rail, 220010 MAF) at headers 7E0/7E2 that either do not
// exist or mean something different -- the over-broad-WMI failure the jeep_ws
// rows below already warn about.
//
// NHTSA vPIC keys the 1500's engine entirely off VIN position 8 (vin[7]), gated
// on positions 4-5 (vin[3], vin[4]). vPIC key format is vin[3..8] + "|" +
// vin[9..17], "*" = any:
//   [NPRUV][HU]**8 -> LZ0 3.0L I6 turbo diesel   <-- the only fit for this profile
//   [NPRUV][HU]**D -> L84 5.3L V8 gas
//   [NPRUV][HU]**L -> L87 6.2L V8 gas
//   [NPRUV][HU]**K -> L3B 2.7L I4 turbo gas
// ⚠️ That vin[3]/vin[4] keying holds for the 2022+ trucks this profile targets,
// but it is NOT how 1500 and HD are told apart in general: in the T1XX era
// (2019-2021) the 1500 shares the HD's vin[3]/vin[4] space entirely. TONNAGE
// LIVES AT vin[5] -- ABCDEFG=1500, LMNPR=2500, STUVW=3500, identical on MY2020
// through MY2024. See gmHeavyDuty/gmLightDutyT1XX below, and note this comment
// once said "vin[4] is what separates 1500 from HD", which was wrong and is how
// a 2020 Sierra 1500 came to be named "Sierra HD".
// Every discriminator below reads vin[3..7], so they all need >= 8 characters.
// vinAt() upper-cases one position; vinIs() tests membership in a small set.
// The explicit c!=0 guard matters: strchr(set, '\0') returns a pointer to the
// set's terminator, i.e. NUL would otherwise "match" every set.
static inline char vinAt(const char* vin, int i) {
  return (char)std::toupper((unsigned char)vin[i]);
}
static inline bool vinIs(const char* set, char c) {
  return c != '\0' && std::strchr(set, c) != nullptr;
}

// vin[9] is VIN position 10: the MODEL YEAR code. Verified against NHTSA vPIC,
// which resolves the year from that position when given no hint:
//   N=2022  P=2023  R=2024  S=2025  T=2026  V=2027
//
// The positional engine/model rules are NOT unique across 20 model years, so a
// profile row needs to say which years it was actually established on. Found
// 2026-08-05 by sweeping vPIC over every year code:
//   * a 2011-12 Chevrolet EXPRESS VAN (6.6 Duramax) satisfies the GM 1500
//     diesel rule exactly, and
//   * a Chrysler Voyager (2001-03) satisfies the Jeep Wagoneer rule.
// Neither is theoretical -- a working van is likelier to meet an OBD dongle
// than most vehicles.
//
// CAUTION: year codes REPEAT every 30 years (N is 1992 and 2022), so this gate
// alone cannot separate a 1992 vehicle from a 2022 one on the same WMI. Where
// that matters, the WMI list is the discriminator -- see the Jeep rows.
//
// A year outside the verified range fails CLOSED: a 2027 truck gets Generic
// until someone confirms the profile still fits it.
static bool vinModelYearIn(const char* vin, const char* codes) {
  if (std::strlen(vin) < 10) return false;
  return vinIs(codes, vinAt(vin, 9));
}

static bool gmSierra1500Diesel(const char* vin) {
  if (std::strlen(vin) < 8) return false;            // need vin[7]; guards the reads below
  return vinIs("NPRUV", vinAt(vin,3)) &&
         vinIs("HU",    vinAt(vin,4)) &&
         vinAt(vin,7) == '8' &&
         // 2023-2026, the years this profile is established on. Excludes the
         // 2011-12 Express van, which matches every positional rule above.
         vinModelYearIn(vin, "PRST");
}

// BMW F10 535i (N55 3.0 turbo I6) — the car this profile was scanned on.
// vPIC frame, verified 2011-2015:
//   vin[3]='F', vin[4]='R' (RWD) or 'U' (xDrive), vin[5]=model, vin[6]='C', vin[7]='5'
// vin[5] is the model digit and it is the ONLY workable separator:
//   1 -> 528i, 7 -> 535i, 9 -> 550i.
// DISPLACEMENT CANNOT BE USED: the 528i is also 3.0L/6-cyl in 2011-13, so a
// "3.0 six" test would happily match an N52 528i and poll N55 PIDs at it.
// The frame decodes to nothing from 2016 onward (F10 replaced by the G30), so
// the pattern is self-limiting and needs no model-year gate.
static bool bmwF10535i(const char* vin) {
  if (std::strlen(vin) < 8) return false;
  return vinAt(vin,3) == 'F' &&
         vinIs("RU", vinAt(vin,4)) &&
         vinAt(vin,5) == '7' &&
         vinAt(vin,6) == 'C' &&
         vinAt(vin,7) == '5';
}

// Audi Q5 (typ FY) 2.0T TFSI EA888.3, 2018-20.
//   vin[3] = trim (A/B/C), vin[4] = engine/line, vin[6]='F', vin[7] = model line
// Two separators do the work:
//   vin[4]: 'N' -> Q5 2.0T, '4' -> SQ5 (EA839 3.0 V6 — a different engine with
//           different PIDs, and the row this most needed to stop matching).
//   vin[7]: 'Y' -> Q5, '1' -> Q8, '3' -> Q3, '7' -> Q7.
// Requiring vin[4]='N' also excludes the 2021+ facelift, which moved the Q5 to
// vin[4]='A' — correct, since the profile was only ever validated on 2018-20.
static bool audiQ5_20T(const char* vin) {
  if (std::strlen(vin) < 8) return false;
  return vinIs("ABC", vinAt(vin,3)) &&
         vinAt(vin,4) == 'N' &&
         vinAt(vin,6) == 'F' &&
         vinAt(vin,7) == 'Y';
}

// Jeep Wagoneer (WS) 5.7L Hemi eTorque, 2022-23.
// 1C4/1J4/3C4 are Stellantis-wide. Left on the WMI alone this row matched the
// Grand Cherokee, Cherokee, Wrangler AND the Ducato / ProMaster VANS — every one
// of which would then be driven with the Wagoneer's 29-bit addressing.
//   vin[4]: 'J' -> Wagoneer family ('F' = Ducato, 'R' = ProMaster)
//   vin[5]: R/S/U/V -> Wagoneer (E,F,G,H,Y = Grand Cherokee; M = Cherokee; X = Wrangler)
//   vin[6]: A/B/D  -> Wagoneer   (E,F,G = Grand Wagoneer)
//   vin[7]: ENGINE — 'T' = 5.7 V8, 'P' = 3.0 I6 Hurricane, 'J' = 6.4 V8, 'G' = 3.6 V6
// The 5.7 is a 2022-23 combination only; 2024 Wagoneers are 3.0 I6 and fall out
// on vin[7] without needing a year check.
static bool jeepWagoneer57(const char* vin) {
  if (std::strlen(vin) < 8) return false;
  return vinAt(vin,4) == 'J' &&
         vinIs("RSUV", vinAt(vin,5)) &&
         vinIs("ABD",  vinAt(vin,6)) &&
         vinAt(vin,7) == 'T' &&
         // 2022-2023 only: the 5.7 Hemi years. 2024+ Wagoneers are the 3.0
         // Hurricane, and this also excludes the 2001-03 Voyager.
         vinModelYearIn(vin, "NP");
}

// Ford Super Duty with the 6.7L Power Stroke. vPIC frame, verified across EVERY
// model-year code B..W (2011-2028): vin[3]='7' is an F-250 and '8' an F-350,
// with vin[4]='W' and vin[6..7]='BT'. Unusually, this pattern IS stable across
// the whole range -- it reports F-250/F-350 6.7L at every year and nothing else
// -- so unlike the GM and Jeep rules it needs no model-year gate.
//
// SCANNED 2026-08-09 (a 2021 F-350), so this now selects a real profile as well
// as identifying -- see profileKeyFor's Ford row and src/vehicles/ford_sd_67.cpp.
// The predicate already pins the ENGINE at vin[7]='T', so a 6.2L/7.3L gas Super
// Duty fails it and falls through to Standard+ Gas rather than being handed a
// profile that reads 10R140 transmission DIDs it does not have.
static bool fordSuperDuty67(const char* vin) {
  if (std::strlen(vin) < 8) return false;
  // SERIES + ENGINE, and deliberately nothing else.
  //
  //   vin[5] = series   2=F-250 3=F-350 4=F-450 5=F-550
  //   vin[7] = engine   T=6.7L Power Stroke  6=6.2L V8  N=7.3L V8
  //
  // The series check is what keeps an F-150 out: vin[7]='T' is a 6.7L on a
  // Super Duty and a 3.5L EcoBoost on an F-150 (docs/VEHICLES.md#ford), so
  // engine alone would hand this profile to the wrong truck. F-150 is vin[5]='1'.
  //
  // ⚠️ THIS USED TO ALSO REQUIRE vin[4]=='W' AND vin[6]=='B', WHICH WAS A BUG.
  // A vPIC 2-D sweep of both positions across the full 33-character alphabet
  // (1089 combinations, 2026-08-13) found 24 valid (vin[4],vin[6]) pairs for a
  // 6.7L Super Duty. That gate accepted exactly ONE of the 24:
  //
  //   vin[4] = CAB STYLE            F=Regular  W=Crew  X=SuperCab
  //   vin[6] = REAR WHEELS + DRIVE  A/E=SRW 4x2  B/F=SRW 4WD
  //                                 C/G=DRW 4x2  D/H=DRW 4WD
  //
  // So it admitted Crew-Cab / single-rear-wheel / 4WD and rejected every
  // Regular Cab, every SuperCab, every dually and every 4x2 -- the same shape
  // as the identification bug fixed in PR #51, which "rejected every 4x2 Super
  // Duty and every cab style but one". Neither cab nor axle changes which PIDs
  // a truck answers; the engine and the transmission generation do, and those
  // are covered here and by fordSuperDuty67_10R140 below.
  //
  // ⚠️ vin[3] is NOT the series either. The same sweep method found 7, 8, B and
  // R all decode as F-250. Two dead helpers (fordF250/fordF350) encoding that
  // wrong rule were removed on 2026-08-13; do not reintroduce them.
  //
  // F-600 (vin[5]='6', a Class-6 chassis cab that also takes the 6.7L) is
  // deliberately EXCLUDED: it is not in the identification table's
  // FORD_SD_SERIES either, and nothing of that class has been scanned. Fails
  // closed, per the doctrine above.
  return vinIs("2345", vinAt(vin,5)) &&
         vinAt(vin,7) == 'T';
}

// The PROFILE gate, distinct from the identity gate above and deliberately
// narrower.
//
// fordSuperDuty67 needs no model-year rule because the vPIC frame identifies a
// 6.7L Super Duty at every year code -- true, and fine for putting a NAME on
// the splash. It is NOT fine for selecting this profile, because the profile
// reads 10R140 transmission DIDs and the transmission generation break is
// 2019->2020 (6R140 -> 10R140). Without this gate, profileKeyFor() is reached
// even for VINs the identification table's own year list rejects, so a 2010
// truck would have been handed a ten-speed gear decode for its six-speed.
//
// L..T = 2020-2026: from the 10R140's introduction to the end of the span the
// identity table verifies. Later years fail CLOSED, per the doctrine above --
// a 2027 truck gets Standard+ until someone confirms the profile still fits.
static bool fordSuperDuty67_10R140(const char* vin) {
  return fordSuperDuty67(vin) && vinModelYearIn(vin, "LMNPRST");
}

// ---------------------------------------------------------------------------
// PROFILE lookup: which gauge profile, if any, this VIN gets. Unchanged in
// spirit from the original table -- a strict, fail-closed match, because
// handing the wrong profile to a truck shows wrong numbers on real gauges.
// ---------------------------------------------------------------------------
static const char* profileKeyFor(const char* vin) {
  if (!vin || std::strlen(vin) < 3) return nullptr;
  char w[4] = { (char)std::toupper((unsigned char)vin[0]),
                (char)std::toupper((unsigned char)vin[1]),
                (char)std::toupper((unsigned char)vin[2]), '\0' };
  struct M { const char* wmi; bool (*extra)(const char* vin); const char* key; };
  static const M MAP[] = {
    {"1GT",gmSierra1500Diesel,"gm_sierra_lz0"},{"3GT",gmSierra1500Diesel,"gm_sierra_lz0"},
    {"1GC",gmSierra1500Diesel,"gm_sierra_lz0"},{"3GC",gmSierra1500Diesel,"gm_sierra_lz0"},
    // BMW. The predicate is the F10 535i frame, so the other WMIs (M cars, the
    // X-series SAVs) fail closed rather than being handed an N55 sedan profile.
    {"WBA",bmwF10535i,"bmw_f10_535i"}, {"WBS",bmwF10535i,"bmw_f10_535i"},
    {"5UX",bmwF10535i,"bmw_f10_535i"}, {"4US",bmwF10535i,"bmw_f10_535i"},
    {"WAU",audiQ5_20T,"audi_q5"}, {"WA1",audiQ5_20T,"audi_q5"},
    {"WUA",audiQ5_20T,"audi_q5"}, {"TRU",audiQ5_20T,"audi_q5"},
    // Stellantis — see jeepWagoneer57() for why the WMI alone was dangerous here.
    // 1C4 ONLY. 1J4 and 3C4 were here and are removed: vPIC resolves 1J4 at the
    // Wagoneer-era year codes to a 1992-96 Cherokee (year codes repeat every 30
    // years, so the gate above cannot separate them), and 3C4 produces no
    // vehicle at all at those codes. Both offered false positives and matched
    // nothing real.
    {"1C4",jeepWagoneer57,"jeep_ws"},
    // FORD SUPER DUTY 6.7L POWER STROKE — SCANNED 2026-08-09, now profiled.
    // fordSuperDuty67 pins the PLATFORM and ENGINE (vin[4]='W', vin[6]='B',
    // vin[7]='T') and is deliberately silent on the series digit, so this one
    // row covers F-250 through F-550 — every Super Duty carrying the 6.7L and
    // the 10R140 the enhanced DIDs below belong to. A gas Super Duty fails on
    // vin[7] and falls through to Standard+ Gas.
    //
    // Scanned on a 2021 F-350. The _10R140 suffix is load-bearing: MY2019 and
    // earlier are the 6R140, so the gear DID this profile decodes as ten
    // positions would be wrong there. See the predicate for why the identity
    // gate alone is not enough.
    {"1FT",fordSuperDuty67_10R140,"ford_sd_67"},
    {"3FT",fordSuperDuty67_10R140,"ford_sd_67"},
    // 1FD is the same truck as its 1FT twin -- the WMI differs by body/plant,
    // not by drivetrain, and a 1FT F-450/F-550 already selects this profile.
    // Leaving 1FD out would deny the profile on a WMI difference alone.
    {"1FD",fordSuperDuty67_10R140,"ford_sd_67"},
                  };
  // First matching row wins. Two rows may share a WMI (the F-250 and F-350
  // predicates both live under 1FT and differ only at vin[3]), so the predicate
  // — not the WMI — decides, and a row whose predicate fails must fall through
  // to the next rather than terminating the search.
  for (const M& m : MAP) {
    if (std::strcmp(w, m.wmi) != 0) continue;
    if (m.extra && !m.extra(vin)) continue;
    return m.key;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// IDENTIFICATION layer: what the truck IS, which is a broader question than
// which profile it gets. Naming a truck wrongly costs a wrong caption; giving
// it the wrong profile costs wrong gauge readings. So this table is allowed to
// cover vehicles the profile table refuses.
//
// Structure, derived from NHTSA vPIC by decoding partial VINs (see
// docs/VEHICLES.md): every make encodes its model line and its engine at
// DIFFERENT VIN positions, so they get separate lookups rather than one row per
// (model, engine) pair -- which would have meant ~40 rows of duplicated engine
// strings.
//
// Each line is bounded to the model years vPIC actually confirmed. Outside that
// span we return nothing rather than assume the pattern held: positional rules
// are not unique across 20 years (the GM rule once also matched an Express van,
// and Ford reused engine code 'T' for both a 6.7L Power Stroke and a 3.5L
// EcoBoost on different lines).
// ---------------------------------------------------------------------------
struct SeriesRow { const char* codes; const char* name; };
// `diesel` picks the Standard+ layout: EGT and NOx tiles only make sense on a
// compression-ignition engine, and a gas truck would show them blank forever.
struct EngineRow { char code; const char* engine; bool diesel; };

static const SeriesRow FORD_SD_SERIES[] = {
  {"2","Ford F-250"}, {"3","Ford F-350"}, {"4","Ford F-450"}, {"5","Ford F-550"},
};
// Engine codes are per LINE, not per make: 'T' is a 6.7L Power Stroke on a
// Super Duty and a 3.5L EcoBoost on an F-150.
static const EngineRow FORD_SD_ENGINES[] = {
  {'T',"6.7L Power Stroke",true}, {'6',"6.2L V8",false}, {'N',"7.3L V8",false},
};

// Pre-2010 Super Duty. SAME series position (vin[5]) and same series codes as
// the modern truck, but a COMPLETELY DIFFERENT engine alphabet -- which is why
// this needs its own table rather than another entry in the one above.
//
// Verified against vPIC 2026-08-13, one probe per model year rather than one
// probe generalised across the span:
//   P = 6.0L Power Stroke  confirmed 2003, 2004, 2005, 2006, 2007
//   R = 6.4L Power Stroke  confirmed 2008, 2009
//   5 = 5.4L Triton V8 (gas)      Y = 6.8L V10 (gas)
//
// 2010 (year code 'A') returned NO Model for either diesel code, so the span
// stops at 2009. The 6.4L was built into 2010, so this is a gap in vPIC's data
// or a VDS change, NOT evidence that no 2010 truck exists -- it is simply
// unverified, and the doctrine here is to gate to what was actually confirmed.
static const EngineRow FORD_SD_ENGINES_PRE2010[] = {
  {'P',"6.0L Power Stroke",true}, {'R',"6.4L Power Stroke",true},
  {'5',"5.4L V8",false}, {'Y',"6.8L V10",false},
};
static const SeriesRow FORD_F150_SERIES[] = { {"1","Ford F-150"} };

// Ram 1500 uses a SET of series codes, not one -- vPIC decodes 6,7,B,E and F
// all as 1500.
static const SeriesRow RAM_SERIES[] = {
  {"67BEF","Ram 1500"}, {"45","Ram 2500"}, {"23","Ram 3500"},
};
static const EngineRow RAM_ENGINES[] = {
  {'T',"5.7L HEMI V8",false}, {'L',"6.7L Cummins I6",true}, {'G',"3.6L V6",false},
  {'J',"6.4L HEMI V8",false}, {'9',"6.2L HEMI V8",false}, {'M',"3.0L EcoDiesel V6",true},
};

// --- Pre-2013 DODGE Ram (the Ram brand split off for MY2013) ---------------
//
// Different WMI (1D7/3D7, not 1C6/3C6 — those decode to NOTHING before 2013)
// and a different code alphabet, but by luck the SAME position: tonnage is
// vin[5], exactly where the modern table reads its series.
//
// ⚠️ THE CODE SETS COLLIDE, WHICH IS WHY THE YEAR GATE IS LOAD-BEARING.
// Modern vin[5]='2' or '3' means 3500; here '2' means 2500 and '3' means 3500.
// A pre-2013 truck read by the modern table would be named one size too big.
// The two are kept apart by year code alone: modern rows take D-R (2013-2024),
// these take 6-B (2006-2011). Never merge them.
//
// Verified against vPIC 2026-08-16, per model year, requiring Model=="Ram" AND
// a Series carrying the tonnage — vPIC happily returns one without the other on
// an under-specified partial VIN, and treating that as data is how a wrong rule
// ships. The digit is perfectly consistent everywhere it verified:
//   vin[5] 1 -> 1500   2 -> 2500   3 or 4 -> 3500
//
// 3500 SPLIT OUT: it only verified for MY2006-2007. From 2008 the 3500 rows
// come back with a blank or "Ram Chassis Cab" Model, which fails the agreement
// check, so those years get a table WITHOUT 3500 and such a truck falls through
// unidentified. Fail closed, the same call MY2010 GMC got in the GMT900 work.
static const SeriesRow RAM_PRE2013_SERIES[] = {
  {"1","Ram 1500"}, {"2","Ram 2500"}, {"34","Ram 3500"},
};
static const SeriesRow RAM_PRE2013_SERIES_NO35[] = {
  {"1","Ram 1500"}, {"2","Ram 2500"},
};

// vin[4] is the model line, and ITS MEANING MOVES YEAR TO YEAR — this is the
// whole reason a single pattern could not be shipped for the span:
//
//   2006-07  A R S U      2008-09  A B R S U V      2010  B P T V   2011  6 B P T V
//
// In 2006-07 the same four codes serve 1500, 2500 AND 3500, so vin[4] carries
// no tonnage at all there; by 2010 it does. A rule derived from the 2010 seed
// alone matched only 26 of 60 line/tonnage pairs across the span.
//
// It is an ALLOWLIST, not a Dakota denylist, because 1D7 is shared with several
// non-pickups whose codes collide across years: Dakota E/W, Grand Caravan N,
// Journey 5/G/H, Nitro 9/U. 'U' is a Ram in 2006-2009 and a Nitro in 2011;
// 'G'/'H' are Ram 3500 in 2007-2010 and a Journey in 2011. Only an
// admit-what-was-verified list survives that. '5' is excluded from 2011: it
// decoded as BOTH a Ram 1500 and a Journey, so it is contradictory, not data.
static bool ramPre2013(const char* vin) {
  switch (vinAt(vin, 9)) {                       // model-year code
    case '6': case '7': return vinIs("ARSU",   vinAt(vin, 4));   // 2006-2007
    case '8': case '9': return vinIs("ABRSUV", vinAt(vin, 4));   // 2008-2009
    case 'A':           return vinIs("BPTV",   vinAt(vin, 4));   // 2010
    case 'B':           return vinIs("6BPTV",  vinAt(vin, 4));   // 2011
  }
  return false;
}
static const EngineRow GM_LD_ENGINES[] = {
  {'8',"3.0L Duramax I6",true}, {'D',"5.3L V8",false}, {'K',"2.7L I4 Turbo",false}, {'L',"6.2L V8",false},
};
// T1XX-era light duty (2019-2021). A different engine alphabet from the 2022+
// table below: the 3.0L Duramax is 'T' (LM2) here and '8' (LZ0) there, and this
// era also offers a 4.3L V6 and a second 5.3L code. Verified per model year on
// 2019, 2020 and 2021 -- all six codes present and identical in all three.
// K2XX-era pickups (2014-2018). TONNAGE IS vin[5] HERE TOO, but the alphabet is
// per-MAKE and it CHANGES MID-GENERATION -- two facts that make a single shared
// table actively dangerous:
//
//   GMC    MY2014-15   1500=TUVW    2500=04XYZ   3500=123
//   GMC    MY2016-18   1500=LMNP    2500=RSTU    3500=VWXY
//   Chevy  MY2014-18   1500=NPRST   2500=UVWX    3500=01YZ
//
// GMC 'T'/'U' therefore means 1500 in 2015 and 2500 in 2016 -- adjacent model
// years, opposite meanings -- and Chevy 'U'/'V'/'W' means 2500 while GMC
// 'T'/'U'/'V'/'W' means 1500 in the same era. Verified on THREE independent
// seeds (different vin[3], vin[4], vin[6] and vin[7]) across MY2014-2018, so
// the mid-generation flip is real and not a seed artefact.
//
// NO ENGINE TABLE, on purpose -- the same reasoning as the F-150 rows below.
// vPIC returns the manufacturer's whole engine list for these VINs rather than
// what a given tonnage could be ordered with (it offers the 6.6L Duramax on a
// 1500, which never existed), so the code-to-engine mapping cannot be trusted
// per row. Name the truck, say nothing about what is under the hood.
static const SeriesRow GM_SIERRA_K2XX_EARLY[] = {
  {"TUVW","GMC Sierra 1500"}, {"04XYZ","GMC Sierra 2500"}, {"123","GMC Sierra 3500"},
};
static const SeriesRow GM_SIERRA_K2XX_LATE[] = {
  {"LMNP","GMC Sierra 1500"}, {"RSTU","GMC Sierra 2500"}, {"VWXY","GMC Sierra 3500"},
};
static const SeriesRow GM_SILVERADO_K2XX[] = {
  {"NPRST","Chevrolet Silverado 1500"}, {"UVWX","Chevrolet Silverado 2500"},
  {"01YZ","Chevrolet Silverado 3500"},
};

// GMT900-era pickups. A THIRD tonnage alphabet, distinct from both K2XX tables
// above, and it collides with them:
//
//   GMC    MY2011-13   1500=TUVWXY  2500=015Z    3500=2346
//   Chevy  MY2010-13   1500=PRSTU   2500=VXY     3500=01Z
//
// Chevy 'U' is a 1500 here and a 2500 in K2XX (2014-18); 'Y' is a 2500 here and
// a 3500 there. GMC 'X'/'Y' are 1500 here and 2500 in early K2XX. So the
// platform boundary flips meanings the same way the mid-K2XX boundary does.
//
// Verified per model year: Chevy on 2010, 2011, 2012, 2013; GMC on 2011, 2012,
// 2013. GMC MY2010 did not decode from the seeds tried and is NOT claimed --
// the row starts at 2011 and fails closed below that.
//
// MY2007-2009 did not decode at all for either make. That is another VDS layout
// (or a vPIC data gap) and is left for a separate pass rather than guessed at.
//
// vPIC reports the 2010 Series as "1/2 Ton" / "3/4 ton" / "1 ton" and switches
// to "1500"/"2500"/"3500" from 2011. Same trucks; this table normalises to the
// modern names so the splash does not change vocabulary by model year.
//
// No engine table, for the same reason as the K2XX rows.
static const SeriesRow GM_SIERRA_GMT900[] = {
  {"TUVWXY","GMC Sierra 1500"}, {"015Z","GMC Sierra 2500"}, {"2346","GMC Sierra 3500"},
};
static const SeriesRow GM_SILVERADO_GMT900[] = {
  {"PRSTU","Chevrolet Silverado 1500"}, {"VXY","Chevrolet Silverado 2500"},
  {"01Z","Chevrolet Silverado 3500"},
};

static const EngineRow GM_LD_ENGINES_T1XX[] = {
  {'T',"3.0L Duramax I6",true}, {'D',"5.3L V8",false}, {'F',"5.3L V8",false},
  {'H',"4.3L V6",false}, {'K',"2.7L I4 Turbo",false}, {'L',"6.2L V8",false},
};
static const EngineRow GM_HD_ENGINES[] = {
  {'Y',"6.6L Duramax V8",true}, {'7',"6.6L V8",false},
};

// Brand/platform gates. These sit at the LINE level so the series and engine
// lookups below can stay dumb table scans.
static bool ramBrand(const char* vin)  { return vinAt(vin,4) == 'R'; }   // 1C6 is shared with Jeep
static bool gmLightDuty(const char* vin) {
  return vinIs("NPRUV", vinAt(vin,3)) && vinIs("HU", vinAt(vin,4));
}
// GM TONNAGE lives at vin[5], and this is load-bearing.
//
// ⚠️ vin[3]+vin[4] DO NOT separate a 1500 from an HD. In the T1XX era
// (2019-2021) the light-duty 1500 sits in the SAME vin[3]/vin[4] space as the
// HD, so the old guard -- vin[3] in 0-5 and vin[4] in 8/9, with no vin[5] check
// -- matched 1500s and named them "Sierra HD". Confirmed against the shipped
// firmware: 1GT09CED5LZ345678 is a 2020 Sierra 1500 5.3L per vPIC and the dash
// called it a Sierra HD with a blank engine.
//
// vin[5] sweeps clean and identically on MY2020, 2021, 2022, 2023 and 2024:
//   ABCDEFG = 1500      LMNPR = 2500      STUVW = 3500
// (MY2024 additionally showed X/Z as 2500 and Y as 3500. Seen on one year only,
// so they are NOT admitted here -- a 2024 truck with those codes fails closed,
// which is the doctrine everywhere else in this file.)
static const char* const GM_TONNAGE_HD = "LMNPRSTUVW";
static const char* const GM_TONNAGE_LD = "ABCDEFG";

static bool gmHeavyDuty(const char* vin) {
  return vinIs("012345", vinAt(vin,3)) && vinIs("89", vinAt(vin,4)) &&
         vinIs(GM_TONNAGE_HD, vinAt(vin,5));
}

// K2XX pickup gates. vin[4] separates the pickup from the vans and the
// medium/heavy trucks that share these WMIs (GMC: 5/6 = Canyon, 4/6/7/8/9 =
// medium-duty; Chevy likewise). vin[3] is NOT gated -- it carries cab/drive.
static bool gmPickupSierraK2XX(const char* vin)    { return vinIs("12", vinAt(vin,4)); }
static bool gmPickupSilveradoK2XX(const char* vin) { return vinIs("CK", vinAt(vin,4)); }

// T1XX light duty (2019-2021). Deliberately does NOT gate vin[3]: the sweep
// showed eleven values valid there and it carries cab/drive, not tonnage --
// gating a position that does not discriminate is the bug that shipped twice on
// the Ford rows. vin[4] in 8/9 is what separates the pickup from a Canyon (5/6)
// and a Savana (7); vin[5] is what separates it from the HD.
static bool gmLightDutyT1XX(const char* vin) {
  return vinIs("89", vinAt(vin,4)) && vinIs(GM_TONNAGE_LD, vinAt(vin,5));
}

struct LineRow {
  const char* wmi;
  const char* yearCodes;          // vin[9], the VERIFIED span only
  bool (*guard)(const char* vin); // brand/platform gate, or nullptr
  int seriesIdx;                  // VIN index holding the series code; -1 = fixed name
  const SeriesRow* series; int nSeries;
  const char* fixedName;          // used when seriesIdx < 0
  const EngineRow* engines; int nEngines;
};

#define ARR(x) x, (int)(sizeof(x)/sizeof((x)[0]))

static const LineRow LINES[] = {
  // Ford Super Duty -- verified 2011-2026.
  //
  // 1FD is the F-450/F-550 (and F-350 chassis-cab) WMI and decodes identically:
  // same series position, same series codes, same engine codes. It was missing
  // until 2026-08-13, so a 1FD truck was not identified at all -- confirmed
  // against vPIC, where 1FD8W4BT/1FD8W5BT decode as a 2022 F-450/F-550 6.7L
  // exactly as their 1FT twins do. There is no 3FD: it decodes to nothing.
  {"1FT","BCDEFGHJKLMNPRST",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES)},
  {"3FT","BCDEFGHJKLMNPRST",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES)},
  {"1FD","BCDEFGHJKLMNPRST",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES)},
  // Ford Super Duty, PREVIOUS generation -- verified 2003-2009 (VEH-12).
  //
  // Year codes cannot collide with the row above: 2001-2009 are the digits 1-9
  // and 2010-2039 are letters, so a digit at vin[9] is unambiguously this era.
  // These identify only -- they get Standard+, never ford_sd_67, because that
  // profile decodes 10R140 gear positions and reads DIDs confirmed on a 6.7L.
  {"1FT","3456789",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES_PRE2010)},
  {"3FT","3456789",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES_PRE2010)},
  {"1FD","3456789",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES_PRE2010)},
  // Ford F-150 -- verified 2010-2023. No engine table on purpose: the codes are
  // year-dependent on this line and only sparsely confirmed, so we name the
  // truck and say nothing about what is under the hood.
  {"1FT","ABCDEFGHJKLMNP",nullptr,5,ARR(FORD_F150_SERIES),nullptr,nullptr,0},
  {"3FT","ABCDEFGHJKLMNP",nullptr,5,ARR(FORD_F150_SERIES),nullptr,nullptr,0},
  // Ram -- verified 2013-2024.
  {"1C6","DEFGHJKLMNPR",ramBrand,5,ARR(RAM_SERIES),nullptr,ARR(RAM_ENGINES)},
  {"3C6","DEFGHJKLMNPR",ramBrand,5,ARR(RAM_SERIES),nullptr,ARR(RAM_ENGINES)},
  // Pre-2013 Dodge Ram. 1D7 and 3D7 were swept independently and behave
  // IDENTICALLY, so they share tables. NO ENGINE TABLE ON PURPOSE: sweeping
  // vin[7] returns Chrysler's whole make-wide alphabet regardless of line or
  // tonnage -- the identical 18-entry list came back for 1500, 2500 and 3500,
  // including a 1.8L four and an 8.4L V10. That is vPIC decoding an
  // under-specified VIN against a global table, not the engines a Ram could
  // have. Same call, same reason, as the F-150 rows above: name the truck and
  // say nothing about what is under the hood.
  // MY2012 is ABSENT deliberately -- vPIC answers ErrorCode 8 ("No detailed
  // data available currently") for every 2012 combination on every candidate
  // WMI, so the year is a data gap, not a pattern we failed to find.
  {"1D7","67",ramPre2013,5,ARR(RAM_PRE2013_SERIES),nullptr,nullptr,0},
  {"3D7","67",ramPre2013,5,ARR(RAM_PRE2013_SERIES),nullptr,nullptr,0},
  {"1D7","89AB",ramPre2013,5,ARR(RAM_PRE2013_SERIES_NO35),nullptr,nullptr,0},
  {"3D7","89AB",ramPre2013,5,ARR(RAM_PRE2013_SERIES_NO35),nullptr,nullptr,0},
  // GM light duty -- verified 2022-2026.
  {"1GT","NPRST",gmLightDuty,-1,nullptr,0,"GMC Sierra 1500",ARR(GM_LD_ENGINES)},
  {"3GT","NPRST",gmLightDuty,-1,nullptr,0,"GMC Sierra 1500",ARR(GM_LD_ENGINES)},
  {"1GC","NPRST",gmLightDuty,-1,nullptr,0,"Chevrolet Silverado 1500",ARR(GM_LD_ENGINES)},
  {"3GC","NPRST",gmLightDuty,-1,nullptr,0,"Chevrolet Silverado 1500",ARR(GM_LD_ENGINES)},
  // GM GMT900 pickups -- Chevy verified 2010-2013, GMC verified 2011-2013.
  // Year codes A-D cannot collide with K2XX (EFGHJ), T1XX (KLM), HD (LMNPR) or
  // the 2022+ rows (NPRST). The vin[4] pickup gates are the same ones the K2XX
  // rows use -- that part of the scheme did not change across the boundary.
  {"1GC","ABCD",gmPickupSilveradoK2XX,5,ARR(GM_SILVERADO_GMT900),nullptr,nullptr,0},
  {"3GC","ABCD",gmPickupSilveradoK2XX,5,ARR(GM_SILVERADO_GMT900),nullptr,nullptr,0},
  {"1GT","BCD",gmPickupSierraK2XX,5,ARR(GM_SIERRA_GMT900),nullptr,nullptr,0},
  {"3GT","BCD",gmPickupSierraK2XX,5,ARR(GM_SIERRA_GMT900),nullptr,nullptr,0},
  // GM K2XX pickups -- verified 2014-2018 (VEH-12). Split into two GMC rows
  // because the tonnage alphabet changes between MY2015 and MY2016; see the
  // tables above. Year codes E-J cannot collide with the T1XX (KLM), HD
  // (LMNPR) or 2022+ (NPRST) rows.
  {"1GT","EF",gmPickupSierraK2XX,5,ARR(GM_SIERRA_K2XX_EARLY),nullptr,nullptr,0},
  {"3GT","EF",gmPickupSierraK2XX,5,ARR(GM_SIERRA_K2XX_EARLY),nullptr,nullptr,0},
  {"1GT","GHJ",gmPickupSierraK2XX,5,ARR(GM_SIERRA_K2XX_LATE),nullptr,nullptr,0},
  {"3GT","GHJ",gmPickupSierraK2XX,5,ARR(GM_SIERRA_K2XX_LATE),nullptr,nullptr,0},
  {"1GC","EFGHJ",gmPickupSilveradoK2XX,5,ARR(GM_SILVERADO_K2XX),nullptr,nullptr,0},
  {"3GC","EFGHJ",gmPickupSilveradoK2XX,5,ARR(GM_SILVERADO_K2XX),nullptr,nullptr,0},
  // GM light duty, T1XX generation -- verified 2019-2021 (VEH-12).
  //
  // These trucks were previously identified as "Sierra HD" / "Silverado HD",
  // because the HD guard did not look at tonnage. Naming them correctly is the
  // point of these rows; the tonnage split is done by gmLightDutyT1XX.
  //
  // The 3.0L Duramax here is the LM2, NOT the LZ0 the gm_sierra_lz0 profile was
  // scanned on, so these identify only -- the profile gate's own year rule
  // (2023-2026) already excludes them, and that is left alone deliberately.
  {"1GT","KLM",gmLightDutyT1XX,-1,nullptr,0,"GMC Sierra 1500",ARR(GM_LD_ENGINES_T1XX)},
  {"3GT","KLM",gmLightDutyT1XX,-1,nullptr,0,"GMC Sierra 1500",ARR(GM_LD_ENGINES_T1XX)},
  {"1GC","KLM",gmLightDutyT1XX,-1,nullptr,0,"Chevrolet Silverado 1500",ARR(GM_LD_ENGINES_T1XX)},
  {"3GC","KLM",gmLightDutyT1XX,-1,nullptr,0,"Chevrolet Silverado 1500",ARR(GM_LD_ENGINES_T1XX)},
  // GM heavy duty -- verified 2020-2024.
  {"1GT","LMNPR",gmHeavyDuty,-1,nullptr,0,"GMC Sierra HD",ARR(GM_HD_ENGINES)},
  {"3GT","LMNPR",gmHeavyDuty,-1,nullptr,0,"GMC Sierra HD",ARR(GM_HD_ENGINES)},
  {"1GC","LMNPR",gmHeavyDuty,-1,nullptr,0,"Chevrolet Silverado HD",ARR(GM_HD_ENGINES)},
  {"3GC","LMNPR",gmHeavyDuty,-1,nullptr,0,"Chevrolet Silverado HD",ARR(GM_HD_ENGINES)},
};

#undef ARR

bool vinIdentify(const char* vin, VinIdentity* out) {
  if (!vin || !out || std::strlen(vin) < 10) return false;
  char w[4] = { (char)std::toupper((unsigned char)vin[0]),
                (char)std::toupper((unsigned char)vin[1]),
                (char)std::toupper((unsigned char)vin[2]), '\0' };

  for (const LineRow& L : LINES) {
    if (std::strcmp(w, L.wmi) != 0) continue;
    if (!vinIs(L.yearCodes, vinAt(vin,9))) continue;   // outside the verified span
    if (L.guard && !L.guard(vin)) continue;

    const char* name = L.fixedName;
    if (L.seriesIdx >= 0) {
      name = nullptr;
      const char code = vinAt(vin, L.seriesIdx);
      for (int i = 0; i < L.nSeries; ++i)
        if (vinIs(L.series[i].codes, code)) { name = L.series[i].name; break; }
      if (!name) continue;        // a series we do not recognise: try the next line
    }

    // Engine is optional. An unrecognised code is "" (say nothing) rather than
    // a guess -- a wrong engine on the splash is worse than a missing one.
    const char* engine = "";
    bool diesel = false;
    for (int i = 0; i < L.nEngines; ++i)
      if (L.engines[i].code == vinAt(vin,7)) {
        engine = L.engines[i].engine; diesel = L.engines[i].diesel; break;
      }

    // A SCANNED profile always wins: it reads real enhanced parameters, and
    // dropping such a truck to the legislated set would lose data we measured.
    // Everything else gets Standard+ rather than bare Generic -- same legislated
    // PIDs either way, but laid out for the engine we know it has. An engine we
    // could not name falls back to the gas layout, because diesel-only tiles on
    // a gas truck look broken while the reverse merely omits data.
    const char* scanned = profileKeyFor(vin);
    out->profileKey = scanned ? scanned : (diesel ? "std_diesel" : "std_gas");
    out->name = name;
    out->engine = engine;
    return true;
  }

  // Not in the identification table, but the profile table may still know it
  // (BMW, Audi, Jeep). Those carry no display name -- their profile supplies
  // one -- so name and engine are empty and only the key is meaningful.
  if (const char* key = profileKeyFor(vin)) {
    out->profileKey = key; out->name = ""; out->engine = "";
    return true;
  }
  return false;
}

const char* vinToProfileKey(const char* vin) {
  // Goes through vinIdentify() rather than profileKeyFor() directly, because a
  // recognized-but-unscanned truck resolves to Standard+ and that decision
  // needs the engine code the identity lookup reads.
  VinIdentity id{};
  return vinIdentify(vin, &id) ? id.profileKey : nullptr;
}

bool vinDisplayIdentity(const char* vin, VinIdentity* out) {
  VinIdentity id{};
  if (!vinIdentify(vin, &id)) return false;
  // Standard+ is a LEGISLATED-PID profile, not a vehicle profile: its label is
  // "Standard+ Diesel", so it cannot name the truck and the splash still needs
  // the stored identity. Only a scanned, vehicle-specific profile carries a
  // name of its own.
  if (id.profileKey && std::strncmp(id.profileKey, "std_", 4) != 0) return false;
  if (!id.name || !id.name[0]) return false;   // identified but nothing worth showing
  *out = id;
  return true;
}

const char* vinAutoTarget(const char* vin, bool vehicleAuto, const char* currentKey) {
  if (!vehicleAuto) return nullptr;
  if (!vin || !vin[0]) return nullptr;
  const char* key = vinToProfileKey(vin);
  if (!key) return nullptr;
  if (!profileForKey(key)) return nullptr;                                   // must exist in registry
  if (currentKey && std::strcmp(key, currentKey) == 0) return nullptr;       // no change
  return key;
}
