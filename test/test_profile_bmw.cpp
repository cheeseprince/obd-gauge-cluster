// test_profile_bmw: BMW profile invariants after the 2026-07-25 on-car scan.
// Links the BMW profile instead of GM. Generic structural checks (shared with
// test_profile) plus BMW specifics: the 6F1 physical path is gone (single 7DF
// header); oil pressure 22586F is the one active enhanced tile and is decoded
// byte-0-only so the 7DF NAK tail can't corrupt it; oil temp / ATF are stubbed
// (need a cold-start drive / gateway-blocked EGS); boost comes from MAP.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "../src/readouts.h"
#include "../src/app_types.h"
#include "../src/vehicle_profile.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile BMW_F10_535I_PROFILE;

int main() {
  g_activeProfile = &BMW_F10_535I_PROFILE;
  assert(READOUT_COUNT == STAT_COUNT);
  for (int i = 0; i < READOUT_COUNT; i++) {
    assert(READOUTS[i].decode != nullptr);
    if (READOUTS[i].flags & RF_COMPUTED) assert(READOUTS[i].cmd == nullptr);
    assert(READOUTS[i].header < VEHICLE.addressingCount);
  }
  // Only one reachable path on this car: the 7DF functional broadcast.
  assert(VEHICLE.addressingCount == 1);

  // Oil pressure (22586F) is the one active enhanced tile, on the sole header 0.
  assert(READOUTS[(int)StatId::OilP].cmd != nullptr);
  assert(std::strcmp(READOUTS[(int)StatId::OilP].cmd, "22586F") == 0);
  assert(READOUTS[(int)StatId::OilP].header == 0);
  // Boost comes from standard MAP (010B), not an enhanced DID.
  assert(std::strcmp(READOUTS[(int)StatId::Boost].cmd, "010B") == 0);
  assert(READOUTS[(int)StatId::Boost].header == 0);

  // Oil temp and ATF are STUBBED post-scan: DA25/DA12 @ 6F1/618 are silent
  // (gateway-blocked), and the 7DF temp candidates need a cold-start drive.
  assert(READOUTS[(int)StatId::Oil].cmd == nullptr);
  assert(READOUTS[(int)StatId::Trans].cmd == nullptr);
  // Diesel-only stats are inactive.
  for (StatId s : {StatId::Egt, StatId::DpfDp, StatId::Def, StatId::Nox})
    assert(READOUTS[(int)s].cmd == nullptr);

  // Layout references only supported stats; three named pages.
  for (int p = 0; p < readoutPageCount(); p++) {
    assert(pageName(p)[0] != '\0');
    for (int c = 0; c < 4; c++) {
      int idx = readoutAt(p, c);
      if (idx < 0) continue;
      assert(READOUTS[idx].cmd != nullptr || (READOUTS[idx].flags & RF_COMPUTED));
    }
  }
  assert(readoutPageCount() == 3);

  // Oil-pressure decode: byte 0 == psi (identity, UNVERIFIED scale). The
  // critical property is byte-0-only: on 7DF a second module appends "7F2222"
  // after the positive frame, so the decoder must read ONLY d[0] and ignore the
  // NAK tail. Same value with and without the tail proves it.
  {
    DecodeCtx ctx{nullptr};
    const uint8_t clean[]   = {0x0C};                    // 12
    const uint8_t withNak[] = {0x0C, 0x7F, 0x22, 0x22};  // 12 + appended NAK
    float a = READOUTS[(int)StatId::OilP].decode(clean, 1, ctx);
    float b = READOUTS[(int)StatId::OilP].decode(withNak, 4, ctx);
    assert(!std::isnan(a) && a == 12.0f);
    assert(a == b);                                      // NAK tail ignored
    const uint8_t bad[] = {0xFF};
    assert(std::isnan(READOUTS[(int)StatId::OilP].decode(bad, 1, ctx)));
  }
  // Boost decode: MAP 201 kPa vs 101.325 baseline -> ~14 psi gauge; vacuum
  // (40 kPa) clamps to 0 (boostPsi floors negative diff).
  {
    DecodeCtx ctx{nullptr};
    const uint8_t boosted[] = {0xC9};   // 201 kPa
    const uint8_t vac[]     = {0x28};   // 40 kPa
    float f = READOUTS[(int)StatId::Boost].decode(boosted, 1, ctx);
    assert(!std::isnan(f) && f > 10.0f && f < 20.0f);
    assert(READOUTS[(int)StatId::Boost].decode(vac, 1, ctx) == 0.0f);
    const uint8_t sentinel[] = {0xFF};   // ELM no-data pad -> NaN, not a bogus ~22 psi
    assert(std::isnan(READOUTS[(int)StatId::Boost].decode(sentinel, 1, ctx)));
  }
  // Addressing: a single header (7DF). Clear BOTH CEA and CRA before selecting
  // 7DF so no stale filter blocks the functional replies.
  {
    char b[24];
    assert(std::strcmp(VEHICLE.addressing[0].emit(0, false, b, sizeof b), "AT CEA\r") == 0);
    assert(std::strcmp(VEHICLE.addressing[0].emit(1, false, b, sizeof b), "AT CRA\r") == 0);
    assert(std::strcmp(VEHICLE.addressing[0].emit(2, false, b, sizeof b), "AT SH 7DF\r") == 0);
    assert(VEHICLE.addressing[0].emit(3, false, b, sizeof b) == nullptr);
  }
  printf("test_profile_bmw: ALL PASS\n");
  return 0;
}
