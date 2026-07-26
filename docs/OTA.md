# Updates (OTA)

How the dash's **Check update** menu item fetches, verifies, and installs a new firmware
image over WiFi, how a release is signed and cut, and what to change to host your own
update channel on a fork.

> **OTA is a CrowPanel-only capability.** `HAS_OTA` is compiled to `1` only when
> `BOARD_CROWPANEL` is defined — every other board (the retired `elecrow`/`elecrow_obd`
> WROVER build) compiles an empty, does-nothing stand-in instead (`src/board_caps.h:22-29`,
> `src/ota_update.h:25`). The `elecrow_obd` build has no "Check update" behavior at all —
> reflash it over USB, always. Everything below describes `crowpanel_obd`/`crowpanel`.

## How a device updates

From **Check update** in the on-screen menu, the dash (`src/ota_update.cpp`,
`otaCheckUpdate`):

1. Joins the strongest saved WiFi network (from the credentials stored during
   [first-time setup](INSTALL.md)).
2. Fetches `manifest.txt` from `OTA_BASE_URL` (GitHub Pages) over TLS, with the server
   certificate validated against the Mozilla root-CA bundle compiled into the firmware
   (`src/ota_update.cpp:159-167`).
3. Fetches `manifest.sig` and verifies an ECDSA P-256/SHA-256 signature over the exact bytes
   of `manifest.txt`, using the public key compiled into the firmware
   (`src/ota_update.cpp:183-215`). **This step is mandatory** — see [Signing](#signing)
   below.
4. Looks up the release matching this device's `OTA_ENV` in the manifest, then compares its
   version against the running firmware's version with a semver comparison. Installs only a
   **strictly newer** release — equal or older is refused, which blocks a
   replayed-old-manifest downgrade attack (`src/ota_update.cpp:217-228`).
5. Streams the `.bin` into the spare OTA slot while hashing it, then checks the streamed
   SHA-256 against the manifest's `sha256` field **before** calling `Update.end()`
   (`src/ota_update.cpp:264-279`).
6. Only then activates the new slot and reboots (`src/ota_update.cpp:281-282`).

**Every failure path — join failed, manifest fetch failed, signature missing/invalid,
already up to date, bin fetch failed, size/hash mismatch, flash error — returns without
touching the currently-running slot.** The device either boots the verified new image or
keeps running exactly what it was already running; there is no partially-applied state.

## Signing

| Piece | Where it lives | Fact | Source |
| :--- | :--- | :--- | :--- |
| Private signing key | GitHub Actions secret `OTA_SIGNING_KEY` | Never committed | `.github/workflows/release.yml` |
| Public verification key | Compiled in, `src/ota_pubkey.h` | Committed to the repo (public keys are meant to be public) | `src/ota_pubkey.h` |
| Enforcement | Firmware | **Mandatory** — file-scope `static_assert(sizeof(OTA_PUBKEY_PEM) > 64, ...)` | `src/ota_update.cpp:39-40` |

A real compiled-in public key is not optional. An earlier version of this firmware had a fail-open branch — internally called "transition mode" — that skipped signature verification while `OTA_PUBKEY_PEM` was still an empty placeholder. That branch was deleted as a security fix: a bad merge or a stripped header would otherwise have silently disabled signature enforcement. Today, shipping firmware with no real key **does not compile** — the `static_assert` above turns that failure mode into a build error instead of a weakened device. At runtime, a missing or invalid `manifest.sig` is refused before the `.bin` is ever downloaded (`src/ota_update.cpp:208-215`).

`publish_ota.sh`, the local fallback publisher, enforces the same posture from the other
side: it refuses to publish a release with no `manifest.sig` unless you explicitly set
`OTA_ALLOW_UNSIGNED=1`, and says why —

```
ERROR: this script publishes an UNSIGNED release, and every device
enforces manifest.sig — no device will install it. Cut a signed
release with a tag push instead (release.yml).
```

(`publish_ota.sh:31-40`). `OTA_ALLOW_UNSIGNED=1` is meant for fork/debug use against a
device that's had its own unsigned-tolerant `ota_pubkey.h`/build swapped in — a stock
build from this repo will still reject an unsigned manifest regardless.

## Anti-rollback

The only rollback protection is the **semver comparison described above** — a device
refuses to install a manifest whose version is not strictly newer than what it's already
running (`src/ota_update.cpp:221-228`). A non-release running version (`local`, `dev-<hash>`,
i.e. anything flashed over USB rather than from a tagged release) parses as version `0`, so
a USB/dev build always accepts the next signed release.

**Be precise about what this is not:** there is no hardware anti-rollback. The CrowPanel
board is an ESP32-S3 (`board = esp32-s3-devkitc-1`, `platformio.ini:140`), which has an
eFuse secure-version counter for exactly this purpose, and it is **not enabled** on this
project. A signed manifest for an *older* released version, if an attacker could get a
device to see it, is only stopped by software semver comparison — it is not fused out at
the silicon level. There is likewise no Secure Boot and no flash encryption configured.
Signature verification is real and mandatory; it is the only line of defense, not one layer
among several.

## Cutting a release

```
git tag vX.Y.Z && git push origin vX.Y.Z
```

Pushing a `v*` tag runs `.github/workflows/release.yml`, which:

1. Builds `crowpanel_obd` and stamps the pushed tag as `FW_VERSION`.
2. Computes the `.bin`'s SHA-256 and size, writing `manifest.txt`.
3. Signs `manifest.txt` into `manifest.sig` with the `OTA_SIGNING_KEY` secret.
4. Publishes `manifest.txt` + `manifest.sig` + `crowpanel_obd.bin` to the `gh-pages` branch
   (GitHub Pages serves it) — **tag pushes only**.
5. Creates a GitHub Release for the tag with the same three files attached, for humans
   browsing the repo (devices never fetch these; they update over the air).

A manual `workflow_dispatch` run (no tag) does steps 1–3 as a dry run — using a
`dev-<hash>` version string — to prove the build-and-sign pipeline still works, but the
publish and release-creation steps are gated on `github.ref_type == 'tag'`
(`.github/workflows/release.yml:76-105`), so a manual run can never overwrite the live
release on `gh-pages`.

`publish_ota.sh` does the same build-and-publish locally as a fallback, requiring a clean
git tree and push access; see [Signing](#signing) above for why it needs
`OTA_ALLOW_UNSIGNED=1` to actually publish anything a device will accept.

**Today only `crowpanel_obd` goes through this pipeline** — `release.yml` and
`publish_ota.sh` both build and publish that one environment. `elecrow_obd` is built and
smoke-tested by CI (`.github/workflows/ci.yml`, `device-build` job) but is not part of the
release/OTA pipeline, consistent with `elecrow`/`elecrow_obd` having no OTA support at all
(see the top of this page).

## One image per board type

A release is one firmware image per board, not per vehicle: `crowpanel_obd.bin` (there is
no current `elecrow_obd.bin` release artifact — see above). A device installs the image
built for its board.

**This is not per-vehicle.** All vehicle profiles ship in the same image and are selected at
runtime by reading the connected vehicle's VIN — see [Vehicles](VEHICLES.md) for how that
selection works. There is no separate build per vehicle, and there never needs to be one:
adding a new vehicle profile does not add a new release artifact.

## Hosting updates for a fork

To run your own update channel instead of pointing at the upstream `gh-pages`:

1. **Enable GitHub Pages** on your fork, serving from the `gh-pages` branch (Settings →
   Pages). `release.yml` creates and pushes that branch for you on the first tagged
   release.
2. **Point `OTA_BASE_URL` at your fork.** It's set per-env in `platformio.ini` (currently
   `https://cheeseprince.github.io/obd-gauge-cluster/` for both `crowpanel` and
   `crowpanel_obd`) and can also be overridden at build time with `-D
   OTA_BASE_URL="https://<you>.github.io/<repo>/"`.
3. **Generate your own signing key pair and replace `src/ota_pubkey.h`.** Do not reuse the
   upstream key — you don't have (and shouldn't want) its private half, and a device
   trusting the upstream public key would refuse manifests signed by your key anyway. The
   comment block at the top of `src/ota_pubkey.h` has the exact `openssl` commands: generate
   an EC P-256 key pair offline, paste the **public** key's PEM into `ota_pubkey.h`, and
   store the **private** key's PEM as your fork's `OTA_SIGNING_KEY` Actions secret. Never
   commit the private key anywhere — this repo's CI gates on `scripts/check_no_pii.py` and
   gitleaks, and a committed private key is exactly the kind of thing gitleaks exists to
   catch.
4. Flash the new `ota_pubkey.h` build once over USB so the device on the bench holds your
   key before you start relying on OTA for it — the first device to receive your new key
   has to get it by cable, same as any factory-fresh device does today.
