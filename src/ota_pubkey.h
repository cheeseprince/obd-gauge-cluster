#pragma once
// ota_pubkey.h — OTA manifest signing PUBLIC key (PEM, ECDSA P-256).
//
// A REAL KEY IS MANDATORY: signature enforcement is always on. Emptying or
// stripping this header is a compile error (static_assert in ota_update.cpp),
// not a silent fall-back to unsigned updates — the old "transition mode"
// ended when the real key landed.
//
// To ROTATE the key (or set one up in a fork):
//   1. Generate an EC P-256 keypair (do this once, offline, and DO NOT commit
//      the private key):
//        openssl ecparam -genkey -name prime256v1 -noout -out ota_signing_key.pem
//        openssl ec -in ota_signing_key.pem -pubout -out ota_pubkey.pem
//   2. Paste the contents of ota_pubkey.pem (the PUBLIC key) below as the
//      OTA_PUBKEY_PEM string literal, keeping the PEM's own line breaks as
//      "\n" inside the C string (or use a raw/concatenated literal).
//   3. Store ota_signing_key.pem's contents (the PRIVATE key) as the
//      OTA_SIGNING_KEY secret in this repo's GitHub Actions settings — see
//      .github/workflows/release.yml, which signs manifest.txt into
//      manifest.sig only when that secret is present.
//   4. Rebuild and flash this firmware once with the real pubkey compiled in
//      (via USB, or by taking one last unsigned/unenforced OTA) so the
//      device on the truck actually holds the key before enforcement starts
//      mattering. Every signed release after that verifies.
//
// src/ota_update.cpp does the actual verification (mbedtls_pk_verify,
// ECDSA-P256/SHA-256 over the exact bytes of manifest.txt as published).
static const char OTA_PUBKEY_PEM[] =
  "-----BEGIN PUBLIC KEY-----\n"
  "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEei2TMey80dnlTcPgTOy/59JO1DSX\n"
  "t6HjjvaQDuCDR1uwVGyfdMdFFS0QITgFMhAjkD43JEg1kXNTZ+GrWvQK+g==\n"
  "-----END PUBLIC KEY-----\n";
