// Host unit tests for obd_parse. Response strings are realistic ELM327 replies.
#include "obd_parse.h"
#include <cstdio>
#include <vector>

static int g_failures = 0;
static void check(const char* name, bool got, bool want) {
  if (got == want) { std::printf("  ok   %s\n", name); }
  else { std::printf("  FAIL %s (got %d, want %d)\n", name, got, want); ++g_failures; }
}
static void checkbyte(const char* name, int got, int want) {
  if (got == want) { std::printf("  ok   %s\n", name); }
  else { std::printf("  FAIL %s (got 0x%02X, want 0x%02X)\n", name, got, want); ++g_failures; }
}

int main() {
  std::vector<uint8_t> d;

  std::printf("mode 01:\n");
  check("rpm-ok", parseObdResponse("41 0C 1A F8", 0x01, 0x0C, d), true);
  checkbyte("rpm-A", d.size() == 2 ? d[0] : -1, 0x1A);
  checkbyte("rpm-B", d.size() == 2 ? d[1] : -1, 0xF8);

  check("searching-preamble", parseObdResponse("SEARCHING...\r41 05 5A\r\r>", 0x01, 0x05, d), true);
  checkbyte("coolant-A", d.size() == 1 ? d[0] : -1, 0x5A);

  // AT SH + query: ELM returns "OK\r" before the data line — parser must skip it.
  // Real buffer looks like: "OK\r\r41 05 5A \r\r>" (AT SH OK then coolant response).
  check("ok-prefix-mode01", parseObdResponse("OK\r\r41 05 5A \r\r>", 0x01, 0x05, d), true);
  checkbyte("ok-prefix-mode01-A", d.size() == 1 ? d[0] : -1, 0x5A);

  check("pid-mismatch", parseObdResponse("41 0C 1A F8", 0x01, 0x05, d), false);
  check("service-mismatch", parseObdResponse("7F 01 12", 0x01, 0x0C, d), false);

  std::printf("mode 22:\n");
  // Trans temp: 62 19 40 7D -> data 0x7D (=125 -> 85C -> 185F).
  check("trans-ok", parseObdResponse("62 19 40 7D", 0x22, 0x1940, d), true);
  checkbyte("trans-A", d.size() == 1 ? d[0] : -1, 0x7D);
  check("trans-pid-mismatch", parseObdResponse("62 19 41 7D", 0x22, 0x1940, d), false);

  // AT SH + query: ELM returns "OK\r" before the mode 22 data line.
  // Real buffer looks like: "OK\r\r62 19 40 7D \r\r>" (AT SH OK then trans-temp response).
  check("ok-prefix-mode22", parseObdResponse("OK\r\r62 19 40 7D \r\r>", 0x22, 0x1940, d), true);
  checkbyte("ok-prefix-mode22-A", d.size() == 1 ? d[0] : -1, 0x7D);

  // BMW 7DF functional broadcast: a positive Mode-22 frame arrives with a SECOND
  // module's "7F2222" NAK appended (no separator once \r is stripped). The BMW
  // decoders (bmw_f10_535i.cpp, oil pressure 586F) read byte 0 ONLY, so this must
  // parse and expose the real value byte with the NAK bytes harmlessly trailing.
  std::printf("mode 22 (7DF functional + NAK tail):\n");
  check("pos-then-nak", parseObdResponse("62 58 6F 09\r7F 22 22\r\r", 0x22, 0x586F, d), true);
  checkbyte("pos-then-nak-A", d.size() >= 1 ? d[0] : -1, 0x09);   // real value; trailing NAK ignored by byte-0 decode
  // NAK-first: byte 0 is 0x7F != 0x62 -> whole reply rejected -> caller keeps last good value.
  check("nak-then-pos", parseObdResponse("7F 22 22\r62 58 6F 09\r\r", 0x22, 0x586F, d), false);
  // Two positive responders concatenated: first one wins (byte-0 decoders read d[0]).
  check("two-positive", parseObdResponse("62 58 6F 09\r62 58 6F 11\r\r", 0x22, 0x586F, d), true);
  checkbyte("two-positive-A", d.size() >= 1 ? d[0] : -1, 0x09);

  std::printf("errors:\n");
  check("no-data", parseObdResponse("NO DATA", 0x01, 0x0C, d), false);
  check("question", parseObdResponse("?", 0x01, 0x0C, d), false);
  check("empty", parseObdResponse("", 0x01, 0x0C, d), false);
  check("too-short", parseObdResponse("41 0C", 0x01, 0x0C, d), false);

  std::printf("multi-frame ISO-TP:\n");
  // EGT 220078: "00C" = 12 bytes; frames "0:"(6 bytes) + "1:"(7 bytes), trailing
  // 0x55 is CAN padding. Assembled: 62 0078 | 07 06E1 0777 0777 0000 -> 9 data
  // bytes once the pad is truncated to the declared length.
  check("egt-multiframe", parseObdResponse("00C\r0:6200780706E1\r1:07770777000055\r\r", 0x22, 0x0078, d), true);
  checkbyte("egt-size", (int)d.size(), 9);
  checkbyte("egt-mask", d.size()==9 ? d[0] : -1, 0x07);
  checkbyte("egt-s1-hi", d.size()==9 ? d[1] : -1, 0x06);
  checkbyte("egt-s1-lo", d.size()==9 ? d[2] : -1, 0xE1);
  checkbyte("egt-pad-trunc-7", d.size()==9 ? d[7] : -1, 0x00);
  checkbyte("egt-pad-trunc-8", d.size()==9 ? d[8] : -1, 0x00);

  // DPF dP 22007A: "00A" = 10 bytes; assembled 62 007A | 05 0151 FFFF 0099 -> 7 data bytes.
  check("dpfdp-multiframe", parseObdResponse("00A\r0:62007A050151\r1:FFFF0099555555\r\r", 0x22, 0x007A, d), true);
  checkbyte("dpfdp-size", (int)d.size(), 7);
  checkbyte("dpfdp-b", d.size()==7 ? d[1] : -1, 0x01);
  checkbyte("dpfdp-c", d.size()==7 ? d[2] : -1, 0x51);

  // An "OK" preamble (from AT SH) before a multi-frame reply must be ignored.
  check("ok-prefix-multiframe", parseObdResponse("OK\r00C\r0:6200780706E1\r1:07770777000055\r\r", 0x22, 0x0078, d), true);
  checkbyte("ok-prefix-multiframe-size", (int)d.size(), 9);

  // Multi-frame reply with the wrong PID is still rejected.
  check("multiframe-pid-mismatch", parseObdResponse("00C\r0:6200780706E1\r1:07770777000055\r\r", 0x22, 0x007A, d), false);

  // DPF T 22007C is genuinely NO DATA (single-frame error path, no frame line).
  check("dpft-no-data", parseObdResponse("NO DATA\r\r", 0x22, 0x007C, d), false);

  // Under-length multi-frame (a CAN fragment was dropped) is rejected, not decoded
  // as truncated -> caller keeps last good value.
  check("multiframe-underlength", parseObdResponse("00C\r0:62009B0E84\r\r", 0x22, 0x009B, d), false);
  // Over-length multi-frame: ISO-TP 0x55 padding stripped to the declared length.
  check("multiframe-pad-strip", parseObdResponse("00C\r0:620083C3008E\r1:0004FFFFFFFF55\r\r", 0x22, 0x0083, d), true);
  checkbyte("multiframe-pad-d1", (int)d[1], 0x00);
  checkbyte("multiframe-pad-d2", (int)d[2], 0x8E);

  if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
  std::printf("\n%d FAILURE(S)\n", g_failures);
  return 1;
}
