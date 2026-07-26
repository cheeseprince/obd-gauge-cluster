#include "ota_common.h"
#include <cstring>
#include <cstdio>

bool parseManifestLine(const char* line, const char* env, OtaRelease& out) {
  if (!line || !env) return false;
  char lenv[32];
  unsigned long size = 0;
  // Widths guard every buffer: env 31, version 23, sha 64, bin 47.
  int got = sscanf(line, "%31s %23s %64s %lu %47s",
                   lenv, out.version, out.sha256, &size, out.bin);
  if (got != 5) return false;
  if (strcmp(lenv, env) != 0) return false;
  if (strlen(out.sha256) != 64 || size == 0) return false;
  out.size = (uint32_t)size;
  return true;
}

bool findRelease(const char* manifest, const char* env, OtaRelease& out) {
  if (!manifest) return false;
  const char* p = manifest;
  while (*p) {
    if (parseManifestLine(p, env, out)) return true;
    const char* nl = strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return false;
}
