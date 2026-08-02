// test_profile_jeep: Jeep Wagoneer WS skeleton invariants + the two enhanced
// decodes. Links the Jeep profile instead of GM/BMW/Audi/Generic. Mirrors
// test_profile_audi.cpp (closest analog: gasoline skeleton), with two things
// that are unique to this platform and are the real point of the file:
//   1. 29-bit addressing emitted UNCONDITIONALLY (can29 ignored), because the
//      Wagoneer's 11-bit path is measured dead and no caller passes can29=true.
//   2. A three-byte ATF reply where the Grand Cherokee signalset implies one.
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

extern const VehicleProfile JEEP_WS_PROFILE;

int main() {
  g_activeProfile = &JEEP_WS_PROFILE;
  assert(READOUT_COUNT == (int)StatId::COUNT);
  for (int i = 0; i < READOUT_COUNT; i++) {
    assert(READOUTS[i].decode != nullptr);
    if (READOUTS[i].flags & RF_COMPUTED) assert(READOUTS[i].cmd == nullptr);
    assert(READOUTS[i].header < VEHICLE.addressingCount);
  }

  // The two active enhanced tiles both live on the TCM (header 1), not the
  // functional broadcast. Getting this backwards is the single most likely way
  // to break this profile, since the census proved the TCM is only reachable at
  // 18DA18F1.
  assert(READOUTS[(int)StatId::Trans].cmd != nullptr);
  assert(std::strcmp(READOUTS[(int)StatId::Trans].cmd, "2204FE") == 0);
  assert(READOUTS[(int)StatId::Trans].header == 1);
  assert(READOUTS[(int)StatId::Gear].cmd != nullptr);
  assert(std::strcmp(READOUTS[(int)StatId::Gear].cmd, "22051A") == 0);
  assert(READOUTS[(int)StatId::Gear].header == 1);

  // Standard Mode-01 tiles confirmed live in the census bitmask (53 PIDs on
  // 18DB33F1). All header 0, all "01xx" commands.
  for (StatId s : {StatId::Coolant, StatId::Volts, StatId::Intake, StatId::Rpm,
                    StatId::Speed, StatId::Load, StatId::FuelLevel, StatId::Pedal,
                    StatId::Baro, StatId::Ambient, StatId::FuelRate,
                    StatId::ActTq, StatId::RefTq}) {
    assert(READOUTS[(int)s].cmd != nullptr);
    assert(READOUTS[(int)s].header == 0);
    assert(READOUTS[(int)s].cmd[0] == '0' && READOUTS[(int)s].cmd[1] == '1');
  }

  // MEASURED NEGATIVES. These are not "not identified yet" -- the census
  // bitmask positively showed them absent, so wiring a command here would poll
  // something the vehicle does not answer.
  //   MAF 0110  -> PID 0x10 absent (the Hemi is speed-density; 0x0B is present)
  //   OIL 015C  -> PID 0x5C absent (an earlier preset note wrongly claimed it)
  assert(READOUTS[(int)StatId::Maf].cmd == nullptr);
  assert(READOUTS[(int)StatId::Oil].cmd == nullptr);

  // Naturally aspirated gasoline V8: no boost, no charge-air cooler, and none
  // of the diesel aftertreatment stats.
  for (StatId s : {StatId::Boost, StatId::Cac, StatId::Egt, StatId::DpfDp,
                    StatId::Def, StatId::Nox})
    assert(READOUTS[(int)s].cmd == nullptr);

  // Every scheduled command must be reachable: a cmd with no page and no helper
  // slot is never polled. With the POWER page added there are NO exemptions --
  // every readout carrying a command is displayed somewhere.
  {
    bool shown[(int)StatId::COUNT] = {false};
    for (int p = 0; p < readoutPageCount(); p++)
      for (int c = 0; c < 4; c++) {
        int idx = readoutAt(p, c);
        if (idx >= 0) shown[idx] = true;
      }
    for (int i = 0; i < READOUT_COUNT; i++) {
      if (READOUTS[i].cmd == nullptr) continue;
      assert(shown[i] && "readout has a cmd but is on no page -- it would never be polled");
    }

    // HP is RF_COMPUTED, so it has no command of its own and is only meaningful
    // if its three inputs are actually polled. updateComputedReadouts() reads
    // ActTq * RefTq * Rpm straight out of values[], which stay NaN unless those
    // rows are scheduled -- i.e. unless they sit on a page. Assert that link
    // explicitly: shipping an HP tile fed by unscheduled inputs would display a
    // permanently blank gauge.
    assert(shown[(int)StatId::Hp]);
    assert(shown[(int)StatId::ActTq] && shown[(int)StatId::RefTq] && shown[(int)StatId::Rpm]);
    // Same argument for the Economy integrator behind the computed MPG rows:
    // it consumes FuelRate + Speed.
    assert(shown[(int)StatId::FuelRate] && shown[(int)StatId::Speed]);
  }

  // Layout references only supported stats; four named pages.
  for (int p = 0; p < readoutPageCount(); p++) {
    assert(pageName(p)[0] != '\0');
    for (int c = 0; c < 4; c++) {
      int idx = readoutAt(p, c);
      if (idx < 0) continue;
      assert(READOUTS[idx].cmd != nullptr || (READOUTS[idx].flags & RF_COMPUTED));
    }
  }
  assert(readoutPageCount() == 4);          // TEMPS / DRIVE / POWER / MISC

  // --- ATF temp decode: 2204FE@DA18. The reply is THREE bytes (6F 75 76);
  // byte A is the wired candidate, A-40 degC. 0x6F = 111 -> 71C -> ~159.8F.
  // The extra bytes must not change the answer or trip the decoder.
  {
    const uint8_t d[] = {0x6F, 0x75, 0x76};           // the exact on-car reply
    DecodeCtx ctx{nullptr};
    float f = READOUTS[(int)StatId::Trans].decode(d, 3, ctx);
    assert(!std::isnan(f) && f > 155.0f && f < 165.0f);
    // Same first byte, truncated reply -> identical result (byte A only).
    float f1 = READOUTS[(int)StatId::Trans].decode(d, 1, ctx);
    assert(f == f1);
  }
  // 0xFF sentinel -> NaN, not -40C.
  {
    const uint8_t d[] = {0xFF, 0x75, 0x76};
    DecodeCtx ctx{nullptr};
    assert(std::isnan(READOUTS[(int)StatId::Trans].decode(d, 3, ctx)));
  }
  // Empty reply -> NaN, no out-of-bounds read.
  {
    DecodeCtx ctx{nullptr};
    assert(std::isnan(READOUTS[(int)StatId::Trans].decode(nullptr, 0, ctx)));
  }

  // --- Gear decode: 22051A@DA18, RAW byte until a full P-R-N-D log maps the
  // enum. 0xDD was Park on both the 2024 capture and this scan.
  {
    const uint8_t d[] = {0xDD};
    DecodeCtx ctx{nullptr};
    float g = READOUTS[(int)StatId::Gear].decode(d, 1, ctx);
    assert(g == 221.0f);                              // 0xDD raw, NOT a decoded gear number
    assert(std::isnan(READOUTS[(int)StatId::Gear].decode(nullptr, 0, ctx)));
  }

  // --- Addressing. THE point of this profile: 29-bit headers emitted
  // unconditionally, in two steps, and IDENTICAL for can29 false and true.
  // A GM-style `can29 ? 29bit : 11bit` ternary here would silently emit 7DF --
  // which this vehicle does not answer -- because nothing passes can29=true.
  {
    char b[24];
    for (bool can29 : {false, true}) {
      assert(std::strcmp(VEHICLE.addressing[0].emit(0, can29, b, sizeof b), "AT CP 18\r") == 0);
      assert(std::strcmp(VEHICLE.addressing[0].emit(1, can29, b, sizeof b), "AT SH DB33F1\r") == 0);
      assert(VEHICLE.addressing[0].emit(2, can29, b, sizeof b) == nullptr);
      assert(std::strcmp(VEHICLE.addressing[1].emit(0, can29, b, sizeof b), "AT CP 18\r") == 0);
      assert(std::strcmp(VEHICLE.addressing[1].emit(1, can29, b, sizeof b), "AT SH DA18F1\r") == 0);
      assert(VEHICLE.addressing[1].emit(2, can29, b, sizeof b) == nullptr);
    }
    // No 11-bit header may appear anywhere in this profile's addressing.
    for (int h = 0; h < VEHICLE.addressingCount; h++)
      for (int step = 0; step < 4; step++) {
        const char* s = VEHICLE.addressing[h].emit(step, false, b, sizeof b);
        if (!s) break;
        assert(std::strstr(s, "7DF") == nullptr);
        assert(std::strstr(s, "7E0") == nullptr);
        assert(std::strstr(s, "7E1") == nullptr);
      }
  }

  printf("test_profile_jeep: ALL PASS\n");
  return 0;
}
