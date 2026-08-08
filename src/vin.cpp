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
      int idx = 0;
      for (size_t k = 0; k < colon; k++) {
        int hv = hexVal(t[k]);
        if (hv < 0) return false;                    // non-hex frame index
        idx = (idx << 4) | hv;
      }
      if ((idx & 0x0F) != (expectFrame & 0x0F)) return false;  // out-of-order/dup/missing
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
// Sierra/Silverado HD sits on a different schema ([NPRUV][89]*E), so vin[4]
// is what separates 1500 from HD.
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
// We have NO profile for it (docs/FORD-STATUS.md: researched, needs a scan), so
// it identifies only. That is the point of VinIdentity.
static bool fordSuperDuty67(const char* vin) {
  if (std::strlen(vin) < 8) return false;
  // Deliberately silent on vin[3] (the series digit): fordF250/fordF350 each
  // pin it exactly, so re-checking it here is dead code -- and worse, it MASKED
  // a mutation, letting fordF350 drop its own vin[3] gate with every test still
  // green. Platform + engine only; series belongs to the callers.
  return vinAt(vin,4) == 'W' &&
         vinAt(vin,6) == 'B' &&
         vinAt(vin,7) == 'T';
}
static bool fordF250(const char* vin) { return fordSuperDuty67(vin) && vinAt(vin,3) == '7'; }
static bool fordF350(const char* vin) { return fordSuperDuty67(vin) && vinAt(vin,3) == '8'; }

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
    // FORD SUPER DUTY 6.7L POWER STROKE — IDENTIFIED, NOT PROFILED.
    // key is nullptr on purpose: docs/FORD-STATUS.md has the research but no
    // scan, so the dash stays on Generic and simply says what the truck is
    // instead of pretending it does not know. Fill in the key when a profile
    // lands; nothing else here changes.
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
struct EngineRow { char code; const char* engine; };

static const SeriesRow FORD_SD_SERIES[] = {
  {"2","Ford F-250"}, {"3","Ford F-350"}, {"4","Ford F-450"}, {"5","Ford F-550"},
};
// Engine codes are per LINE, not per make: 'T' is a 6.7L Power Stroke on a
// Super Duty and a 3.5L EcoBoost on an F-150.
static const EngineRow FORD_SD_ENGINES[] = {
  {'T',"6.7L Power Stroke"}, {'6',"6.2L V8"}, {'N',"7.3L V8"},
};
static const SeriesRow FORD_F150_SERIES[] = { {"1","Ford F-150"} };

// Ram 1500 uses a SET of series codes, not one -- vPIC decodes 6,7,B,E and F
// all as 1500.
static const SeriesRow RAM_SERIES[] = {
  {"67BEF","Ram 1500"}, {"45","Ram 2500"}, {"23","Ram 3500"},
};
static const EngineRow RAM_ENGINES[] = {
  {'T',"5.7L HEMI V8"}, {'L',"6.7L Cummins I6"}, {'G',"3.6L V6"},
  {'J',"6.4L HEMI V8"}, {'9',"6.2L HEMI V8"},   {'M',"3.0L EcoDiesel V6"},
};
static const EngineRow GM_LD_ENGINES[] = {
  {'8',"3.0L Duramax I6"}, {'D',"5.3L V8"}, {'K',"2.7L I4 Turbo"}, {'L',"6.2L V8"},
};
static const EngineRow GM_HD_ENGINES[] = {
  {'Y',"6.6L Duramax V8"}, {'7',"6.6L V8"},
};

// Brand/platform gates. These sit at the LINE level so the series and engine
// lookups below can stay dumb table scans.
static bool ramBrand(const char* vin)  { return vinAt(vin,4) == 'R'; }   // 1C6 is shared with Jeep
static bool gmLightDuty(const char* vin) {
  return vinIs("NPRUV", vinAt(vin,3)) && vinIs("HU", vinAt(vin,4));
}
static bool gmHeavyDuty(const char* vin) {
  // vin[4] 8/9 and vin[3] 0-5 mark the HD chassis. The 2500-vs-3500 split is a
  // JOINT function of vin[4]+vin[5] that vPIC only resolves for 16 of 1089
  // combinations, so we name the line and stop rather than guess the tonnage.
  return vinIs("012345", vinAt(vin,3)) && vinIs("89", vinAt(vin,4));
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
  {"1FT","BCDEFGHJKLMNPRST",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES)},
  {"3FT","BCDEFGHJKLMNPRST",nullptr,5,ARR(FORD_SD_SERIES),nullptr,ARR(FORD_SD_ENGINES)},
  // Ford F-150 -- verified 2010-2023. No engine table on purpose: the codes are
  // year-dependent on this line and only sparsely confirmed, so we name the
  // truck and say nothing about what is under the hood.
  {"1FT","ABCDEFGHJKLMNP",nullptr,5,ARR(FORD_F150_SERIES),nullptr,nullptr,0},
  {"3FT","ABCDEFGHJKLMNP",nullptr,5,ARR(FORD_F150_SERIES),nullptr,nullptr,0},
  // Ram -- verified 2013-2024.
  {"1C6","DEFGHJKLMNPR",ramBrand,5,ARR(RAM_SERIES),nullptr,ARR(RAM_ENGINES)},
  {"3C6","DEFGHJKLMNPR",ramBrand,5,ARR(RAM_SERIES),nullptr,ARR(RAM_ENGINES)},
  // GM light duty -- verified 2022-2026.
  {"1GT","NPRST",gmLightDuty,-1,nullptr,0,"GMC Sierra 1500",ARR(GM_LD_ENGINES)},
  {"3GT","NPRST",gmLightDuty,-1,nullptr,0,"GMC Sierra 1500",ARR(GM_LD_ENGINES)},
  {"1GC","NPRST",gmLightDuty,-1,nullptr,0,"Chevrolet Silverado 1500",ARR(GM_LD_ENGINES)},
  {"3GC","NPRST",gmLightDuty,-1,nullptr,0,"Chevrolet Silverado 1500",ARR(GM_LD_ENGINES)},
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
    for (int i = 0; i < L.nEngines; ++i)
      if (L.engines[i].code == vinAt(vin,7)) { engine = L.engines[i].engine; break; }

    out->profileKey = profileKeyFor(vin);
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
  return profileKeyFor(vin);
}

bool vinDisplayIdentity(const char* vin, VinIdentity* out) {
  VinIdentity id{};
  if (!vinIdentify(vin, &id)) return false;
  if (id.profileKey) return false;             // profiled -> its profile carries the name
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
