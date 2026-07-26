#pragma once
#include <cstdint>

// ota_common.h — pure helpers shared by the OTA updater (host-tested).
//
// Release manifest format (one line per build env, published by
// publish_ota.sh / release.yml to the GitHub Pages release repo):
//   <env> <version> <sha256-hex> <size-bytes> <bin-filename>
//
// <version> is the release's FW_VERSION (the git tag for a tagged release,
// e.g. "v1.1.0", or a "dev-<hash>" fallback for untagged builds) — see
// src/fw_git.h. The device compares this against its own FW_VERSION to
// decide whether an update is available; the sha256 field is still the
// integrity check on the .bin, independent of the version compare.

struct OtaRelease {
  char     version[24];
  char     sha256[65];
  uint32_t size;
  char     bin[48];
};

// Parse one manifest line. True only if the line's env matches `env` and all
// five fields parse sanely (64-char sha, nonzero size).
bool parseManifestLine(const char* line, const char* env, OtaRelease& out);

// Scan a whole manifest body (\n-separated) for `env`. True on hit.
bool findRelease(const char* manifest, const char* env, OtaRelease& out);
