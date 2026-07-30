#include "boot_banner.h"
#include <cstdio>

size_t formatBootBanner(const BootInfo& bi, char* out, size_t n) {
  if (!out || n == 0) return 0;

  // An empty or null key means the Generic profile is active. Print a real
  // token: "profile=" with nothing after it cannot be parsed unambiguously.
  const char* key = (bi.profileKey && bi.profileKey[0]) ? bi.profileKey : "generic";

  // key=value throughout, on purpose. The rig parses this by splitting on '='
  // rather than matching a prose shape, so adding a field later cannot break
  // the parser.
  int w = snprintf(out, n,
                   "[BOOT] env=%s ver=%s git=%s profile=%s\n"
                   "[BOOT] psram=%uMB flash=%uMB reset=%d heap=%u",
                   bi.env ? bi.env : "n/a",
                   bi.version ? bi.version : "n/a",
                   bi.git ? bi.git : "n/a",
                   key,
                   (unsigned)(bi.psramBytes / (1024u * 1024u)),
                   (unsigned)(bi.flashBytes / (1024u * 1024u)),
                   bi.resetReason,
                   (unsigned)bi.freeHeap);
  if (w < 0) return 0;

  // snprintf's return value is the length it WOULD have written if `out` were
  // big enough — when the banner is truncated that number can exceed n, which
  // would let a caller walk past the end of `out` if it trusted the return
  // value as a byte count into that buffer. Clamp to n-1 (snprintf always
  // reserves the last byte for the NUL), so the return value is always a safe
  // bound on what's actually in `out`.
  return (size_t)w < n ? (size_t)w : n - 1;
}
