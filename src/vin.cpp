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
static bool gmSierra1500Diesel(const char* vin) {
  if (std::strlen(vin) < 8) return false;            // need vin[7]; guards the reads below
  const char c3 = (char)std::toupper((unsigned char)vin[3]);
  const char c4 = (char)std::toupper((unsigned char)vin[4]);
  const char c7 = (char)std::toupper((unsigned char)vin[7]);
  return std::strchr("NPRUV", c3) != nullptr &&
         std::strchr("HU",    c4) != nullptr &&
         c7 == '8';
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
    // STILL WMI-ONLY (nullptr predicate). Each of these is as over-broad as the
    // GM rows were — a WMI covers a manufacturer's whole range, not one engine.
    // Tighten them the same way once someone establishes the discriminator.
    {"WBA","bmw_f10_535i",nullptr}, {"WBS","bmw_f10_535i",nullptr},
    {"5UX","bmw_f10_535i",nullptr}, {"4US","bmw_f10_535i",nullptr},
    {"WAU","audi_q5",nullptr}, {"WA1","audi_q5",nullptr},
    {"WUA","audi_q5",nullptr}, {"TRU","audi_q5",nullptr},
    // Stellantis. These WMIs cover Jeep broadly, but the profile was scanned on
    // a WS-platform Wagoneer specifically — a Wrangler or Cherokee sharing a WMI
    // gets the Wagoneer's 29-bit addressing, which will simply read nothing on a
    // vehicle that answers 11-bit. Settings -> Pick Vehicle overrides it.
    {"1C4","jeep_ws",nullptr}, {"1J4","jeep_ws",nullptr}, {"3C4","jeep_ws",nullptr},
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
