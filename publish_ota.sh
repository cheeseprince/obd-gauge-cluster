#!/usr/bin/env bash
# publish_ota.sh — build the firmware and publish an OTA release.
#
# Releases live on this repo's `gh-pages` branch, served by GitHub Pages at
# OTA_BASE_URL (see platformio.ini). A device's "Check update" menu fetches
# manifest.txt from there, compares the version against its own build, and
# self-updates over WiFi with SHA-256 verification.
#
# NOTE: unlike release.yml, this local fallback does NOT publish a
# manifest.sig — and signature enforcement is mandatory in the firmware
# (ota_update.cpp), so EVERY device will refuse a manifest published this
# way. The script therefore aborts unless OTA_ALLOW_UNSIGNED=1 is set. Use a
# tagged `git push --tags` release (release.yml) for signed releases.
#
#   ./publish_ota.sh            build crowpanel_obd, publish to gh-pages
#
# Requires: a clean git tree (the release is stamped with HEAD's short hash),
# push access to this repo, and pio on PATH. Run from the repo root.
set -euo pipefail
cd "$(dirname "$0")"

ENVS=(crowpanel_obd)
PAGES_BRANCH="gh-pages"
REMOTE_URL="$(git remote get-url origin)"

if [ -n "$(git status --porcelain)" ]; then
  echo "ERROR: git tree is dirty — commit first (the release is stamped with HEAD)."
  exit 1
fi

# Fail LOUDLY rather than produce a release no device will take: this path
# publishes no manifest.sig, and the firmware unconditionally enforces the
# manifest signature (see ota_update.cpp).
if [ "${OTA_ALLOW_UNSIGNED:-0}" != "1" ]; then
  echo "ERROR: this script publishes an UNSIGNED release, and every device"
  echo "enforces manifest.sig — no device will install it. Cut a signed"
  echo "release with a tag push instead (release.yml)."
  echo "To publish anyway (fork/debug use): OTA_ALLOW_UNSIGNED=1 $0"
  exit 1
fi
GIT=$(git rev-parse --short HEAD)
# HEAD is a tagged release (vX.Y.Z) -> stamp that tag as the version;
# otherwise fall back to a dev-<hash> form (matches release.yml + the
# FW_VERSION fallback baked into src/fw_git.h).
VERSION=$(git describe --tags --exact-match --match 'v*' 2>/dev/null || echo "dev-$GIT")

# Stamp fw_git.h with the real hash + version so the device can compare
# against the manifest, build, then restore the tracked placeholder.
sed -i "s/#define FW_GIT \".*\"/#define FW_GIT \"$GIT\"/" src/fw_git.h
sed -i "s/#define FW_VERSION \".*\"/#define FW_VERSION \"$VERSION\"/" src/fw_git.h
trap 'git checkout -q -- src/fw_git.h' EXIT

for e in "${ENVS[@]}"; do
  echo "== building $e"
  pio run -e "$e" >/dev/null
done

# Publish to the gh-pages branch via a detached worktree (keeps binaries out
# of the source history).
TMP=$(mktemp -d)
trap 'git checkout -q -- src/fw_git.h; rm -rf "$TMP"; git worktree prune' EXIT
if git ls-remote --exit-code --heads "$REMOTE_URL" "$PAGES_BRANCH" >/dev/null 2>&1; then
  git fetch -q origin "$PAGES_BRANCH"
  git worktree add -q "$TMP/pages" "origin/$PAGES_BRANCH"
  git -C "$TMP/pages" checkout -q -B "$PAGES_BRANCH"
else
  git worktree add -q --detach "$TMP/pages"
  git -C "$TMP/pages" checkout -q --orphan "$PAGES_BRANCH"
  git -C "$TMP/pages" rm -rq --cached . 2>/dev/null || true
  find "$TMP/pages" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
fi

: > "$TMP/pages/manifest.txt"
for e in "${ENVS[@]}"; do
  BIN=".pio/build/$e/firmware.bin"
  SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
  SIZE=$(stat -c%s "$BIN")
  cp "$BIN" "$TMP/pages/$e.bin"
  echo "$e $VERSION $SHA $SIZE $e.bin" >> "$TMP/pages/manifest.txt"
  echo "   $e: $VERSION ($GIT) $SIZE bytes"
done

git -C "$TMP/pages" add -A
git -C "$TMP/pages" commit -q -m "release $VERSION ($GIT)"
git -C "$TMP/pages" push -q origin "$PAGES_BRANCH"
echo "published $VERSION ($GIT) -> gh-pages (GitHub Pages serves it in ~1 min)"
