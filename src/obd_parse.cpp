#include "obd_parse.h"
#include <cctype>

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Assemble an ELM327 multi-frame ISO-TP reply into a clean payload hex string.
// Such replies look like "LLL\r0:HEX\r1:HEX\r..." where LLL is the total byte
// length and each "N:" line carries one CAN frame's data (8-byte frames padded
// with 0x55). Returns true and fills hexOut (uppercase hex, truncated to LLL
// bytes) when `resp` is multi-frame; returns false when there is no "N:" frame
// line, so the caller uses its normal single-frame path.
static bool assembleMultiFrame(const std::string& resp, std::string& hexOut) {
  hexOut.clear();
  bool sawFrame = false;
  int  declaredLen = -1;
  std::string acc;

  size_t start = 0;
  while (start <= resp.size()) {
    size_t end = resp.find_first_of("\r\n", start);
    std::string raw = (end == std::string::npos) ? resp.substr(start)
                                                 : resp.substr(start, end - start);
    // Normalize the line: uppercase, drop spaces/tabs.
    std::string t;
    for (char ch : raw)
      if (ch != ' ' && ch != '\t')
        t += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    size_t colon = t.find(':');
    if (colon != std::string::npos) {
      // Frame line "N:HEX" — keep the hex chars after the colon.
      sawFrame = true;
      for (size_t k = colon + 1; k < t.size(); k++)
        if (hexVal(t[k]) >= 0) acc += t[k];
    } else if (t.size() == 3 &&
               hexVal(t[0]) >= 0 && hexVal(t[1]) >= 0 && hexVal(t[2]) >= 0) {
      // Length header line, e.g. "00C".
      declaredLen = (hexVal(t[0]) << 8) | (hexVal(t[1]) << 4) | hexVal(t[2]);
    }
    // Any other line (OK / SEARCHING... / blank / ">" prompt) is ignored.

    if (end == std::string::npos) break;
    start = end + 1;
  }

  if (!sawFrame) return false;
  if (declaredLen >= 0) {
    size_t want = static_cast<size_t>(declaredLen) * 2;
    // Under-length = a CAN fragment was dropped (BLE notify loss). Reject the whole
    // reply (empty -> parseObdResponse returns false -> caller keeps last good value)
    // rather than hand a decoder truncated bytes that would decode to a bogus value.
    if (acc.size() < want) { hexOut.clear(); return true; }
    if (acc.size() > want) acc.resize(want);            // strip ISO-TP 0x55 padding
  } else if (acc.size() > 128) {
    acc.resize(128);                                     // no length header: cap at 64 bytes
  }
  hexOut = acc;
  return true;
}

bool parseObdResponse(const std::string& resp, uint8_t mode, uint16_t pid,
                      std::vector<uint8_t>& data) {
  data.clear();

  std::string s;
  std::string assembled;
  if (assembleMultiFrame(resp, assembled)) {
    // Multi-frame: `assembled` is already uppercase hex with no separators.
    s = assembled;
  } else {
    // Single-frame path: strip whitespace and the prompt; reject error replies.
    for (char c : resp) {
      if (c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '>') continue;
      s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (s.empty()) return false;
    if (s.find("NODATA") != std::string::npos) return false;
    if (s.find('?') != std::string::npos) return false;

    // Drop a leading "SEARCHING..." token if present.
    size_t pos = s.find("SEARCHING...");
    if (pos != std::string::npos) s.erase(pos, 12);

    // Drop a leading "OK" token if present (ELM returns "OK" for AT SH commands
    // immediately before the data response in the same accumulated buffer).
    if (s.size() >= 2 && s[0] == 'O' && s[1] == 'K') s.erase(0, 2);
  }

  // Parse hex pairs into bytes; any non-hex char is a malformed reply.
  std::vector<uint8_t> bytes;
  if (s.size() % 2 != 0) return false;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    int hi = hexVal(s[i]);
    int lo = hexVal(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  if (mode == 0x01) {
    if (bytes.size() < 3) return false;               // service + pid + >=1 data
    if (bytes[0] != 0x41) return false;
    if (bytes[1] != static_cast<uint8_t>(pid & 0xFF)) return false;
    data.assign(bytes.begin() + 2, bytes.end());
    return true;
  }
  if (mode == 0x22) {
    if (bytes.size() < 4) return false;               // service + pidHi + pidLo + >=1 data
    if (bytes[0] != 0x62) return false;
    if (bytes[1] != static_cast<uint8_t>((pid >> 8) & 0xFF)) return false;
    if (bytes[2] != static_cast<uint8_t>(pid & 0xFF)) return false;
    data.assign(bytes.begin() + 3, bytes.end());
    return true;
  }
  return false;
}
