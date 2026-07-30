#include <cstdio>
#include <cstring>
#include "boot_banner.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }
static bool has(const char* hay, const char* needle){ return std::strstr(hay, needle) != nullptr; }

int main() {
  char buf[256];

  // A tagged release build with a locked GM profile.
  BootInfo bi;
  bi.env        = "crowpanel_obd";
  bi.version    = "0.1.3";
  bi.git        = "e1626f9";
  bi.profileKey = "gm_sierra_lz0";
  bi.psramBytes = 8u * 1024u * 1024u;
  bi.flashBytes = 16u * 1024u * 1024u;
  bi.resetReason = 1;
  bi.freeHeap    = 241344;
  formatBootBanner(bi, buf, sizeof buf);

  check(has(buf, "[BOOT] env=crowpanel_obd"), "line 1 tagged with env");
  check(has(buf, "ver=0.1.3"),                "version");
  check(has(buf, "git=e1626f9"),              "git hash");
  check(has(buf, "profile=gm_sierra_lz0"),    "profile key");
  check(has(buf, "psram=8MB"),                "psram in MB");
  check(has(buf, "flash=16MB"),               "flash in MB");
  check(has(buf, "reset=1"),                  "reset reason");
  check(has(buf, "heap=241344"),              "free heap");
  check(std::strchr(buf, '\n') != nullptr,    "two lines");

  // An empty profile key means the Generic fallback is active. It must print a
  // real token, not an empty value — "profile=" alone is unparseable.
  BootInfo gen = bi;
  gen.profileKey = "";
  formatBootBanner(gen, buf, sizeof buf);
  check(has(buf, "profile=generic"), "empty key prints as generic");

  // A null key must behave like an empty one, not crash.
  BootInfo nul = bi;
  nul.profileKey = nullptr;
  formatBootBanner(nul, buf, sizeof buf);
  check(has(buf, "profile=generic"), "null key prints as generic");

  // A local pio build stamps FW_VERSION/FW_GIT as "local" (src/fw_git.h).
  BootInfo loc = bi;
  loc.version = "local"; loc.git = "local";
  formatBootBanner(loc, buf, sizeof buf);
  check(has(buf, "ver=local"), "bench build version");
  check(has(buf, "git=local"), "bench build hash");

  // Truncation must stay in-bounds and NUL-terminated, and must be DETECTABLE.
  // snprintf semantics mean the reported length exceeds the buffer when the
  // banner did not fit — that is the signal, not a bug.
  char tiny[16];
  size_t n = formatBootBanner(bi, tiny, sizeof tiny);
  check(tiny[sizeof tiny - 1] == '\0', "tiny buffer NUL-terminated");
  check(n > sizeof tiny,               "truncation is detectable");

  // Non-power-of-two sizes must round down, not print a bogus fraction.
  BootInfo odd = bi;
  odd.psramBytes = 0; odd.flashBytes = 4u * 1024u * 1024u;
  formatBootBanner(odd, buf, sizeof buf);
  check(has(buf, "psram=0MB"), "zero psram");
  check(has(buf, "flash=4MB"), "4MB flash");

  if (failures == 0) printf("test_boot_banner: all checks passed\n");
  return failures ? 1 : 0;
}
