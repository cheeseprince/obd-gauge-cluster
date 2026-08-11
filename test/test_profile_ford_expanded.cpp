// test_profile_ford_expanded: the 2026-08-09 Ford F-350 profile EXPANSION --
// eight rows that used to be dark (BOOST, RAIL, FUEL RATE, DEF, DPF dP,
// PEDAL, CAC, EGR) filled from a re-check of the same supported-PID bitmap
// (see ford_sd_67.cpp). The original test_profile_ford.cpp still asserts the
// STATIC invariants (row count, addressing, TRANS/GEAR enhanced decodes,
// which rows stay dark); this file is scoped to the eight NEW decoders only,
// each checked against a real captured payload, a too-short frame, and an
// all-0xFF sentinel frame.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "../src/readouts.h"
#include "../src/app_types.h"
#include "../src/vehicle_profile.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile FORD_SD_67_PROFILE;

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

int main() {
  g_activeProfile = &FORD_SD_67_PROFILE;
  DecodeCtx ctx{nullptr};   // BOOST falls back to the 101.3 kPa std-atmosphere default

  // --- every one of the eight rows must now carry a live cmd, at the ECM (7E0) ---
  struct Row { StatId id; const char* cmd; };
  static const Row kRows[] = {
    { StatId::Boost,    "0187" },
    { StatId::Rail,     "016D" },
    { StatId::FuelRate, "019D" },
    { StatId::Def,      "019B" },
    { StatId::DpfDp,    "017A" },
    { StatId::Pedal,    "0149" },
    { StatId::Cac,      "0177" },
    { StatId::Egr,      "0169" },
  };
  for (const auto& r : kRows) {
    assert(READOUTS[(int)r.id].cmd != nullptr);
    assert(std::strcmp(READOUTS[(int)r.id].cmd, r.cmd) == 0);
    assert(READOUTS[(int)r.id].header == 0);   // all eight are ECM (7E0) rows
    assert(READOUTS[(int)r.id].decode != nullptr);
  }
  printf("  ok   eight rows carry the census-confirmed cmd, at 7E0\n");

  // --- every one of the eight rows must be reachable (page tile or helper) ---
  // (test_profile_ford.cpp already runs this same check over the whole table;
  // repeating it scoped to just the eight new rows pins the specific claim
  // that filling a row's cmd didn't leave it stranded.)
  {
    bool shown[(int)StatId::COUNT] = {false};
    for (int p = 0; p < readoutPageCount(); p++)
      for (int c = 0; c < 4; c++) {
        int idx = readoutAt(p, c);
        if (idx >= 0) shown[idx] = true;
      }
    for (int h = 0; h < VEHICLE.defaultLayout.helperCount; h++)
      shown[(int)VEHICLE.defaultLayout.helpers[h]] = true;
    for (const auto& r : kRows)
      assert(shown[(int)r.id] && "newly-live row has a cmd but is on no page or helper");
  }
  printf("  ok   all eight newly-live rows are reachable (page or helper)\n");

  // ===========================================================================
  // BOOST (0187): bytes[1..2] uint16 * 0.03125 = kPa abs; minus baro (ctx, else
  // 101.3 std atmosphere); kPa->psi; clamp negative to 0.
  // ===========================================================================
  {
    auto boost = READOUTS[(int)StatId::Boost].decode;

    // Real capture, cruise sample: 01 0C D3 00 00.
    // bytes[1..2] = 0x0CD3 = 3283 -> *0.03125 = 102.59375 kPa.
    // minus 101.3 std-atmosphere fallback (no ctx.values) = 1.29375 kPa
    // -> *0.1450377 psi/kPa = 0.1877 psi.
    { const uint8_t d[] = {0x01, 0x0C, 0xD3, 0x00, 0x00};
      assert(near(boost(d, 5, ctx), 0.1877f, 0.02f)); }

    // Real capture, boosted sample: 01 10 69 00 00.
    // bytes[1..2] = 0x1069 = 4201 -> *0.03125 = 131.28125 kPa.
    // minus 101.3 = 29.98125 kPa -> *0.1450377 = 4.349 psi.
    { const uint8_t d[] = {0x01, 0x10, 0x69, 0x00, 0x00};
      assert(near(boost(d, 5, ctx), 4.349f, 0.05f)); }

    // ctx.values with a plausible BARO overrides the 101.3 fallback. Same
    // boosted-sample MAP (131.28125 kPa) against a 95.0 kPa BARO instead:
    // 131.28125 - 95.0 = 36.28125 kPa -> *0.1450377 = 5.263 psi.
    { float vals[(int)StatId::COUNT] = {0};
      vals[(int)StatId::Baro] = 95.0f;
      DecodeCtx baroCtx{vals};
      const uint8_t d[] = {0x01, 0x10, 0x69, 0x00, 0x00};
      assert(near(boost(d, 5, baroCtx), 5.263f, 0.05f)); }

    // Short frame: need bytes[1..2], n=2 is one byte short.
    { const uint8_t d[] = {0x01, 0x0C};
      assert(std::isnan(boost(d, 2, ctx))); }
    // All-0xFF sentinel on the used bytes.
    { const uint8_t d[] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
      assert(std::isnan(boost(d, 5, ctx))); }
  }
  printf("  ok   BOOST (0187)\n");

  // ===========================================================================
  // RAIL (016D): bytes[3..4] uint16 * 10 = kPa -> psi.
  // ===========================================================================
  {
    auto rail = READOUTS[(int)StatId::Rail].decode;

    // Real capture: 07 0D B6 0D BF 46 00 00 00 00 00.
    // bytes[3..4] = 0x0DBF = 3519 -> *10 = 35190 kPa -> *0.1450377 = 5103.88 psi.
    { const uint8_t d[] = {0x07, 0x0D, 0xB6, 0x0D, 0xBF, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00};
      assert(near(rail(d, 11, ctx), 5103.88f, 1.0f)); }

    // Short frame: need bytes[3..4], n=4 is one byte short.
    { const uint8_t d[] = {0x07, 0x0D, 0xB6, 0x0D};
      assert(std::isnan(rail(d, 4, ctx))); }
    // All-0xFF sentinel on the used bytes.
    { const uint8_t d[] = {0x07, 0x0D, 0xB6, 0xFF, 0xFF, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00};
      assert(std::isnan(rail(d, 11, ctx))); }
  }
  printf("  ok   RAIL (016D)\n");

  // ===========================================================================
  // FUEL RATE (019D): bytes[0..1] uint16 * 0.02 = g/s; /0.832 g/mL -> mL/s;
  // *3600 -> mL/hr; /3785.41 -> US gal/hr.
  // ===========================================================================
  {
    auto fuelRate = READOUTS[(int)StatId::FuelRate].decode;

    // Real capture: 00 1E 00 22.
    // bytes[0..1] = 0x001E = 30 -> *0.02 = 0.6 g/s.
    // (0.6/0.832) * 3600 / 3785.41 = 0.6859 gal/hr.
    { const uint8_t d[] = {0x00, 0x1E, 0x00, 0x22};
      assert(near(fuelRate(d, 4, ctx), 0.6859f, 0.01f)); }

    // Short frame: need bytes[0..1], n=1 is one byte short.
    { const uint8_t d[] = {0x00};
      assert(std::isnan(fuelRate(d, 1, ctx))); }
    // All-0xFF sentinel on the used bytes.
    { const uint8_t d[] = {0xFF, 0xFF, 0x00, 0x22};
      assert(std::isnan(fuelRate(d, 4, ctx))); }
  }
  printf("  ok   FUEL RATE (019D)\n");

  // ===========================================================================
  // DEF (019B): byte[1] * 100/255 = %.
  // ===========================================================================
  {
    auto def = READOUTS[(int)StatId::Def].decode;

    // Real capture: 3F 7F 40 95. byte[1] = 0x7F = 127 -> 127*100/255 = 49.8039%.
    { const uint8_t d[] = {0x3F, 0x7F, 0x40, 0x95};
      assert(near(def(d, 4, ctx), 49.80f, 0.05f)); }

    // Short frame: need byte[1], n=1 is one byte short.
    { const uint8_t d[] = {0x3F};
      assert(std::isnan(def(d, 1, ctx))); }
    // All-0xFF sentinel on the used byte.
    { const uint8_t d[] = {0x3F, 0xFF, 0x40, 0x95};
      assert(std::isnan(def(d, 4, ctx))); }
  }
  printf("  ok   DEF (019B)\n");

  // ===========================================================================
  // DPF dP (017A): bytes[3..4] UNSIGNED uint16 * 0.01 = kPa -> psi. NOTE this
  // is a different byte offset/sign than pid_decode.cpp's decodeDpfDeltaKpa()
  // (bytes[1..2], signed) -- this truck's response layout genuinely differs.
  // ===========================================================================
  {
    auto dpf = READOUTS[(int)StatId::DpfDp].decode;

    // Real capture: 02 01 E0 00 3C FF FF.
    // bytes[3..4] = 0x003C = 60 -> *0.01 = 0.6 kPa -> *0.1450377 = 0.0870 psi.
    // (trailing FF FF at d[5..6] is NOT in the used byte range and must not
    // trigger the sentinel guard.)
    { const uint8_t d[] = {0x02, 0x01, 0xE0, 0x00, 0x3C, 0xFF, 0xFF};
      assert(near(dpf(d, 7, ctx), 0.0870f, 0.005f)); }

    // Short frame: need bytes[3..4], n=4 is one byte short.
    { const uint8_t d[] = {0x02, 0x01, 0xE0, 0x00};
      assert(std::isnan(dpf(d, 4, ctx))); }
    // All-0xFF sentinel on the used bytes.
    { const uint8_t d[] = {0x02, 0x01, 0xE0, 0xFF, 0xFF, 0xFF, 0xFF};
      assert(std::isnan(dpf(d, 7, ctx))); }
  }
  printf("  ok   DPF dP (017A)\n");

  // ===========================================================================
  // PEDAL (0149): byte[0] * 100/255 = %.
  // ===========================================================================
  {
    auto pedal = READOUTS[(int)StatId::Pedal].decode;

    // Real capture: 26. byte[0] = 0x26 = 38 -> 38*100/255 = 14.902%.
    { const uint8_t d[] = {0x26};
      assert(near(pedal(d, 1, ctx), 14.902f, 0.01f)); }

    // Short frame: need byte[0], n=0.
    { const uint8_t d[] = {};
      assert(std::isnan(pedal(d, 0, ctx))); }
    // All-0xFF sentinel.
    { const uint8_t d[] = {0xFF};
      assert(std::isnan(pedal(d, 1, ctx))); }
  }
  printf("  ok   PEDAL (0149)\n");

  // ===========================================================================
  // CAC (0177): byte[1] - 40 = degC -> degF.
  // ===========================================================================
  {
    auto cac = READOUTS[(int)StatId::Cac].decode;

    // Real capture: 01 49 00 00 00. byte[1] = 0x49 = 73 -> 33 degC -> 91.4 degF.
    { const uint8_t d[] = {0x01, 0x49, 0x00, 0x00, 0x00};
      assert(near(cac(d, 5, ctx), 91.4f, 0.05f)); }

    // Short frame: need byte[1], n=1 is one byte short.
    { const uint8_t d[] = {0x01};
      assert(std::isnan(cac(d, 1, ctx))); }
    // All-0xFF sentinel on the used byte.
    { const uint8_t d[] = {0x01, 0xFF, 0x00, 0x00, 0x00};
      assert(std::isnan(cac(d, 5, ctx))); }
  }
  printf("  ok   CAC (0177)\n");

  // ===========================================================================
  // EGR (0169): byte[1] * 100/255 = %.
  // ===========================================================================
  {
    auto egr = READOUTS[(int)StatId::Egr].decode;

    // Real capture: 07 10 10 80 00 00 00. byte[1] = 0x10 = 16 -> 16*100/255 = 6.2745%.
    { const uint8_t d[] = {0x07, 0x10, 0x10, 0x80, 0x00, 0x00, 0x00};
      assert(near(egr(d, 7, ctx), 6.2745f, 0.01f)); }

    // Short frame: need byte[1], n=1 is one byte short.
    { const uint8_t d[] = {0x07};
      assert(std::isnan(egr(d, 1, ctx))); }
    // All-0xFF sentinel on the used byte.
    { const uint8_t d[] = {0x07, 0xFF, 0x10, 0x80, 0x00, 0x00, 0x00};
      assert(std::isnan(egr(d, 7, ctx))); }
  }
  printf("  ok   EGR (0169)\n");

  printf("test_profile_ford_expanded: ALL PASS\n");
  return 0;
}
