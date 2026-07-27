// Standalone driver for the libFuzzer harness, so the host suite can build and
// exercise it without clang or libFuzzer.
//
// WHY THIS EXISTS. The real fuzzing runs in ClusterFuzzLite, which needs the
// OSS-Fuzz clang toolchain. Without this file the harness would compile ONLY
// inside that Docker image — so a refactor that broke it (a renamed decoder
// signature, a changed DecodeCtx) would go unnoticed until the next
// ClusterFuzzLite run, and would fail there as an infrastructure error rather
// than an obvious compile break in the PR that caused it.
//
// Compiling it here with the same ASan/UBSan flags as fuzz_obd.cpp keeps the
// harness honest on every PR at effectively zero cost. It is NOT a substitute
// for coverage-guided fuzzing: the inputs below are fixed, so this proves the
// harness works, not that the code is bug-free.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv);
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

// Same reproducible xorshift32 as fuzz_obd.cpp.
static uint32_t g_rng = 0xC0FFEEu;
static uint32_t rnd() {
  uint32_t x = g_rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  g_rng = x ? x : 0xC0FFEEu;
  return g_rng;
}

int main() {
  LLVMFuzzerInitialize(nullptr, nullptr);

  // Known-tricky shapes, each prefixed with the 4 control bytes the harness
  // consumes. These are exactly the inputs that have historically broken the
  // parser: empty, prompt-only, under-length multiframe, padded multiframe.
  static const char* corpus[] = {
    "", ">", "\r\r", "OK\r\r>", "NO DATA\r\r", "?\r",
    "SEARCHING...\r41 0C 1A F8\r>", "41 0C 1A F8",
    "62 00 9B 0E 84 41 E1", "7F 22 31",
    "00C\r0:62009B0E84\r\r",
    "00C\r0:620083C3008E\r1:0004FFFFFFFF55\r\r",
    "ZZZZ", "414141414141", ":::", "0:\r1:\r2:\r", "FFF\r0:\r",
  };
  long cases = 0;
  for (const char* c : corpus) {
    for (uint8_t mode : {(uint8_t)0x01, (uint8_t)0x22}) {
      std::vector<uint8_t> in{mode, 0x00, 0x9B, 0x00};
      in.insert(in.end(), c, c + std::strlen(c));
      LLVMFuzzerTestOneInput(in.data(), in.size());
      cases++;
    }
  }

  // Short inputs, including every length below the 4-byte control prefix, so
  // the early-return path is covered too.
  for (size_t n = 0; n <= 8; n++) {
    std::vector<uint8_t> in(n);
    for (size_t i = 0; i < n; i++) in[i] = (uint8_t)(rnd() & 0xFF);
    LLVMFuzzerTestOneInput(in.data(), in.size());
    cases++;
  }

  // Bulk random, to shake the decoder-selector across the whole table.
  for (int it = 0; it < 20000; it++) {
    size_t n = (size_t)(rnd() % 64);
    std::vector<uint8_t> in(n);
    for (size_t i = 0; i < n; i++) in[i] = (uint8_t)(rnd() & 0xFF);
    LLVMFuzzerTestOneInput(in.data(), in.size());
    cases++;
  }

  std::printf("libfuzzer harness driver: %ld cases, no sanitizer trip\n", cases);
  std::printf("\nFUZZ HARNESS OK\n");
  return 0;
}
