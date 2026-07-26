// Fuzz + sanitizer harness for the OBD parse + decode path.
//
// Feeds large volumes of arbitrary / malformed / structured input at
// parseObdResponse() and at EVERY decoder (READOUTS[i].decode), to prove the
// hardening (NaN guards, length checks, multi-frame under-length reject) has no
// out-of-bounds read or undefined behavior on hostile input.
//
// Built with -fsanitize=address,undefined -fno-sanitize-recover=all (see Makefile):
// any OOB read, signed overflow, bad shift, null deref, etc. ABORTS the run with a
// nonzero exit, failing `make`. Deterministic (own xorshift PRNG) so failures are
// reproducible.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "obd_parse.h"
#include "readouts.h"
#include "app_types.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

// Reproducible PRNG (xorshift32). Never 0.
static uint32_t g_rng = 0x12345678u;
static uint32_t rnd() {
  uint32_t x = g_rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  g_rng = x ? x : 0x12345678u;
  return g_rng;
}

// Run every decoder against one (ptr,len) — the core OOB/UB check. `data` is sized
// EXACTLY len (heap), so any read past len is a real heap-buffer-overflow ASan trips.
static volatile float g_sink = 0.0f;
static long g_decodeCalls = 0;
static void hammerDecoders(const uint8_t* data, int len, float* vals) {
  DecodeCtx ctx{vals};
  for (int i = 0; i < READOUT_COUNT; i++) {
    if (READOUTS[i].decode) { g_sink += READOUTS[i].decode(data, len, ctx); g_decodeCalls++; }
  }
}

// Build a syntactically valid ELM reply for table row `idx` (random data + random
// framing: single-line or multi-frame). Returns the string and the mode/pid to
// parse it with, so parseObdResponse() takes its SUCCESS path.
static std::string makeValidFrame(int idx, uint8_t& mode, uint16_t& pid) {
  const char* cmd = READOUTS[idx].cmd;               // caller ensures non-null
  mode = (cmd[0] == '2' && cmd[1] == '2') ? 0x22 : 0x01;
  pid  = (uint16_t)strtol(cmd + 2, nullptr, 16);
  char hdr[16];
  if (mode == 0x22) std::snprintf(hdr, sizeof hdr, "62%04X", (unsigned)pid);
  else              std::snprintf(hdr, sizeof hdr, "41%02X", (unsigned)(pid & 0xFF));
  std::string body = hdr;
  int dn = 1 + (int)(rnd() % 6);                      // 1..6 random data bytes
  for (int i = 0; i < dn; i++) { char b[8]; std::snprintf(b, sizeof b, "%02X", (unsigned)(rnd() & 0xFF)); body += b; }
  if (rnd() & 1) return body + "\r>";                 // single frame
  // Multi-frame: declared length header + 7-byte "N:" fragments.
  char lh[16]; std::snprintf(lh, sizeof lh, "%03X", (unsigned)(body.size() / 2));
  std::string s = lh; s += '\r';
  int fi = 0;
  for (size_t p = 0; p < body.size(); p += 14) { char pf[16]; std::snprintf(pf, sizeof pf, "%d:", fi++); s += pf; s += body.substr(p, 14); s += '\r'; }
  return s + ">";
}

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  float vals[STAT_COUNT] = {0};
  // A handful of bytes sometimes referenced via ctx (e.g. baro for boost).
  vals[IDX_BARO] = 101.3f;

  long parseCalls = 0, parseTrue = 0;
  const uint8_t modes[] = {0x01, 0x22};

  // --- Seed corpus: known-tricky inputs first (must never crash) ---
  static const char* corpus[] = {
    "", ">", "\r\r", "OK\r\r>", "NO DATA\r\r", "?\r", "SEARCHING...\r41 0C 1A F8\r>",
    "41 0C 1A F8", "62 00 9B 0E 84 41 E1", "7F 22 31",
    "00C\r0:62009B0E84\r\r",                       // under-length multiframe
    "00C\r0:620083C3008E\r1:0004FFFFFFFF55\r\r",   // padded multiframe
    "ZZZZ", "414141414141", ":::", "0:\r1:\r2:\r", "FFF\r0:\r",
  };
  for (const char* c : corpus) {
    std::string s(c);
    for (uint8_t mode : modes) {
      std::vector<uint8_t> d; parseCalls++;
      if (parseObdResponse(s, mode, (uint16_t)(rnd() & 0xFFFF), d)) { parseTrue++; if (!d.empty()) hammerDecoders(d.data(), (int)d.size(), vals); }
    }
  }

  // --- Loop A: parser fuzz. Half random-alphabet, half structured multi-frame. ---
  static const char alpha[] = "0123456789ABCDEFabcdef:\r\n >OKNODATA?5";
  const int NA = (int)sizeof(alpha) - 1;
  for (long it = 0; it < 120000; it++) {
    std::string s;
    uint8_t mode; uint16_t pid;
    uint32_t pick = rnd() % 3;
    if (pick == 2) {
      // Valid frame for a real row -> drives the parse SUCCESS path + decoders.
      int idx;
      do { idx = (int)(rnd() % READOUT_COUNT); } while (!READOUTS[idx].cmd);
      s = makeValidFrame(idx, mode, pid);
    } else {
      mode = modes[rnd() & 1];
      pid  = (uint16_t)(rnd() & 0xFFFF);
      if (pick == 0) {                                  // random alphabet soup
        int len = (int)(rnd() % 80);
        s.reserve(len);
        for (int i = 0; i < len; i++) s += alpha[rnd() % NA];
      } else {                                          // structured-ish multi-frame
        char hdr[16]; std::snprintf(hdr, sizeof hdr, "%03X", (unsigned)(rnd() % 0x60));
        s += hdr; s += '\r';
        int frags = (int)(rnd() % 5);
        for (int f = 0; f < frags; f++) {
          char p[16]; std::snprintf(p, sizeof p, "%d:", f);
          s += p;
          int hl = (int)(rnd() % 16);
          for (int k = 0; k < hl; k++) s += alpha[rnd() % 16];   // hex chars only
          s += '\r';
        }
      }
    }
    std::vector<uint8_t> d; parseCalls++;
    if (parseObdResponse(s, mode, pid, d)) {
      parseTrue++;
      if (!d.empty()) hammerDecoders(d.data(), (int)d.size(), vals);
    }
  }

  // --- Loop B: decoder fuzz. Every decoder vs exact-sized random buffers, len 0..8
  // (n=0 included -> catches any unguarded d[0] read). Heap-sized so OOB is real. ---
  DecodeCtx ctx{vals};
  for (long it = 0; it < 300000; it++) {
    int n = (int)(rnd() % 9);                 // 0..8
    std::vector<uint8_t> buf(n);
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)(rnd() & 0xFF);
    int idx = (int)(rnd() % READOUT_COUNT);
    if (READOUTS[idx].decode) { g_sink += READOUTS[idx].decode(buf.data(), n, ctx); g_decodeCalls++; }
  }

  std::printf("parse calls=%ld (true=%ld)  decode calls=%ld  sink=%g\n",
              parseCalls, parseTrue, g_decodeCalls, (double)g_sink);
  std::printf("\nFUZZ OK\n");
  return 0;
}
