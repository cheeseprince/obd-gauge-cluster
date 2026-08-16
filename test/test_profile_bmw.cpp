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

  // --- Oil pressure is a 16-bit millibar value, NOT byte 0 -----------------
  //
  // 22586F was decoded byte-0-only on the theory that a second module NAKs
  // every 7DF Mode-22 read, making the payload [value, 0x7F, 0x22, 0x22].
  // The 2026-07-25 drive log refutes that for this DID: all 159 samples are
  // EXACTLY 2 bytes, NONE contains 7F2222, and the second byte is spread
  // across 0x87-0xF9 — the low byte of a 16-bit value, not a fixed NAK.
  //
  // The original reasoning failed in an instructive way: the HIGH BYTE of a
  // rising u16 also rises monotonically with RPM, so "shape confirmed by the
  // RPM correlation" looked right while the magnitude was wrong ~4x.
  //
  // Payloads below are verbatim from obd-display/bmw_drive.csv.
  {
    auto oilp = READOUTS[(int)StatId::OilP].decode;
    DecodeCtx ctx{nullptr};
    auto psi = [&](uint16_t raw) {
      const uint8_t d[2] = {(uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF)};
      return oilp(d, 2, ctx);
    };
    // Observed span of the drive: 2324 mbar (idle) .. 4776 mbar (loaded).
    assert(std::fabs(psi(0x0914) - 33.71f) < 0.2f);   // 2324 mbar -> 33.7 psi
    assert(std::fabs(psi(0x12A8) - 69.27f) < 0.2f);   // 4776 mbar -> 69.3 psi
    // A byte-0 decode would have called the idle sample 9 psi. Anything under
    // 20 psi on a running engine is an oil-pressure-warning condition, so this
    // is the assertion that would have caught the original bug.
    assert(psi(0x0914) > 25.0f);

    // 0xFF in the HIGH byte was the old sentinel check; a 16-bit read must not
    // inherit it, because 0xFFxx is simply an out-of-range pressure. Reject the
    // all-ones frame instead.
    assert(std::isnan(psi(0xFFFF)));
    // Short frame: one byte can no longer be decoded at all.
    { const uint8_t d[1] = {0x09}; assert(std::isnan(oilp(d, 1, ctx))); }
  }

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

  // Oil-pressure NAK tolerance, against the SHAPES ACTUALLY OBSERVED.
  //
  // The old version of this test asserted [0x0C, 0x7F, 0x22, 0x22] — a ONE-byte
  // value followed by the NAK — and that shape never occurs. The real value is
  // two bytes. Verbatim evidence:
  //
  //   sweep.json  "raw": "62586F03FF\r7F2222\r\r"   (NAK on its own line)
  //   drive log   62586F09B6                        (159 samples, no NAK)
  //
  // parseObdResponse does NOT strip the NAK when it shares the buffer — it
  // returns [03 FF 7F 22 22]. So the decoder must take the FIRST TWO bytes and
  // ignore anything after, which is what makes it tolerant of both shapes.
  {
    DecodeCtx ctx{nullptr};
    auto dec = READOUTS[(int)StatId::OilP].decode;
    const uint8_t clean[]   = {0x03, 0xFF};                    // drive-log shape
    const uint8_t withNak[] = {0x03, 0xFF, 0x7F, 0x22, 0x22};  // parser output
    float a = dec(clean, 2, ctx);
    float b = dec(withNak, 5, ctx);
    assert(!std::isnan(a));
    assert(a == b);                                    // NAK tail ignored
    assert(std::fabs(a - 14.84f) < 0.05f);             // 1023 mbar
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
