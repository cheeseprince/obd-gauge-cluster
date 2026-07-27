// libFuzzer entry point for the OBD parse + decode path (ClusterFuzzLite).
//
// This is the SAME attack surface as fuzz_obd.cpp, driven differently:
//
//   fuzz_obd.cpp            fixed-seed sweep, own PRNG, runs in `make` on every
//                           PR. Deterministic and fast; coverage is whatever the
//                           seed happens to reach, and it never grows.
//   this file               coverage-guided. libFuzzer mutates toward new paths
//                           and keeps a corpus, so coverage accumulates across
//                           runs instead of resetting.
//
// Both are worth having. The sweep is a regression gate you get for free in CI;
// this one is the explorer. Neither replaces the other.
//
// Why this surface: parseObdResponse() and the decoders are the only code in the
// firmware that consumes bytes from OUTSIDE the trust boundary — whatever the
// BLE adapter sends. A malicious or simply broken adapter is the one untrusted
// input this device has, so it is the one that earns continuous fuzzing.
//
// Built by .clusterfuzzlite/build.sh. Also compiled and exercised by the host
// suite via fuzz_obd_driver.cpp, so it cannot silently stop building.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "obd_parse.h"
#include "readouts.h"
#include "app_types.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

// Kept live so the optimiser cannot delete the decoder calls we are testing.
static volatile float g_sink = 0.0f;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
  // READOUTS / READOUT_COUNT are macros over g_activeProfile; without this they
  // dereference null. GM is chosen because it is the widest table (33 rows,
  // both Mode-01 and Mode-22), so it reaches the most decoders.
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // 4 control bytes then the payload. Taking mode/pid/selector FROM the input
  // rather than fixing them lets libFuzzer steer which parser branch and which
  // decoder it exercises, which is the whole point of coverage guidance.
  if (size < 4) return 0;
  const uint8_t  mode = data[0];
  const uint16_t pid  = (uint16_t)((data[1] << 8) | data[2]);
  const uint8_t  sel  = data[3];
  const uint8_t* body = data + 4;
  const size_t   n    = size - 4;

  float vals[STAT_COUNT] = {0};
  vals[IDX_BARO] = 101.3f;          // some decoders (boost) read baro via ctx
  DecodeCtx ctx{vals};

  // 1) Parser, on the raw remainder treated as an ELM reply.
  std::string s(reinterpret_cast<const char*>(body), n);
  std::vector<uint8_t> out;
  if (parseObdResponse(s, mode, pid, out) && !out.empty()) {
    for (int i = 0; i < READOUT_COUNT; i++)
      if (READOUTS[i].decode)
        g_sink += READOUTS[i].decode(out.data(), (int)out.size(), ctx);
  }

  // 2) One decoder directly on the payload, selected by the input.
  //
  // The copy is deliberate and must not be optimised into a view of `data`:
  // the buffer has to be sized EXACTLY n on the heap so that a read one byte
  // past the end is a real heap-buffer-overflow ASan can see. Passing `body`
  // straight through would hide overruns inside libFuzzer's own larger buffer.
  std::vector<uint8_t> exact(body, body + n);
  const int idx = (int)(sel % (uint8_t)READOUT_COUNT);
  if (READOUTS[idx].decode)
    g_sink += READOUTS[idx].decode(exact.data(), (int)exact.size(), ctx);

  return 0;
}
