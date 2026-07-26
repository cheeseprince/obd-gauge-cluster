#!/usr/bin/env bash
# test_ota_sign.sh — proves the release-side OTA manifest signing format
# round-trips: generate a throwaway P-256 key, sign a sample manifest.txt the
# same way .github/workflows/release.yml does (openssl dgst -sha256 -sign),
# then verify it the way an independent auditor would with
# `openssl dgst -verify`. Also proves a tampered manifest fails verification.
#
# This does NOT exercise the on-device mbedtls verify path in
# src/ota_update.cpp (device-only code, not host-buildable) — it validates
# that the release.yml signing step produces a signature openssl itself
# agrees is valid over the exact published bytes, and that corrupting those
# bytes is detected. Skips cleanly (exit 0) if openssl isn't on PATH.
set -euo pipefail

if ! command -v openssl >/dev/null 2>&1; then
  echo "SKIP: openssl not found on PATH — cannot exercise OTA manifest signing"
  exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Throwaway P-256 keypair — mirrors the real OTA_SIGNING_KEY / ota_pubkey.h
# pair, but generated fresh every run and never touches the real repo secret.
openssl ecparam -genkey -name prime256v1 -noout -out "$TMP/key.pem" >/dev/null 2>&1
openssl ec -in "$TMP/key.pem" -pubout -out "$TMP/pub.pem" >/dev/null 2>&1

# Sample manifest, same shape release.yml publishes:
#   <env> <version> <sha256-hex> <size-bytes> <bin-filename>
SHA=$(printf 'fake-firmware-bytes' | sha256sum | cut -d' ' -f1)
printf 'crowpanel_obd v1.1.0 %s 1065648 crowpanel_obd.bin\n' "$SHA" > "$TMP/manifest.txt"

# Sign exactly as release.yml's "Sign the manifest" step does.
openssl dgst -sha256 -sign "$TMP/key.pem" -out "$TMP/manifest.sig" "$TMP/manifest.txt"

fail=0

if openssl dgst -sha256 -verify "$TMP/pub.pem" -signature "$TMP/manifest.sig" "$TMP/manifest.txt" >/dev/null 2>&1; then
  echo "PASS: valid signature verifies against the published manifest bytes"
else
  echo "FAIL: valid signature did not verify"
  fail=1
fi

# Tamper the manifest bytes (bump the version) and confirm the same signature
# no longer verifies — this is the exact attack the device-side check exists
# to catch (a manifest edited/replaced after signing).
sed 's/v1.1.0/v1.9.9/' "$TMP/manifest.txt" > "$TMP/manifest_tampered.txt"
if openssl dgst -sha256 -verify "$TMP/pub.pem" -signature "$TMP/manifest.sig" "$TMP/manifest_tampered.txt" >/dev/null 2>&1; then
  echo "FAIL: tampered manifest verified against the original signature (should have failed)"
  fail=1
else
  echo "PASS: tampered manifest correctly rejected"
fi

if [ "$fail" -ne 0 ]; then
  echo
  echo "test_ota_sign: FAILED"
  exit 1
fi
echo
echo "test_ota_sign: ALL PASS"
