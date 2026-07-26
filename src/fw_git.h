#pragma once
// Firmware identity stamp for the splash/menu display and the OTA manifest.
//
// FW_GIT: short git commit hash of the build. Default "local" (untouched
// working tree). release.yml / publish_ota.sh overwrite this line via sed
// with the built commit's short hash. Always present; used as the fallback
// version string for untagged/local builds.
//
// FW_VERSION: the version string shown to the user (splash + VERSION menu
// card) and published in the OTA manifest, which the device compares against
// the manifest's version to decide whether an update is available. For a
// tagged release (release.yml, triggered by a `v*` tag push) this is stamped
// with that tag, e.g. "v1.1.0" — see the "Stamp firmware version" step.
// Local/dev/manual builds fall back to a "dev-<hash>" form (or "local").
#define FW_GIT "local"
#define FW_VERSION "local"
