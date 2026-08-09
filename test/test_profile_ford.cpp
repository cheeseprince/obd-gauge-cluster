// test_profile_ford: 2021 F-350 6.7L Power Stroke profile invariants plus the
// two enhanced decodes confirmed on the 2026-08-09 scan. Mirrors
// test_profile_jeep.cpp / test_profile_audi.cpp. Three things are specific to
// this platform and are the real point of the file:
//   1. 11-bit addressing with TWO physical ECUs (7E0 ECM, 7E1 TCM). The census
//      found ALL FIFTEEN 29-bit headers silent -- the inverse of the Sierra --
//      so getting the header index wrong is the likeliest way to break this.
//   2. INTAKE is read at the TCM. 010F is absent from the ECM's supported-PID
//      bitmap but present in the TCM's, which is easy to "tidy" back to
//      header 0 and thereby silently blank the tile.
//   3. The ATF temperature MUST decode as SIGNED int16. The common unsigned
//      form reads ~4096 degC on a sub-zero cold start.
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

extern const VehicleProfile FORD_SD_67_PROFILE;

static bool near(float a, float b, float tol = 0.05f) { return std::fabs(a - b) <= tol; }

int main() {
  g_activeProfile = &FORD_SD_67_PROFILE;
  DecodeCtx ctx{};

  assert(READOUT_COUNT == (int)StatId::COUNT);
  for (int i = 0; i < READOUT_COUNT; i++) {
    assert(READOUTS[i].decode != nullptr);
    if (READOUTS[i].flags & RF_COMPUTED) assert(READOUTS[i].cmd == nullptr);
    assert(READOUTS[i].header < VEHICLE.addressingCount);
  }

  // --- the two CONFIRMED enhanced tiles, both on the TCM (header 1) ---------
  assert(READOUTS[(int)StatId::Trans].cmd != nullptr);
  assert(std::strcmp(READOUTS[(int)StatId::Trans].cmd, "221E1C") == 0);
  assert(READOUTS[(int)StatId::Trans].header == 1);
  assert(READOUTS[(int)StatId::Gear].cmd != nullptr);
  assert(std::strcmp(READOUTS[(int)StatId::Gear].cmd, "221E60") == 0);
  assert(READOUTS[(int)StatId::Gear].header == 1);

  // TRANS alarms are OFF. No 10R140 thresholds have been sourced, and the
  // Sierra's 240/260 belong to a 10L80 -- a different transmission. If someone
  // later fills these in, it must be from a source, not from the GM profile.
  {
    const Thresholds& t = READOUTS[(int)StatId::Trans].thr;
    assert(std::isnan(t.warnHi) && std::isnan(t.critHi));
    assert(std::isnan(t.warnLo)  && std::isnan(t.critLo));
  }

  // --- ATF temperature: degC = int16 / 16, displayed in degF ----------------
  // 0x01AD is the first sample of the real drive log: 429/16 = 26.8 degC.
  { const uint8_t d[] = {0x01, 0xAD};
    assert(near(READOUTS[(int)StatId::Trans].decode(d, 2, ctx), 80.26f)); }
  // 0x05AA is the last: 1450/16 = 90.6 degC. Together these bracket the
  // measured warm-up, so a scaling regression cannot pass both.
  { const uint8_t d[] = {0x05, 0xAA};
    assert(near(READOUTS[(int)StatId::Trans].decode(d, 2, ctx), 195.16f, 0.2f)); }
  // SIGNED. Unsigned would read 65336/16 = 4083 degC here instead of -12.5.
  { const uint8_t d[] = {0xFF, 0x38};
    assert(near(READOUTS[(int)StatId::Trans].decode(d, 2, ctx), 9.5f)); }
  // Dropout pad and short frames are NaN, never a reading.
  { const uint8_t d[] = {0xFF, 0xFF};
    assert(std::isnan(READOUTS[(int)StatId::Trans].decode(d, 2, ctx))); }
  { const uint8_t d[] = {0x01};
    assert(std::isnan(READOUTS[(int)StatId::Trans].decode(d, 1, ctx))); }

  // --- gear: raw byte, ten positions on a 10R140 ---------------------------
  { const uint8_t d[] = {0x01};
    assert(near(READOUTS[(int)StatId::Gear].decode(d, 1, ctx), 1.0f)); }
  { const uint8_t d[] = {0x0A};   // 10th -- the value that proves it is a ten-speed
    assert(near(READOUTS[(int)StatId::Gear].decode(d, 1, ctx), 10.0f)); }
  // Out of range is NOT a gear. 0x00 and 0x0B must not render as "0" or "11".
  for (uint8_t bad : {(uint8_t)0x00, (uint8_t)0x0B, (uint8_t)0xFF}) {
    const uint8_t d[] = {bad};
    assert(std::isnan(READOUTS[(int)StatId::Gear].decode(d, 1, ctx)));
  }

  // --- legislated tiles: present in the ECM bitmap, header 0, "01xx" --------
  for (StatId s : {StatId::Coolant, StatId::Volts, StatId::Rpm, StatId::Speed,
                    StatId::Load, StatId::Oil, StatId::Egt, StatId::FuelLevel,
                    StatId::ActTq, StatId::RefTq, StatId::Baro, StatId::Maf,
                    StatId::Ambient, StatId::Nox}) {
    assert(READOUTS[(int)s].cmd != nullptr);
    assert(READOUTS[(int)s].header == 0);
    assert(READOUTS[(int)s].cmd[0] == '0' && READOUTS[(int)s].cmd[1] == '1');
  }

  // INTAKE is the exception: 010F is ABSENT from the ECM's bitmap and PRESENT
  // in the TCM's, so it is read at header 1. This is measured, not a guess.
  assert(READOUTS[(int)StatId::Intake].cmd != nullptr);
  assert(std::strcmp(READOUTS[(int)StatId::Intake].cmd, "010F") == 0);
  assert(READOUTS[(int)StatId::Intake].header == 1);

  // MEASURED NEGATIVES. The census bitmap positively showed these absent on the
  // ECM, so wiring a command would poll something the truck does not answer.
  //   PEDAL 0111, RAIL 0123, EGR 012C, FUEL RATE 015E
  for (StatId s : {StatId::Pedal, StatId::Rail, StatId::Egr, StatId::FuelRate})
    assert(READOUTS[(int)s].cmd == nullptr);

  // DEF stays dark on purpose: 22F485 answered but returned a CONSTANT 10-byte
  // payload across all 64 samples, which is expected over 29 minutes and is
  // therefore not enough to say which byte carries level. The Sierra's byte[1]
  // is concentration and reads stuck while byte[3] is the real level -- a wrong
  // guess here would drive a low-DEF alarm.
  assert(READOUTS[(int)StatId::Def].cmd == nullptr);
  // DPF differential pressure: 22116C is ABSENT from the sweep entirely. The
  // community source for it published a blank equation.
  assert(READOUTS[(int)StatId::DpfDp].cmd == nullptr);
  // Oil pressure was swept for and never identified on this truck.
  assert(READOUTS[(int)StatId::OilP].cmd == nullptr);

  // --- addressing: plain 11-bit, one AT SH per ECU, no 29-bit path ----------
  assert(VEHICLE.addressingCount == 2);
  { char buf[32];
    const char* ecm = VEHICLE.addressing[0].emit(0, false, buf, sizeof buf);
    const char* tcm = VEHICLE.addressing[1].emit(0, false, buf, sizeof buf);
    assert(ecm && std::strstr(ecm, "7E0"));
    assert(tcm && std::strstr(tcm, "7E1"));
    // Single-step: nothing to emit after step 0 (no BMW-style CEA/CRA dance).
    assert(VEHICLE.addressing[0].emit(1, false, buf, sizeof buf) == nullptr);
    assert(VEHICLE.addressing[1].emit(1, false, buf, sizeof buf) == nullptr);
    // can29 must not change anything -- every 29-bit header was silent.
    const char* ecm29 = VEHICLE.addressing[0].emit(0, true, buf, sizeof buf);
    assert(ecm29 && std::strstr(ecm29, "7E0"));
  }

  // --- every scheduled command must actually be reachable ------------------
  // A cmd with no page and no helper slot is never polled, so it is a silently
  // dead row rather than a feature.
  {
    bool shown[(int)StatId::COUNT] = {false};
    for (int p = 0; p < readoutPageCount(); p++)
      for (int c = 0; c < 4; c++) {
        int idx = readoutAt(p, c);
        if (idx >= 0) shown[idx] = true;
      }
    for (int h = 0; h < VEHICLE.defaultLayout.helperCount; h++)
      shown[(int)VEHICLE.defaultLayout.helpers[h]] = true;
    for (int i = 0; i < READOUT_COUNT; i++) {
      if (READOUTS[i].cmd == nullptr) continue;
      assert(shown[i] && "readout has a cmd but is on no page or helper -- never polled");
    }
  }

  printf("test_profile_ford: ALL PASS\n");
  return 0;
}
