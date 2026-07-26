// test_addressing: GM's AddressingDef.emit() must reproduce, byte for byte, the
// exact AT SH strings the legacy atShFor() produced — the regression oracle that
// proves the addressing refactor does not change the truck-validated firmware.
#include <cassert>
#include <cstring>
#include <cstdio>
#include "../src/vehicle_profile.h"
#include "../src/vehicle_active.h"
#include "../src/can29_ecm_addr.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

// The legacy atShFor() (verbatim from obd_query.h before the refactor), kept
// here as the oracle. ECM 29-bit address default is 0x10.
static const char* legacyAtSh(int h, bool can29) {
  static char b[16];
  if (can29) {
    if (h == 0) return "AT SH DB33F1\r";
    snprintf(b, sizeof b, "AT SH DA%02XF1\r", h == 1 ? 0x18u : (unsigned)can29EcmAddr());
    return b;
  }
  return h == 1 ? "AT SH 7E2\r" : h == 2 ? "AT SH 7E0\r" : "AT SH 7DF\r";
}

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  assert(VEHICLE.addressingCount == 3);
  char buf[24];
  for (int h = 0; h < 3; h++) {
    for (int c = 0; c < 2; c++) {              // c=0 11-bit, c=1 29-bit
      bool can29 = (c == 1);
      const char* got = VEHICLE.addressing[h].emit(0, can29, buf, sizeof buf);
      assert(got && std::strcmp(got, legacyAtSh(h, can29)) == 0);
      // Exactly one setup command per GM header: step 1 terminates the sequence.
      assert(VEHICLE.addressing[h].emit(1, can29, buf, sizeof buf) == nullptr);
    }
  }
  // The runtime-mutable ECM address must flow through emit() — proves the hook
  // survived the refactor (it was inert before, but Task 2 relies on it staying live).
  can29EcmAddr() = 0x17;
  const char* mutated = VEHICLE.addressing[2].emit(0, /*can29*/true, buf, sizeof buf);
  assert(mutated && std::strcmp(mutated, "AT SH DA17F1\r") == 0);
  can29EcmAddr() = 0x10;   // restore the default for any later assertions

  printf("test_addressing: ALL PASS\n");
  return 0;
}
