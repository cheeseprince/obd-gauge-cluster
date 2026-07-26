#include <cstdio>
#include <cstring>
#include "ota_common.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

static const char SHA[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

int main() {
  OtaRelease r;
  char line[256];

  snprintf(line, sizeof line, "elecrow v1.0.0 %s 1368448 elecrow.bin", SHA);
  check(parseManifestLine(line, "elecrow", r), "well-formed line parses");
  check(strcmp(r.version, "v1.0.0") == 0, "version field");
  check(r.size == 1368448u, "size field");
  check(strcmp(r.bin, "elecrow.bin") == 0, "bin field");

  check(!parseManifestLine(line, "crowpanel_obd", r), "env mismatch rejected");
  check(!parseManifestLine("garbage line", "elecrow", r), "garbage rejected");
  snprintf(line, sizeof line, "elecrow v1.0.0 deadbeef 100 x.bin");
  check(!parseManifestLine(line, "elecrow", r), "short sha rejected");
  snprintf(line, sizeof line, "elecrow v1.0.0 %s 0 x.bin", SHA);
  check(!parseManifestLine(line, "elecrow", r), "zero size rejected");

  // Multi-line manifest: finds the right env on any line.
  char man[512];
  snprintf(man, sizeof man,
           "crowpanel_obd v1.1.0 %s 1065648 crowpanel_obd.bin\n"
           "elecrow dev-2222222 %s 1368448 elecrow.bin\n", SHA, SHA);
  check(findRelease(man, "elecrow", r) && strcmp(r.version, "dev-2222222") == 0,
        "findRelease locates second line");
  check(findRelease(man, "crowpanel_obd", r) && strcmp(r.version, "v1.1.0") == 0,
        "findRelease locates first line");
  check(!findRelease(man, "elecrow_obd", r), "absent env not found");

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
