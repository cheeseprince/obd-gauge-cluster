// test_profile_audi: Audi Q5 skeleton invariants + the three enhanced decodes.
// Links the Audi profile instead of GM/BMW/Generic. Mirrors test_profile_bmw.cpp
// (closest analog: gasoline skeleton) with Audi's own active-PID set and
// standard 7E0/7E1 addressing (no BMW-style CEA/CRA dance).
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

extern const VehicleProfile AUDI_Q5_PROFILE;

int main() {
  g_activeProfile = &AUDI_Q5_PROFILE;
  assert(READOUT_COUNT == (int)StatId::COUNT);
  for (int i = 0; i < READOUT_COUNT; i++) {
    assert(READOUTS[i].decode != nullptr);
    if (READOUTS[i].flags & RF_COMPUTED) assert(READOUTS[i].cmd == nullptr);
    assert(READOUTS[i].header < VEHICLE.addressingCount);
  }

  // The three active enhanced tiles: Trans@7E1, Oil@7E0, Boost@7E0.
  assert(READOUTS[(int)StatId::Trans].cmd != nullptr);
  assert(READOUTS[(int)StatId::Trans].header == 1);
  assert(READOUTS[(int)StatId::Oil].cmd != nullptr);
  assert(READOUTS[(int)StatId::Oil].header == 0);
  assert(READOUTS[(int)StatId::Boost].cmd != nullptr);
  assert(READOUTS[(int)StatId::Boost].header == 0);

  // Standard Mode-01 tiles are active (all header 0, all "0x.." commands).
  for (StatId s : {StatId::Coolant, StatId::Volts, StatId::Intake, StatId::Rpm,
                    StatId::Speed, StatId::Load, StatId::FuelLevel, StatId::Maf,
                    StatId::Pedal}) {
    assert(READOUTS[(int)s].cmd != nullptr);
    assert(READOUTS[(int)s].header == 0);
  }

  // MAF is g/s, NOT the gph->L/h Flow conversion — must be Quantity::None so
  // Metric mode doesn't silently multiply it by 3.785.
  assert(READOUTS[(int)StatId::Maf].quantity == Quantity::None);

  // FUEL% (012F) must actually be scheduled — it has a cmd, so it must appear on
  // a page (or be a helper) or it never gets polled. Here it's on the AIR page.
  {
    bool fuelShown = false;
    for (int p = 0; p < readoutPageCount(); p++)
      for (int c = 0; c < 4; c++)
        if (readoutAt(p, c) == (int)StatId::FuelLevel) fuelShown = true;
    assert(fuelShown);
  }

  // Diesel/DEF/NOx-only stats are inactive (gasoline skeleton).
  for (StatId s : {StatId::Egt, StatId::DpfDp, StatId::Def, StatId::Nox})
    assert(READOUTS[(int)s].cmd == nullptr);

  // Not-yet-identified scan targets are inactive.
  for (StatId s : {StatId::Baro, StatId::Ambient, StatId::OilP, StatId::Gear,
                    StatId::Rail, StatId::ActTq, StatId::RefTq})
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

  // Oil temp decode: 2220A1@7E0, raw*0.1 = degC. 0x03A2 = 930 -> 93.0C -> ~199.4F.
  {
    const uint8_t d[] = {0x03, 0xA2};
    DecodeCtx ctx{nullptr};
    float f = READOUTS[(int)StatId::Oil].decode(d, 2, ctx);
    assert(!std::isnan(f) && f > 195.0f && f < 205.0f);
  }
  // ATF temp decode: 222104@7E1, best-guess raw degC. 0x5C = 92 -> 92C -> ~197.6F.
  {
    const uint8_t d[] = {0x5C};
    DecodeCtx ctx{nullptr};
    float f = READOUTS[(int)StatId::Trans].decode(d, 1, ctx);
    assert(!std::isnan(f) && f > 190.0f && f < 205.0f);
  }
  // Boost decode: 22202A@7E0, 16-bit mbar absolute, gauge above a 1013 baseline,
  // clamped to 0 under vacuum (matches GM/BMW boostPsi()). 0x03EB = 1003 mbar
  // (below baseline) -> 0, not a negative reading.
  {
    const uint8_t atm[] = {0x03, 0xEB};
    DecodeCtx ctx{nullptr};
    float f = READOUTS[(int)StatId::Boost].decode(atm, 2, ctx);
    assert(f == 0.0f);
  }
  // Above baseline: 0x0640 = 1600 mbar -> (1600-1013)*0.014504 ~ 8.5 psi gauge.
  {
    const uint8_t boosted[] = {0x06, 0x40};
    DecodeCtx ctx{nullptr};
    float f = READOUTS[(int)StatId::Boost].decode(boosted, 2, ctx);
    assert(!std::isnan(f) && f > 5.0f && f < 12.0f);
  }
  // 0xFFFF sentinel -> NaN, not a bogus reading.
  {
    const uint8_t sentinel[] = {0xFF, 0xFF};
    DecodeCtx ctx{nullptr};
    float f = READOUTS[(int)StatId::Boost].decode(sentinel, 2, ctx);
    assert(std::isnan(f));
  }

  // Addressing: standard single-step AT SH per header, no CEA/CRA dance.
  {
    char b[24];
    assert(std::strcmp(VEHICLE.addressing[0].emit(0, false, b, sizeof b), "AT SH 7E0\r") == 0);
    assert(VEHICLE.addressing[0].emit(1, false, b, sizeof b) == nullptr);
    assert(std::strcmp(VEHICLE.addressing[1].emit(0, false, b, sizeof b), "AT SH 7E1\r") == 0);
    assert(VEHICLE.addressing[1].emit(1, false, b, sizeof b) == nullptr);
  }

  printf("test_profile_audi: ALL PASS\n");
  return 0;
}
