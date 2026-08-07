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
  //
  // GM is NOT WMI-only. 1GT/3GT/1GC/3GC are GM light-truck-wide: they also cover
  // gasoline Silverado/Sierra 1500 (L84 5.3 V8, L87 6.2 V8, L3B 2.7 turbo-4) and
  // Sierra/Silverado HD (L5P 6.6 Duramax). Handing any of those the LZ0 profile
  // makes the dash poll Mode-22 PIDs (221940 trans temp, 220078 EGT, 220023 rail,
  // 220010 MAF) at headers 7E0/7E2 that either do not exist or mean something
  // else there -- the same over-broad-WMI failure the jeep_ws rows warn about.
  //
  // NHTSA vPIC keys the 1500 engine entirely off VIN position 8 (vin[7]), gated
  // on positions 4-5 (vin[3], vin[4]):
  //   [NPRUV][HU]**8 -> LZ0 3.0L I6 turbo diesel   <-- the only fit for this profile
  //   [NPRUV][HU]**D -> L84 5.3L V8 gas
  //   [NPRUV][HU]**L -> L87 6.2L V8 gas
  //   [NPRUV][HU]**K -> L3B 2.7L I4 turbo gas
  // HD lives on a different schema ([NPRUV][89]*E), so vin[4] separates 1500 from HD.
  //
  // EVERY VIN BELOW IS SYNTHETIC (…012345678 / …0123456789ABCD tails, no relation
  // to any real vehicle). Never commit a real VIN -- scripts/check_no_pii.py
  // allowlists these exact tokens and fails CI on any other VIN-shaped string.
  // MODEL YEAR IS PART OF THE PATTERN. vin[9] is VIN position 10, the model-year
  // code (P=2023, R=2024, S=2025, T=2026 — verified against NHTSA vPIC, which
  // resolves the year from that position when given no hint).
  //
  // Without it the positional rule alone is NOT unique across 20 model years.
  // Found 2026-08-05 by sweeping vPIC over every year code: a 2011-12 Chevrolet
  // EXPRESS VAN with the 6.6L Duramax satisfies WMI 1GC + vin[3] in [NPRUV] +
  // vin[4]='H' + vin[7]='8' exactly, and would have been handed the Sierra LZ0
  // profile. A working van is far likelier to meet an OBD dongle than most
  // false positives, so this is not theoretical.
  //
  // A year outside the verified range fails CLOSED. A 2027 truck gets Generic
  // until someone confirms the profile still fits it — which is the same rule
  // the engine discriminator follows.
  check(strcmp(vinToProfileKey("3GTUUEE80S2345678"),"gm_sierra_lz0")==0, "LZ0 MY2025 (S) -> gm");
  check(strcmp(vinToProfileKey("3GTUUEE80P2345678"),"gm_sierra_lz0")==0, "LZ0 MY2023 (P) -> gm");
  check(strcmp(vinToProfileKey("3GTUUEE80T2345678"),"gm_sierra_lz0")==0, "LZ0 MY2026 (T) -> gm");
  check(vinToProfileKey("1GCNHEE80B2345678")==nullptr, "2011 Express van 6.6 Duramax -> null");
  check(vinToProfileKey("3GTUUEE80V2345678")==nullptr, "MY2027 (V), unverified year -> null");
  check(vinToProfileKey("3GTUUEE80")==nullptr, "too short to read vin[9] -> null");
  check(strcmp(vinToProfileKey("3GTUUEE80S2345678"),"gm_sierra_lz0")==0, "3GT LZ0 diesel -> gm");
  check(strcmp(vinToProfileKey("1GTUUEE80S2345678"),"gm_sierra_lz0")==0, "1GT LZ0 diesel -> gm");
  check(strcmp(vinToProfileKey("3gtuuee80s2345678"),"gm_sierra_lz0")==0, "LZ0 -> gm (case)");
  check(vinToProfileKey("3GTUUEED0S2345678")==nullptr, "L84 5.3 gas, same WMI -> null");
  check(vinToProfileKey("3GTUUEEL0S2345678")==nullptr, "L87 6.2 gas, same WMI -> null");
  check(vinToProfileKey("3GTUUEEK0S2345678")==nullptr, "L3B 2.7 gas, same WMI -> null");
  check(vinToProfileKey("3GTU8EEE0S2345678")==nullptr, "Sierra HD (vin[4] not H/U) -> null");
  check(vinToProfileKey("1GT0123456789ABCD")==nullptr, "GM WMI but non-1500 pattern -> null");
  check(vinToProfileKey("1GTUUEE")==nullptr, "GM WMI but too short to read vin[7] -> null");
  // BMW: the profile was scanned on an F10 535i (N55 3.0 turbo I6). vPIC frame,
  // verified 2011-2015 -- vin[3]='F', vin[4] R(RWD)/U(xDrive), vin[5]=model,
  // vin[6]='C', vin[7]='5'. vin[5] is the discriminator and DISPLACEMENT IS NOT:
  // the 528i is also 3.0/6cyl in 2011-13, so only vin[5]='7' separates them.
  // The frame decodes to nothing from 2016 (F10 -> G30), so it is self-limiting.
  check(strcmp(vinToProfileKey("WBAFR7C5012345678"),"bmw_f10_535i")==0, "F10 535i RWD -> bmw");
  check(strcmp(vinToProfileKey("WBAFU7C5012345678"),"bmw_f10_535i")==0, "F10 535i xDrive -> bmw");
  check(strcmp(vinToProfileKey("wbafr7c5012345678"),"bmw_f10_535i")==0, "F10 535i -> bmw (case)");
  check(vinToProfileKey("WBAFR1C5012345678")==nullptr, "528i (also 3.0 I6) -> null");
  check(vinToProfileKey("WBAFR9C5012345678")==nullptr, "550i 4.4 V8 -> null");
  check(vinToProfileKey("WBA0123456789ABCD")==nullptr, "BMW WMI, non-F10 pattern -> null");

  // Audi: profile is the Q5 (typ FY) 2.0T EA888.3, 2018-20. vin[4] separates the
  // Q5 (N) from the SQ5 (4, EA839 3.0 V6 -- different PIDs entirely) and vin[7]
  // separates Q5 (Y) from Q3(3)/Q7(7)/Q8(1). 2021 moved the Q5 to vin[4]='A',
  // so requiring 'N' also keeps the later facelift out.
  check(strcmp(vinToProfileKey("WA1ANAFY012345678"),"audi_q5")==0, "Q5 2.0T -> audi_q5");
  check(strcmp(vinToProfileKey("WA1BNAFY012345678"),"audi_q5")==0, "Q5 2.0T other trim -> audi_q5");
  check(strcmp(vinToProfileKey("wa1anafy012345678"),"audi_q5")==0, "Q5 2.0T -> audi_q5 (case)");
  check(vinToProfileKey("WA1A4AFY012345678")==nullptr, "SQ5 3.0 V6 -> null");
  check(vinToProfileKey("WA1ANAF1012345678")==nullptr, "Q8 -> null");
  check(vinToProfileKey("WA1ANAF7012345678")==nullptr, "Q7 -> null");
  check(vinToProfileKey("WAU0123456789ABCE")==nullptr, "Audi WMI, non-Q5 pattern -> null");

  // Jeep: profile scanned on a WS Wagoneer 5.7 Hemi eTorque. 1C4/1J4/3C4 are
  // Stellantis-wide and cover Grand Cherokee, Cherokee, Wrangler AND the Ducato /
  // ProMaster vans (vin[4] F and R) -- all of which previously got the Wagoneer's
  // 29-bit addressing. vin[7] is the engine: T=5.7 V8, P=3.0 I6 Hurricane,
  // J=6.4 V8, G=3.6 V6. The 5.7 is 2022-23 only; 2024 Wagoneers are 3.0 I6.
  // Same cross-year problem, same fix. The Wagoneer 5.7 positional rule also
  // matched a Chrysler Voyager (2001-03) and a Jeep Cherokee (1992-96).
  // N=2022, P=2023 — the years the profile was scanned on.
  check(strcmp(vinToProfileKey("1C4SJVBT0N2345678"),"jeep_ws")==0, "Wagoneer MY2022 (N) -> jeep_ws");
  check(strcmp(vinToProfileKey("1C4SJVBT0P2345678"),"jeep_ws")==0, "Wagoneer MY2023 (P) -> jeep_ws");
  // 1J4 is removed from the table entirely: at the Wagoneer year codes vPIC
  // resolves it to a 1992-96 Cherokee, and year codes repeat every 30 years so
  // the gate cannot disambiguate. The WMI list is the discriminator here.
  check(vinToProfileKey("1J4SJVBT0N2345678")==nullptr, "1J4 is not a Wagoneer WMI -> null");
  check(vinToProfileKey("1C4SJVBT0T2345678")==nullptr, "MY2026 (T) is Grand Wagoneer -> null");
  check(strcmp(vinToProfileKey("1C4SJVBT0N2345678"),"jeep_ws")==0, "Wagoneer 5.7 -> jeep_ws");
  check(strcmp(vinToProfileKey("1c4sjvbt0n2345678"),"jeep_ws")==0, "Wagoneer 5.7 -> jeep_ws (case)");
  check(vinToProfileKey("1C4SJVBP0N2345678")==nullptr, "Wagoneer 3.0 Hurricane -> null");
  check(vinToProfileKey("1C4SJVBJ0N2345678")==nullptr, "Wagoneer 6.4 V8 -> null");
  check(vinToProfileKey("1C4SJVET0N2345678")==nullptr, "Grand Wagoneer -> null");
  check(vinToProfileKey("1C4SJXBT0N2345678")==nullptr, "Wrangler -> null");
  check(vinToProfileKey("1C4SFVBT0N2345678")==nullptr, "Ducato van (vin[4]='F') -> null");
  check(vinToProfileKey("1C40123456789ABCD")==nullptr, "Jeep WMI, non-Wagoneer pattern -> null");
  check(vinToProfileKey("JHM0123456789ABCD")==nullptr, "unknown WMI -> null");
  check(vinToProfileKey("WA")==nullptr, "short -> null");

  // vinAutoTarget: the decision.
  check(strcmp(vinAutoTarget("3GTUUEE80S2345678", true, ""),"gm_sierra_lz0")==0, "auto+changed -> key");
  check(vinAutoTarget("3GTUUEE80S2345678", true, "gm_sierra_lz0")==nullptr, "already current -> null");
  check(vinAutoTarget("3GTUUEE80S2345678", false, "")==nullptr, "auto off -> null");
  // Fail CLOSED, and fail QUIETLY: a VIN whose key is now nullptr (a gas 1500 on
  // a GM truck WMI) must leave the active profile exactly as it was, not reset it
  // to generic and not swap it. vinAutoTarget returning nullptr is what the caller
  // reads as "leave the current profile alone".
  check(vinAutoTarget("3GTUUEED0S2345678", true, "gm_sierra_lz0")==nullptr, "gas GM VIN -> leave current profile alone");
  check(vinAutoTarget("3GTUUEED0S2345678", true, "audi_q5")==nullptr, "gas GM VIN -> no change even from a foreign current");
  check(vinAutoTarget("JHM0123456789ABCD", true, "")==nullptr, "unmapped -> null");
  check(vinAutoTarget("", true, "gm_sierra_lz0")==nullptr, "empty vin -> null");
  check(strcmp(vinAutoTarget("WA1ANAFY012345678", true, "gm_sierra_lz0"),"audi_q5")==0, "audi VIN + auto + different current -> key");

  // ---- vinIdentify: naming a vehicle we cannot profile --------------------
  // The point of separating identity from profile selection: the dash should
  // say "Ford F-250 / 6.7L Power Stroke" while still running Generic gauges,
  // rather than showing nothing because no profile exists.
  const VinIdentity* id = vinIdentify("1FT7W2BT0N2345678");
  check(id != nullptr, "F-250 VIN identifies");
  check(id && id->profileKey == nullptr, "F-250 has NO profile key (identify-only)");
  check(id && strcmp(id->name, "Ford F-250")==0, "F-250 name");
  check(id && strcmp(id->engine, "6.7L Power Stroke")==0, "F-250 engine");
  check(vinToProfileKey("1FT7W2BT0N2345678")==nullptr, "F-250 still selects no profile");

  const VinIdentity* id350 = vinIdentify("1FT8W3BT0N2345678");
  check(id350 && strcmp(id350->name, "Ford F-350")==0, "vin[3]=8 -> F-350, not F-250");
  const VinIdentity* idmx = vinIdentify("3FT7W2BT0N2345678");
  check(idmx && strcmp(idmx->name, "Ford F-250")==0, "3FT (Mexico-built) also identifies");

  // Fail closed: the Super Duty predicate must not swallow the whole 1FT WMI.
  check(vinIdentify("1FT7X2BT0N2345678")==nullptr, "1FT, vin[4]!='W' -> unidentified");
  check(vinIdentify("1FT7W2B90N2345678")==nullptr, "1FT Super Duty frame, non-6.7 engine -> unidentified");
  check(vinIdentify("1FT9W2BT0N2345678")==nullptr, "1FT, vin[3] not 7/8 -> unidentified");

  // A profiled vehicle carries its key and needs no display name -- the
  // profile itself supplies one.
  const VinIdentity* gm = vinIdentify("3GTUUEE80S2345678");
  check(gm && gm->profileKey && strcmp(gm->profileKey,"gm_sierra_lz0")==0, "GM identity carries the profile key");

  // Two rows share the 1FT WMI and differ only in their predicate. A WMI match
  // with a failing predicate must fall through to the next row, not stop the
  // search -- otherwise whichever row is listed first wins for both trucks.
  check(vinIdentify("1FT8W3BT0N2345678")!=nullptr, "second row under a shared WMI is reachable");

  // ---- vinDisplayIdentity: what the splash should remember ----------------
  // Only identify-only vehicles get a stored name. A PROFILED vehicle must
  // return null so the caller CLEARS the stored strings -- otherwise moving the
  // dash from a Ford to the Sierra would leave "Ford F-250" on the splash of a
  // truck running the GM profile.
  check(vinDisplayIdentity("1FT7W2BT0N2345678")!=nullptr, "unprofiled Ford -> store its name");
  check(vinDisplayIdentity("3GTUUEE80S2345678")==nullptr, "profiled GM -> store nothing");
  check(vinDisplayIdentity("JHM0123456789ABCD")==nullptr, "unknown VIN -> store nothing");
  check(vinDisplayIdentity("")==nullptr, "empty VIN -> store nothing");
  const VinIdentity* d = vinDisplayIdentity("1FT8W3BT0N2345678");
  check(d && strcmp(d->name,"Ford F-350")==0, "stored name is the identified one");

  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("test_vin: ALL PASS\n"); return 0;
}
