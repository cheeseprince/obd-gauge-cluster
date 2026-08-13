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
  check(strcmp(vinToProfileKey("3GTUUEED0S2345678"),"std_gas")==0, "L84 5.3 gas -> Standard+ gas, NOT the Duramax profile");
  check(strcmp(vinToProfileKey("3GTUUEEL0S2345678"),"std_gas")==0, "L87 6.2 gas -> Standard+ gas");
  check(strcmp(vinToProfileKey("3GTUUEEK0S2345678"),"std_gas")==0, "L3B 2.7 gas -> Standard+ gas");
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
  // A gas 1500 on a GM truck WMI USED to return nullptr -- "we cannot tell what
  // this is, so leave the active profile alone". That rationale is gone: we can
  // now tell exactly what it is, and Standard+ gas is the right home for it. So
  // the correct behaviour flipped from "leave alone" to "switch", and switching
  // is the safer of the two -- it takes a gas truck OFF a Duramax profile whose
  // enhanced tiles it can never populate.
  check(strcmp(vinAutoTarget("3GTUUEED0S2345678", true, "gm_sierra_lz0"),"std_gas")==0,
        "gas GM VIN moves OFF the Duramax profile to Standard+ gas");
  check(strcmp(vinAutoTarget("3GTUUEED0S2345678", true, "audi_q5"),"std_gas")==0,
        "gas GM VIN switches even from a foreign current profile");
  check(vinAutoTarget("3GTUUEED0S2345678", true, "std_gas")==nullptr,
        "already on Standard+ gas -> no reboot loop");
  check(vinAutoTarget("JHM0123456789ABCD", true, "")==nullptr, "unmapped -> null");
  check(vinAutoTarget("", true, "gm_sierra_lz0")==nullptr, "empty vin -> null");
  check(strcmp(vinAutoTarget("WA1ANAFY012345678", true, "gm_sierra_lz0"),"audi_q5")==0, "audi VIN + auto + different current -> key");

  // ---- vinIdentify: naming a vehicle we cannot profile --------------------
  // The point of separating identity from profile selection: the dash should
  // say "Ford F-250 / 6.7L Power Stroke" while still running Generic gauges,
  // rather than showing nothing because no profile exists.
  VinIdentity id{};
  check(vinIdentify("1FT7W2BT0N2345678", &id), "F-250 VIN identifies");
  // SCANNED 2026-08-09 (a 2021 F-350): the 6.7 Power Stroke now gets a real
  // vehicle profile, not Standard+. The identity fields are unchanged -- the
  // identification table still resolves series and engine independently.
  check(id.profileKey && strcmp(id.profileKey,"ford_sd_67")==0, "F-250 6.7 -> Ford profile");
  check(strcmp(id.name, "Ford F-250")==0, "F-250 name");
  check(strcmp(id.engine, "6.7L Power Stroke")==0, "F-250 engine");
  check(strcmp(vinToProfileKey("1FT7W2BT0N2345678"),"ford_sd_67")==0, "F-250 selects the Ford profile");

  // A failed identify must not scribble on the caller's struct -- callers copy
  // out of it and a partial write would leak the previous vehicle's name.
  VinIdentity keep{ (const char*)1, "PREV", "PREVENG" };
  check(!vinIdentify("JHM0123456789ABCD", &keep), "unknown WMI -> false");
  check(strcmp(keep.name,"PREV")==0 && strcmp(keep.engine,"PREVENG")==0,
        "failed identify leaves *out untouched");

  // ---- Ford: series at vin[5], engine at vin[7] ---------------------------
  // vin[5] is the series digit (Ford's published VDS layout, position 6
  // 1-indexed) and vin[3] is NOT -- an earlier version of this table read the
  // series from vin[3] and gated on vin[4]/vin[6], which encode cab and drive
  // type, so it rejected every 4x2 Super Duty and every other cab style.
  struct FordCase { const char* vin; const char* name; const char* engine; };
  static const FordCase FORD[] = {
    {"1FT7W2BT0N2345678", "Ford F-250", "6.7L Power Stroke"},
    {"1FT8W3BT0N2345678", "Ford F-350", "6.7L Power Stroke"},
    {"1FTAA4AT0N2345678", "Ford F-450", "6.7L Power Stroke"},   // vin[5]='4'
    {"1FTAA5AT0N2345678", "Ford F-550", "6.7L Power Stroke"},   // vin[5]='5'
    {"3FT7W2BT0N2345678", "Ford F-250", "6.7L Power Stroke"},   // Mexico-built
    {"1FT7A2A60N2345678", "Ford F-250", "6.2L V8"},             // vin[7]='6'
    {"1FT7A2AN0N2345678", "Ford F-250", "7.3L V8"},             // vin[7]='N'
    // Cab and drive must NOT be gated: 4x2 and every cab style are real trucks.
    {"1FTAA2AT0N2345678", "Ford F-250", "6.7L Power Stroke"},   // vin[4],vin[6] arbitrary
    {"1FTZZ3ZT0N2345678", "Ford F-350", "6.7L Power Stroke"},
  };
  for (const auto& c : FORD) {
    VinIdentity f{};
    check(vinIdentify(c.vin, &f) && strcmp(f.name, c.name)==0, c.name);
    check(strcmp(f.engine, c.engine)==0, c.engine);
  }

  // ---- Ford PROFILE gate: cab style and axle must not gate it --------------
  // The identification table already refuses to gate cab/drive (above), but the
  // PROFILE gate did, and nothing tested it: fordSuperDuty67 required
  // vin[4]=='W' and vin[6]=='B', which is Crew Cab / single-rear-wheel / 4WD.
  // A vPIC 2-D sweep (2026-08-13) found 24 valid (vin[4],vin[6]) pairs for a
  // 6.7L Super Duty; that gate accepted 1 of the 24, so every Regular Cab,
  // SuperCab, dually and 4x2 silently fell through to Standard+.
  //
  //   vin[4] cab   F=Regular  W=Crew  X=SuperCab
  //   vin[6] axle  A/E=SRW 4x2  B/F=SRW 4WD  C/G=DRW 4x2  D/H=DRW 4WD
  {
    static const char* const CABS = "FWX";
    static const char* const AXLES = "ABCDEFGH";
    char vin[18]; int accepted = 0;
    for (const char* cab = CABS; *cab; cab++) {
      for (const char* ax = AXLES; *ax; ax++) {
        // 1FT <cab> <series=3> <axle> T <chk> <year=N> ...  -> 2022 F-350 6.7L
        std::snprintf(vin, sizeof vin, "1FT8%c3%cT0N2345678", *cab, *ax);
        const char* key = vinToProfileKey(vin);
        if (key && strcmp(key, "ford_sd_67") == 0) accepted++;
      }
    }
    check(accepted == 24, "every cab/axle combination gets the Ford profile (24 of 24)");
  }

  // Every Super Duty series takes the profile; the F-150 must not.
  check(strcmp(vinToProfileKey("1FT8F2AT0N2345678"),"ford_sd_67")==0, "F-250 RegCab 4x2 -> Ford profile");
  check(strcmp(vinToProfileKey("1FT8X4HT0N2345678"),"ford_sd_67")==0, "F-450 SuperCab dually 4WD -> Ford profile");
  check(strcmp(vinToProfileKey("1FT8W5DT0N2345678"),"ford_sd_67")==0, "F-550 Crew dually 4WD -> Ford profile");
  // vin[7]='T' on an F-150 is a 3.5L EcoBoost, NOT a Power Stroke — the series
  // digit is the only thing keeping this profile off that truck.
  check(vinToProfileKey("1FTFW1ET0L2345678")==nullptr ||
        strcmp(vinToProfileKey("1FTFW1ET0L2345678"),"ford_sd_67")!=0, "F-150 never gets the Ford profile");
  // Gas Super Dutys read no 10R140 DIDs; they belong on Standard+.
  check(vinToProfileKey("1FT8W3B60N2345678")==nullptr ||
        strcmp(vinToProfileKey("1FT8W3B60N2345678"),"ford_sd_67")!=0, "6.2L V8 never gets the Ford profile");
  check(vinToProfileKey("1FT8W3BN0N2345678")==nullptr ||
        strcmp(vinToProfileKey("1FT8W3BN0N2345678"),"ford_sd_67")!=0, "7.3L V8 never gets the Ford profile");
  // F-600 (vin[5]='6') also takes the 6.7L but is a Class-6 chassis cab that
  // has never been scanned — fails closed on purpose.
  check(vinToProfileKey("1FT8W6BT0N2345678")==nullptr ||
        strcmp(vinToProfileKey("1FT8W6BT0N2345678"),"ford_sd_67")!=0, "F-600 fails closed");
  // The 6R140/10R140 break still holds: a 2019 truck must not get this profile.
  check(vinToProfileKey("1FT8W3BT0K2345678")==nullptr ||
        strcmp(vinToProfileKey("1FT8W3BT0K2345678"),"ford_sd_67")!=0, "2019 (6R140) still excluded");

  // F-150 is named but its ENGINE is deliberately left blank: vin[7]='T' is a
  // 3.5L EcoBoost on an F-150 and a 6.7L Power Stroke on a Super Duty, so the
  // engine codes are not shared between the two lines and only the Super Duty
  // set was verified across its whole span.
  VinIdentity f150{};
  check(vinIdentify("1FTFW1ET0L2345678", &f150), "F-150 identifies");
  check(strcmp(f150.name, "Ford F-150")==0, "F-150 name");
  check(f150.engine[0] == '\0', "F-150 engine deliberately blank");

  // ---- Ram: brand at vin[4], series at vin[5] -----------------------------
  // 1C6 is shared with Jeep, so vin[4]='R' is the brand gate; without it a
  // Grand Cherokee would be captioned as a pickup.
  struct RamCase { const char* vin; const char* name; const char* engine; };
  static const RamCase RAM[] = {
    {"1C6SR6FT0L2345678", "Ram 1500", "5.7L HEMI V8"},
    {"1C6SR4FL0L2345678", "Ram 2500", "6.7L Cummins I6"},
    {"1C6SR2FL0L2345678", "Ram 3500", "6.7L Cummins I6"},
    {"3C6SR6FG0L2345678", "Ram 1500", "3.6L V6"},
    {"1C6SR6FJ0L2345678", "Ram 1500", "6.4L HEMI V8"},
  };
  for (const auto& c : RAM) {
    VinIdentity r{};
    check(vinIdentify(c.vin, &r) && strcmp(r.name, c.name)==0, c.name);
    check(strcmp(r.engine, c.engine)==0, c.engine);
  }
  check(!vinIdentify("1C6SJ6FT0L2345678", &id), "1C6 with vin[4]!='R' (Jeep) -> not a Ram");

  // ---- GM: series from vin[3]+vin[4], engine at vin[7] --------------------
  struct GmCase { const char* vin; const char* name; const char* engine; };
  static const GmCase GM[] = {
    {"1GTUUEED0S2345678", "GMC Sierra 1500",        "5.3L V8"},
    {"1GCUUEEK0S2345678", "Chevrolet Silverado 1500","2.7L I4 Turbo"},
    {"3GTUUEEL0S2345678", "GMC Sierra 1500",        "6.2L V8"},
    {"1GT49PEY0N2345678", "GMC Sierra HD",          "6.6L Duramax V8"},
    {"1GC49PE70N2345678", "Chevrolet Silverado HD", "6.6L V8"},
  };
  for (const auto& c : GM) {
    VinIdentity g{};
    check(vinIdentify(c.vin, &g) && strcmp(g.name, c.name)==0, c.name);
    check(strcmp(g.engine, c.engine)==0, c.engine);
  }

  // The profiled truck keeps its profile key AND gets named by the same call.
  VinIdentity gm{};
  check(vinIdentify("3GTUUEE80S2345678", &gm), "profiled GM identifies");
  check(gm.profileKey && strcmp(gm.profileKey,"gm_sierra_lz0")==0, "GM identity carries the profile key");

  // ---- Year gates: fail closed outside the VERIFIED span ------------------
  // Every span here is what vPIC actually confirmed, not what the truck was
  // sold in -- an unverified year is treated as unknown rather than assumed.
  check(!vinIdentify("1FT7W2BT0A2345678", &id), "Super Duty 2010 (code A) outside verified span");
  check(!vinIdentify("1C6SR6FT0C2345678", &id), "Ram 2012 (code C) outside verified span");
  check(!vinIdentify("1GT49PEY0S2345678", &id), "GM HD 2025 (code S) outside verified span");
  check( vinIdentify("1FT7W2BT0T2345678", &id), "Super Duty 2026 (code T) inside span");

  // ---- vinDisplayIdentity: what the splash should remember ----------------
  // Only identify-only vehicles get a stored name. A PROFILED vehicle must
  // return false so the caller CLEARS the stored strings -- otherwise moving
  // the dash from a Ford to the Sierra would leave "Ford F-250" on the splash
  // of a truck running the GM profile.
  VinIdentity d{};
  // The 6.7 Power Stroke is PROFILED now, so it must store nothing and let the
  // profile name the truck -- the same rule the GM already followed. A GAS
  // Super Duty is still identify-only and keeps its stored name.
  check(!vinDisplayIdentity("1FT7W2BT0N2345678", &d), "profiled Ford 6.7 -> store nothing");
  check(!vinDisplayIdentity("3GTUUEE80S2345678", &d), "profiled GM -> store nothing");
  check(!vinDisplayIdentity("JHM0123456789ABCD", &d), "unknown VIN -> store nothing");
  check(!vinDisplayIdentity("", &d), "empty VIN -> store nothing");
  check(vinDisplayIdentity("1FT7A2A60N2345678", &d) && strcmp(d.name,"Ford F-250")==0,
        "unprofiled gas Super Duty still stores its name");

  // ---- Standard+ selection: a richer profile for unscanned trucks ---------
  // Recognized-but-unscanned trucks get the legislated Mode-01 set instead of
  // bare Generic, chosen by the ENGINE code -- the same vin[7] the identity
  // lookup already reads. Diesel-only tiles on a gas truck would sit blank
  // forever, so the two layouts are separate.
  struct StdCase { const char* vin; const char* key; const char* why; };
  static const StdCase STD[] = {
    // NOTE: the 6.7 Power Stroke is no longer here -- it was SCANNED on
    // 2026-08-09 and now selects ford_sd_67. Its cases live in the Ford
    // profile-gate block below. A GAS Super Duty still lands on Standard+,
    // which is what the next two rows pin.
    {"1FT7A2A60N2345678", "std_gas",    "F-250 6.2 V8 -> gas"},
    {"1FT7A2AN0N2345678", "std_gas",    "F-250 7.3 V8 -> gas"},
    {"1FTFW1ET0L2345678", "std_gas",    "F-150 unknown engine -> gas fallback"},
    {"1C6SR4FL0L2345678", "std_diesel", "Ram 2500 Cummins -> diesel"},
    {"1C6SR6FT0L2345678", "std_gas",    "Ram 1500 HEMI -> gas"},
    {"1GTUUEED0S2345678", "std_gas",    "Sierra 1500 5.3 V8 -> gas"},
    {"1GT49PEY0N2345678", "std_diesel", "Sierra HD 6.6 Duramax -> diesel"},
    {"1GC49PE70N2345678", "std_gas",    "Silverado HD 6.6 gas -> gas"},
  };
  for (const auto& c : STD) {
    VinIdentity v{};
    check(vinIdentify(c.vin, &v) && v.profileKey && strcmp(v.profileKey, c.key)==0, c.why);
    const char* k = vinToProfileKey(c.vin);   // may be null -- guard, do not strcmp it
    check(k && strcmp(k, c.key)==0, "vinToProfileKey agrees");
  }

  // ---- Ford profile gate: the 10R140 years ONLY ---------------------------
  // ford_sd_67 decodes 221E60 as a TEN-position gear. The transmission
  // generation break is 2019->2020 (6R140 -> 10R140), so a pre-2020 Super Duty
  // must NOT get this profile even though the identity frame matches it at
  // every year code. Without the gate, profileKeyFor() is reached independently
  // of the identification table's year list and a 2010 truck was handed a
  // ten-speed decode for its six-speed.
  struct FordGateCase { const char* vin; const char* key; const char* why; };
  static const FordGateCase FORDS[] = {
    {"1FT7W2BT0L2345678", "ford_sd_67", "2020 (L) first 10R140 year -> Ford profile"},
    {"1FT8W3BT0M2345678", "ford_sd_67", "2021 (M) the scanned truck -> Ford profile"},
    {"1FT7W2BT0T2345678", "ford_sd_67", "2026 (T) end of verified span -> Ford profile"},
    // Below the span: a 2019 truck is a 6R140. It still IDENTIFIES (K is in the
    // identity table's year list) and is a diesel, so it lands on Standard+
    // Diesel -- the legislated set, no ten-speed gear decode. That is the
    // desired outcome, not a bare null.
    {"1FT7W2BT0K2345678", "std_diesel", "2019 (K) is a 6R140 -> Standard+, not the Ford profile"},
    // Outside the IDENTITY year list entirely -> nothing at all, both sides.
    {"1FT7W2BT0A2345678", nullptr,      "2010 (A) -> no profile"},
    {"1FT7W2BT0V2345678", nullptr,      "2027 (V) unverified -> fails closed"},
  };
  for (const auto& c : FORDS) {
    const char* k = vinToProfileKey(c.vin);
    if (c.key) check(k && strcmp(k, c.key)==0, c.why);
    else       check(k == nullptr, c.why);
  }
  // The F-450/F-550 share the platform and transmission, so one row covers the
  // whole line -- the predicate is deliberately silent on the series digit.
  check(vinToProfileKey("1FT9W4BT0M2345678") &&
        strcmp(vinToProfileKey("1FT9W4BT0M2345678"),"ford_sd_67")==0,
        "F-450 6.7 also gets the Ford profile");

  // A SCANNED profile always wins over Standard+ -- the Sierra 3.0 Duramax has
  // real enhanced parameters and must not be downgraded to the legislated set.
  check(strcmp(vinToProfileKey("3GTUUEE80S2345678"),"gm_sierra_lz0")==0,
        "scanned profile beats Standard+");

  // Standard+ does NOT name the vehicle (its label is "Standard+ Diesel"), so
  // the splash still needs the stored identity -- unlike a scanned profile,
  // which carries its own name.
  // (Uses a GAS Super Duty: the 6.7 diesel is profiled now and names itself.)
  VinIdentity sp{};
  check(vinDisplayIdentity("1FT7A2A60N2345678", &sp) && strcmp(sp.name,"Ford F-250")==0,
        "Standard+ truck still stores its name for the splash");
  check(!vinDisplayIdentity("3GTUUEE80S2345678", &sp),
        "scanned profile still stores nothing");


  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("test_vin: ALL PASS\n"); return 0;
}
