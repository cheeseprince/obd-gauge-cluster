#include <cstdio>
#include <cstring>
#include "../src/vin.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  char vin[18];
  // parseVinFromPayload: 0x01 count byte + 17 ascii; trailing-17 heuristic.
  { const char* v="1GT0123456789ABCD"; uint8_t p[18]; p[0]=0x01; memcpy(p+1,v,17);
    check(parseVinFromPayload(p,18,vin) && strcmp(vin,v)==0, "parse count+17"); }
  { const char* v="WBA0123456789ABCD"; uint8_t p[17]; memcpy(p,v,17);
    check(parseVinFromPayload(p,17,vin) && strcmp(vin,v)==0, "parse bare 17"); }
  check(!parseVinFromPayload((const uint8_t*)"\x01\x02",2,vin), "short -> false");

  // parseVinReply: a real multi-frame 0902 ELM reply (spaces + frame indices).
  // ELM327 assembles Mode-09 PID-02 (VIN) over 3 ISO-TP frames: "LLL\r0:HEX\r
  // 1:HEX\r2:HEX\r>" where LLL=014 (20 decimal bytes) and the assembled 20
  // bytes decode to: 49 02 01 <17 VIN ASCII bytes> = "1GT0123456789ABCD".
  // Verified by hand: concatenating the 3 frames' hex (42 chars = 21 bytes),
  // then truncating to the declared 20 bytes (drops the final ISO-TP pad
  // byte) yields exactly service=0x49, pid=0x02, msgCount=0x01, followed by
  // the ASCII bytes of "1GT0123456789ABCD" (17 chars) -- this is the real
  // GMC Sierra WMI (1GT) used elsewhere in this file's WMI-map assertions.
  { const char* reply = "014\r0:490201314754\r1:30313233343536\r2:3738394142434400\r>";
    check(parseVinReply(reply,vin) && strcmp(vin,"1GT0123456789ABCD")==0,
          "parseVinReply assembles + decodes the real VIN"); }
  // Malformed replies must fail closed, not crash or return garbage.
  check(!parseVinReply(nullptr,vin), "null reply -> false");
  check(!parseVinReply("NO DATA\r>",vin), "no-frame reply -> false");
  { const char* short_reply = "014\r0:490201314754\r1:3031323334353\r>"; // frame 2 missing entirely, and
    // frame 1 is itself short a nibble -- either way the assembled hex (25 chars)
    // falls well under the declared 40 (014 = 20 bytes), so this is a dropped-
    // frame/under-length fragment, not an odd-nibble parity error.
    check(!parseVinReply(short_reply,vin), "dropped-frame/under-length reply -> false"); }
  // No "LLL" declared-length header line at all (still multi-frame: has "N:"
  // lines). assembleIsoTpFrames must fail closed here rather than use the
  // accumulated hex unbounded -- an uncapped headerless assembly could let
  // trailing ISO-TP 0x55 pad bytes shift the trailing-17-byte VIN window and
  // yield a wrong-but-plausible VIN. Same frame hex as the real reply above,
  // just missing the "014\r" length line.
  { const char* headerless_reply = "0:490201314754\r1:30313233343536\r2:3738394142434400\r>";
    check(!parseVinReply(headerless_reply,vin), "headerless multi-frame reply -> false (fails closed)"); }

  // Frame-ordinal validation: a reordered or duplicated frame (BLE-notify glitch)
  // must fail closed, NOT splice bytes into a wrong-but-length-correct VIN that
  // would map to the wrong vehicle profile.
  { const char* reordered = "014\r0:490201314754\r2:3738394142434400\r1:30313233343536\r>";
    check(!parseVinReply(reordered,vin), "out-of-order frames (0,2,1) -> false (fails closed)"); }
  { const char* dup = "014\r0:490201314754\r0:490201314754\r1:30313233343536\r2:3738394142434400\r>";
    check(!parseVinReply(dup,vin), "duplicate frame -> false (fails closed)"); }

  // vinToProfileKey: WMI -> registry key.
  check(strcmp(vinToProfileKey("1GT0123456789ABCD"),"gm_sierra_lz0")==0, "1GT->gm");
  check(strcmp(vinToProfileKey("3gt0123456789abcd"),"gm_sierra_lz0")==0, "3GT->gm (case)");
  check(strcmp(vinToProfileKey("WBA0123456789ABCD"),"bmw_f10_535i")==0, "WBA->bmw");
  check(strcmp(vinToProfileKey("WAU0123456789ABCE"),"audi_q5")==0, "WAU->audi_q5");
  check(strcmp(vinToProfileKey("wa10123456789abcd"),"audi_q5")==0, "WA1->audi_q5 (case)");
  check(strcmp(vinToProfileKey("1C40123456789ABCD"),"jeep_ws")==0, "1C4->jeep_ws");
  check(strcmp(vinToProfileKey("3c40123456789abcd"),"jeep_ws")==0, "3C4->jeep_ws (case)");
  check(vinToProfileKey("JHM0123456789ABCD")==nullptr, "unknown WMI -> null");
  check(vinToProfileKey("WA")==nullptr, "short -> null");

  // vinAutoTarget: the decision.
  check(strcmp(vinAutoTarget("1GT0123456789ABCD", true, ""),"gm_sierra_lz0")==0, "auto+changed -> key");
  check(vinAutoTarget("1GT0123456789ABCD", true, "gm_sierra_lz0")==nullptr, "already current -> null");
  check(vinAutoTarget("1GT0123456789ABCD", false, "")==nullptr, "auto off -> null");
  check(vinAutoTarget("JHM0123456789ABCD", true, "")==nullptr, "unmapped -> null");
  check(vinAutoTarget("", true, "gm_sierra_lz0")==nullptr, "empty vin -> null");
  check(strcmp(vinAutoTarget("WAU0123456789ABCE", true, "gm_sierra_lz0"),"audi_q5")==0, "audi VIN + auto + different current -> key");

  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("test_vin: ALL PASS\n"); return 0;
}
