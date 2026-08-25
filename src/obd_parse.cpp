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

// Decode one already-normalized hex string (uppercase, no separators) into the
// payload bytes for (mode, pid). Leaves `data` empty and returns false unless the
// service and PID bytes match, so a caller can try several candidate strings and
// keep the first that fits.
static bool decodeFrameHex(const std::string& s, uint8_t mode, uint16_t pid,
                           std::vector<uint8_t>& data) {
  data.clear();
  std::vector<uint8_t> bytes;
  if (s.empty() || s.size() % 2 != 0) return false;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    int hi = hexVal(s[i]);
    int lo = hexVal(s[i + 1]);
    if (hi < 0 || lo < 0) return false;              // any non-hex char = malformed
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

// Try each line of `resp` on its own, in buffer order, and keep the first that
// decodes as a positive reply for (mode, pid).
//
// WHY THIS EXISTS: on a functional broadcast (7DF) several modules answer the
// same request, and a module that does not implement the DID replies with a
// negative response (7F <service> <NRC>). parseObdResponse's primary path strips
// the \r separators and concatenates everything, so the service-byte check sees
// whichever frame landed FIRST -- and the order is not stable. A 2026-08-24
// sweep of a BMW F10 (462 answering DIDs on 7DF) had the NAK arrive ahead of the
// positive frame on 4 of them, 0.9%: those reads were rejected outright and the
// caller held its previous value. Falling back to a per-line scan recovers them.
//
// Deliberately a FALLBACK rather than the primary path: the concatenating parse
// runs first and unchanged, so no reply that decodes today can decode
// differently now. This can only turn a former `false` into a value.
static bool parseFirstPositiveLine(const std::string& resp, uint8_t mode, uint16_t pid,
                                   std::vector<uint8_t>& data) {
  size_t start = 0;
  while (start <= resp.size()) {
    size_t end = resp.find_first_of("\r\n", start);
    std::string raw = (end == std::string::npos) ? resp.substr(start)
                                                 : resp.substr(start, end - start);
    std::string t;
    for (char ch : raw) {
      if (ch == ' ' || ch == '\t' || ch == '>') continue;
      t += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (decodeFrameHex(t, mode, pid, data)) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  data.clear();
  return false;
}

bool parseObdResponse(const std::string& resp, uint8_t mode, uint16_t pid,
                      std::vector<uint8_t>& data) {
  data.clear();

  // Multi-frame (ISO-TP) replies are self-delimiting and already reassembled in
  // order, so they never need the per-line fallback below -- and their "N:HEX"
  // lines are not plain hex, so it could not help them anyway.
  std::string assembled;
  if (assembleMultiFrame(resp, assembled))
    return decodeFrameHex(assembled, mode, pid, data);

  // Single-frame path: strip whitespace and the prompt; reject error replies.
  std::string s;
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

  // Primary path, unchanged: the whole buffer as one concatenated frame.
  if (decodeFrameHex(s, mode, pid, data)) return true;

  // Fallback 1: a multi-responder buffer whose first frame is not ours (typically
  // another module's 7F NAK ahead of the positive reply). See the note above.
  if (parseFirstPositiveLine(resp, mode, pid, data)) return true;

  // Fallback 2: the same situation, but with the frames NOT separated by \r --
  // some adapters/log captures render a multi-responder buffer as one run of hex.
  // Strip well-formed negative-response frames (7F <service> <NRC>, exactly three
  // bytes) off the FRONT and retry. Only that exact shape is skipped, so this
  // cannot walk into payload data looking for a match.
  std::string rest = s;
  while (rest.size() >= 6 && rest[0] == '7' && rest[1] == 'F') {
    rest.erase(0, 6);
    if (decodeFrameHex(rest, mode, pid, data)) return true;
  }

  data.clear();
  return false;
}
