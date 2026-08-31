// Host unit tests for obd_parse. Response strings are realistic ELM327 replies.
#include "obd_parse.h"
#include <cstdio>
#include <string>
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
  // NAK-FIRST. The order the two frames arrive in is NOT stable: a 2026-08-24
  // sweep of an F10 on 7DF (462 answering DIDs) saw the NAK land ahead of the
  // positive frame on 4 of them, 0.9%. Concatenating puts 0x7F at byte 0, which
  // the service check rejects -- so those reads used to be dropped outright and
  // the caller held its previous value. parseObdResponse now falls back to a
  // per-line scan and finds the positive frame wherever it sits in the buffer.
  check("nak-then-pos", parseObdResponse("7F 22 22\r62 58 6F 09\r\r", 0x22, 0x586F, d), true);
  checkbyte("nak-then-pos-A", d.size() >= 1 ? d[0] : -1, 0x09);
  // The real 2-byte shape, both orderings, must yield the SAME payload -- this is
  // the pair that matters for decBmwOilPress (a u16 read of 0x0A72 = 2674 mbar).
  check("oilp-pos-then-nak", parseObdResponse("62 58 6F 0A 72\r7F 22 22\r\r", 0x22, 0x586F, d), true);
  checkbyte("oilp-pos-then-nak-A", d.size() >= 2 ? d[0] : -1, 0x0A);
  checkbyte("oilp-pos-then-nak-B", d.size() >= 2 ? d[1] : -1, 0x72);
  check("oilp-nak-then-pos", parseObdResponse("7F 22 22\r62 58 6F 0A 72\r\r", 0x22, 0x586F, d), true);
  checkbyte("oilp-nak-then-pos-A", d.size() >= 2 ? d[0] : -1, 0x0A);
  checkbyte("oilp-nak-then-pos-B", d.size() >= 2 ? d[1] : -1, 0x72);
  // Oil temp 224402, NAK first -- the wire shape, frames separated by \r.
  check("oiltemp-nak-first", parseObdResponse("7F 22 22\r62 44 02 00 BA\r\r>", 0x22, 0x4402, d), true);
  checkbyte("oiltemp-nak-first-A", d.size() >= 2 ? d[0] : -1, 0x00);
  checkbyte("oiltemp-nak-first-B", d.size() >= 2 ? d[1] : -1, 0xBA);
  // The SAME reply with no separator between the two frames. NOT AN OBSERVED WIRE
  // SHAPE -- every capture in hand is \r-separated, and this form is how a sweep
  // log rendered it. It exercises fallback 2, which exists because a coalesced
  // buffer would otherwise be discarded silently, and a silently dropped reading
  // is close to undiagnosable from the field. Six lines of insurance with no
  // false-positive surface: only an exact 7F <service> <NRC> triple is skipped,
  // and only from the front.
  check("oiltemp-nak-no-separator", parseObdResponse("7F2222 62440200BA  >", 0x22, 0x4402, d), true);
  checkbyte("oiltemp-nak-no-sep-A", d.size() >= 2 ? d[0] : -1, 0x00);
  checkbyte("oiltemp-nak-no-sep-B", d.size() >= 2 ? d[1] : -1, 0xBA);
  // A NAK with no positive frame anywhere is still a rejection, not a value.
  check("nak-only", parseObdResponse("7F 22 22\r\r", 0x22, 0x586F, d), false);
  // ...and a positive frame for a DIFFERENT DID must not be mistaken for ours.
  check("nak-then-other-did", parseObdResponse("7F 22 22\r62 44 02 00 BA\r\r", 0x22, 0x586F, d), false);
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

  // --- ISO-TP frame-index validation -------------------------------------------
  // assembleMultiFrame() concatenated every "N:" line without ever looking at N,
  // so a duplicated or out-of-order fragment decoded as valid data as long as the
  // bytes happened to reach the declared length. src/vin.cpp has always validated
  // the ordinal for exactly this reason -- a wrong VIN picks a wrong profile --
  // and the live-data path had no such guard. Reported alongside obd-discover#6.
  //
  // A duplicate index masking a missing fragment: 0: arrives twice, 1: never
  // does, and 6+7=13 bytes clears the declared 12. Must be REFUSED, not decoded.
  check("multiframe-duplicate-index",
        parseObdResponse("00C\r0:6200780706E1\r0:07770777000055\r\r", 0x22, 0x0078, d), false);
  // A gap: fragment 1 was dropped and 2 arrived, again reaching the declared
  // length. Splicing 2 into 1's position shifts every byte after it.
  check("multiframe-index-gap",
        parseObdResponse("00C\r0:6200780706E1\r2:07770777000055\r\r", 0x22, 0x0078, d), false);
  // Reordering: the right fragments, the wrong order.
  check("multiframe-out-of-order",
        parseObdResponse("00C\r1:07770777000055\r0:6200780706E1\r\r", 0x22, 0x0078, d), false);

  // ...and the wrap the validation must NOT break. The sequence number is four
  // bits, so the ELM prints F then 0 again: a reply of 17+ frames is legal and
  // must still assemble. 6 + 16*7 = 118 bytes = 0x76 declared.
  {
    std::string wide = "076\r0:620077010203";
    for (int i = 1; i <= 16; i++) {
      char idx = "0123456789ABCDEF"[i & 0x0F];       // wraps to '0' at i == 16
      wide += "\r"; wide += idx; wide += ":11223344556677";
    }
    wide += "\r\r";
    check("multiframe-sequence-wraps", parseObdResponse(wide.c_str(), 0x22, 0x0077, d), true);
    checkbyte("multiframe-wrap-size", (int)d.size(), 118 - 3);
    checkbyte("multiframe-wrap-first", (int)d[0], 0x01);
    checkbyte("multiframe-wrap-last", (int)d[d.size() - 1], 0x77);
  }

  if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
  std::printf("\n%d FAILURE(S)\n", g_failures);
  return 1;
}
