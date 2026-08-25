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
#include "../src/pid_decode.h"
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

  // OIL TEMP is now ACTIVE on 224402@7DF (probe, 2026-08-23). Block 0x2244 had
  // never been swept, so the candidate BMW-STATUS.md itself names was never in a
  // candidate list. ATF stays stubbed — the EGS is still gateway-blocked.
  assert(strcmp(READOUTS[(int)StatId::Oil].cmd, "224402") == 0);
  assert(READOUTS[(int)StatId::Trans].cmd == nullptr);

  // Decode against the ACTUAL probe sample, taken at warm idle with the
  // legislated coolant PID reading 99 C at the same moment.
  {
    DecodeCtx ctx{nullptr};
    auto oilt = READOUTS[(int)StatId::Oil].decode;
    const uint8_t d[2] = {0x00, 0xBA};                 // probe: 62440200BA
    const float f = oilt(d, 2, ctx);
    assert(std::fabs(f - 196.7f) < 0.5f);              // 186*0.75-48 = 91.5 C
    // Oil must read BELOW coolant at warm idle. Coolant was 99 C = 210.2 F.
    // A decode that put oil ABOVE coolant at idle would be the wrong scale.
    assert(f < 210.2f);
    // Same NAK tolerance as oil pressure: take the first two bytes, ignore any
    // 7F2222 the parser leaves in the buffer.
    const uint8_t withNak[5] = {0x00, 0xBA, 0x7F, 0x22, 0x22};
    assert(oilt(withNak, 5, ctx) == f);
    assert(std::isnan(oilt(d, 1, ctx)));               // short frame
  }

  // TORQUE — 2258BA, identified by BEHAVIOUR on the 2026-08-24 drive and named in
  // none of the six community BMW engine tables.
  {
    DecodeCtx ctx{nullptr};
    auto tq  = READOUTS[(int)StatId::ActTq].decode;
    auto ref = READOUTS[(int)StatId::RefTq].decode;
    assert(strcmp(READOUTS[(int)StatId::ActTq].cmd, "2258BA") == 0);
    assert(strcmp(READOUTS[(int)StatId::RefTq].cmd, "224517") == 0);

    // Reference torque was CONSTANT 4013 across all 486 rows of the drive.
    const uint8_t r[2] = {0x0F, 0xAD};                       // 4013
    assert(std::fabs(ref(r, 2, ctx) - 501.6f) < 0.5f);       // x0.125 Nm/count

    // Peak actual torque seen: 3087 at 3800 rpm = 76.9% of reference.
    const uint8_t pk[2] = {0x0C, 0x0F};
    assert(std::fabs(tq(pk, 2, ctx) - 76.9f) < 0.3f);
    assert(tq(pk, 2, ctx) < 100.0f);       // actual must not exceed the reference

    // ZERO ON OVERRUN is the identifying behaviour: 168 rows read exactly 0 while
    // moving at 22-119 km/h on 7-18% load. A decode unable to return 0 here would
    // destroy the one signature separating torque from engine load.
    const uint8_t zero[2] = {0x00, 0x00};
    assert(tq(zero, 2, ctx) == 0.0f);

    // Horsepower must reconcile with the figure measured from VEHICLE ACCELERATION:
    // raw 2861 at 5940 rpm gave 291 hp from F=ma on the same drive.
    const uint8_t s2[2] = {0x0B, 0x2D};                      // 2861
    const float hp = computeHorsepower(tq(s2, 2, ctx), 501.6f, 5940.0f);
    assert(hp > 270.0f && hp < 320.0f);
  }

  // The legislated rows added from the same probe, each against its sample.
  {
    DecodeCtx ctx{nullptr};
    const uint8_t rail[2] = {0x02, 0xE6};              // probe: 412302E6
    assert(std::fabs(READOUTS[(int)StatId::Rail].decode(rail, 2, ctx) - 1076.2f) < 1.0f);
    const uint8_t maf[2] = {0x02, 0xCC};               // probe: 411002CC
    assert(std::fabs(READOUTS[(int)StatId::Maf].decode(maf, 2, ctx) - 7.16f) < 0.01f);
    const uint8_t ped[1] = {0x23};                     // probe: 414923
    assert(std::fabs(READOUTS[(int)StatId::Pedal].decode(ped, 1, ctx) - 13.73f) < 0.05f);

    // FUEL RATE is DERIVED from MAF — this DME publishes neither 015E nor 019D,
    // and 015E is absent from its own supported-PID bitmap. Economy::update()
    // takes gal/hr, so that is what the decoder must return.
    const float gph = READOUTS[(int)StatId::FuelRate].decode(maf, 2, ctx);
    assert(std::fabs(gph - 0.62f) < 0.02f);            // 7.16 g/s air -> 0.62 gal/hr
    // Sanity: a 3.0 L six at idle burns well under 2 gal/hr. A decoder that
    // returned air mass instead of fuel mass would be ~14.7x this.
    assert(gph > 0.1f && gph < 2.0f);
  }
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
  assert(readoutPageCount() == 6);   // + TRIP, FLUIDS & FUEL, POWER

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
