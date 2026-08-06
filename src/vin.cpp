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

const char* vinToProfileKey(const char* vin) {
  if (!vin || std::strlen(vin) < 3) return nullptr;
  char w[4] = { (char)std::toupper((unsigned char)vin[0]),
                (char)std::toupper((unsigned char)vin[1]),
                (char)std::toupper((unsigned char)vin[2]), '\0' };
  // `extra` is optional: nullptr means the row matches on WMI alone. The
  // BMW/Audi/Jeep rows are still WMI-only and can be tightened the same way when
  // someone works out their discriminators.
  struct M { const char* wmi; const char* key; bool (*extra)(const char* vin); };
  static const M MAP[] = {
    {"1GT","gm_sierra_lz0",gmSierra1500Diesel},{"3GT","gm_sierra_lz0",gmSierra1500Diesel},
    {"1GC","gm_sierra_lz0",gmSierra1500Diesel},{"3GC","gm_sierra_lz0",gmSierra1500Diesel},
    // BMW. The predicate is the F10 535i frame, so the other WMIs (M cars, the
    // X-series SAVs) fail closed rather than being handed an N55 sedan profile.
    {"WBA","bmw_f10_535i",bmwF10535i}, {"WBS","bmw_f10_535i",bmwF10535i},
    {"5UX","bmw_f10_535i",bmwF10535i}, {"4US","bmw_f10_535i",bmwF10535i},
    {"WAU","audi_q5",audiQ5_20T}, {"WA1","audi_q5",audiQ5_20T},
    {"WUA","audi_q5",audiQ5_20T}, {"TRU","audi_q5",audiQ5_20T},
    // Stellantis — see jeepWagoneer57() for why the WMI alone was dangerous here.
    // 1C4 ONLY. 1J4 and 3C4 were here and are removed: vPIC resolves 1J4 at the
    // Wagoneer-era year codes to a 1992-96 Cherokee (year codes repeat every 30
    // years, so the gate above cannot separate them), and 3C4 produces no
    // vehicle at all at those codes. Both offered false positives and matched
    // nothing real.
    {"1C4","jeep_ws",jeepWagoneer57},
    // Ford (1FT/...) returns nullptr until that profile is registered — add a
    // row + a registry entry together.
  };
  for (const M& m : MAP)
    if (std::strcmp(w, m.wmi) == 0)
      return (!m.extra || m.extra(vin)) ? m.key : nullptr;
  return nullptr;
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
