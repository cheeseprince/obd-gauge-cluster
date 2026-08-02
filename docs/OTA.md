# Updates (OTA)

How the dash's **Check update** menu item fetches, verifies, and installs a new firmware
image over WiFi, how a release is signed and cut, and what to change to host your own
update channel on a fork.

There is one board and one OTA image; everything below describes it.

## How a device updates

From **Check update** in the on-screen menu, the dash (`src/ota_update.cpp`,
`otaCheckUpdate`):

1. Joins the strongest saved WiFi network (from the credentials stored during
   [first-time setup](INSTALL.md)).
2. Fetches `manifest.txt` from `OTA_BASE_URL` (GitHub Pages) over TLS, with the server
   certificate validated against the Mozilla root-CA bundle compiled into the firmware
   (`otaCheckUpdate` in `src/ota_update.cpp`).
3. Fetches `manifest.sig` and verifies an ECDSA P-256/SHA-256 signature over the exact bytes
   of `manifest.txt`, using the public key compiled into the firmware
   (`otaVerifyManifest`, called from `otaCheckUpdate`). **This step is mandatory** — see [Signing](#signing)
   below.
4. Looks up the release matching this device's `OTA_ENV` in the manifest, then compares its
   version against the running firmware's version with a semver comparison. Installs only a
   **strictly newer** release — equal or older is refused, which blocks a
   replayed-old-manifest downgrade attack (`parseSemver` in `src/ota_update.cpp`).
5. Streams the `.bin` into the spare OTA slot while hashing it, then checks the streamed
   SHA-256 against the manifest's `sha256` field **before** calling `Update.end()`
   (the streaming-hash loop in `otaCheckUpdate`).
6. Only then activates the new slot and reboots (the end of `otaCheckUpdate`).

**Every failure path — join failed, manifest fetch failed, signature missing/invalid,
already up to date, bin fetch failed, size/hash mismatch, flash error — returns without
touching the currently-running slot.** The device either boots the verified new image or
keeps running exactly what it was already running; there is no partially-applied state.

## Signing

| Piece | Where it lives | Fact | Source |
| :--- | :--- | :--- | :--- |
| Private signing key | GitHub Actions secret `OTA_SIGNING_KEY` | Never committed | `.github/workflows/release.yml` |
| Public verification key | Compiled in, `src/ota_pubkey.h` | Committed to the repo (public keys are meant to be public) | `src/ota_pubkey.h` |
| Enforcement | Firmware | **Mandatory** — file-scope `static_assert(sizeof(OTA_PUBKEY_PEM) > 64, ...)` | the file-scope `static_assert` in `src/ota_update.cpp` |

A real compiled-in public key is not optional. An earlier version of this firmware had a fail-open branch — internally called "transition mode" — that skipped signature verification while `OTA_PUBKEY_PEM` was still an empty placeholder. That branch was deleted as a security fix: a bad merge or a stripped header would otherwise have silently disabled signature enforcement. Today, shipping firmware with no real key **does not compile** — the `static_assert` above turns that failure mode into a build error instead of a weakened device. At runtime, a missing or invalid `manifest.sig` is refused before the `.bin` is ever downloaded (`otaCheckUpdate`, before the download starts).

`publish_ota.sh`, the local fallback publisher, enforces the same posture from the other
side: it refuses to publish a release with no `manifest.sig` unless you explicitly set
`OTA_ALLOW_UNSIGNED=1`, and says why —

```
ERROR: this script publishes an UNSIGNED release, and every device
enforces manifest.sig — no device will install it. Cut a signed
release with a tag push instead (release.yml).
```

(the signing guard in `publish_ota.sh`). `OTA_ALLOW_UNSIGNED=1` is meant for fork/debug use
against a device that's had its own unsigned-tolerant `ota_pubkey.h`/build swapped in — a stock
build from this repo will still reject an unsigned manifest regardless.

## Anti-rollback

The only rollback protection is the **semver comparison described above** — a device
refuses to install a manifest whose version is not strictly newer than what it's already
running (`parseSemver` in `src/ota_update.cpp`). A non-release running version (`local`, `dev-<hash>`,
i.e. anything flashed over USB rather than from a tagged release) parses as version `0`, so
a USB/dev build always accepts the next signed release.

**Be precise about what this is not:** there is no hardware anti-rollback. The CrowPanel
board is an ESP32-S3 (`board = esp32-s3-devkitc-1` in `[env:crowpanel_obd]`), which has an
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
4. Generates a CycloneDX **SBOM** for the firmware (`tools/gen_sbom.py`) describing the
   resolved dependency tree the binary was actually built from.
5. **Attests** both the SBOM and the build provenance to Sigstore, and stages the provenance
   bundle beside the `.bin` so it is available as a file, not only through the attestations
   API.
6. Publishes the whole `out/` directory to the `gh-pages` branch (GitHub Pages serves it) —
   **tag pushes only**.
7. Creates a GitHub Release for the tag with the same files attached, for humans browsing
   the repo (devices never fetch these; they update over the air).

Steps 4–7 put **five** files on both surfaces, not three:

| Asset | What it is |
| :--- | :--- |
| `crowpanel_obd.bin` | The firmware image |
| `manifest.txt` | Version, SHA-256, size — what the device reads to decide whether to update |
| `manifest.sig` | Signature over the manifest; the device's trust anchor |
| `crowpanel_obd.bin.cdx.json` | CycloneDX SBOM |
| `crowpanel_obd.bin.intoto.jsonl` | Sigstore build-provenance bundle |

Only the first three participate in an OTA update. The SBOM and provenance bundle are there
for verification (`gh attestation verify`) and for anyone auditing what went into a build.

A manual `workflow_dispatch` run (no tag) does steps 1–4 as a dry run — using a
`dev-<hash>` version string — to prove the build, sign and SBOM pipeline still works, but the
publish and release-creation steps are gated on `github.ref_type == 'tag'`
(the `if: github.ref_type == 'tag'` guards in `.github/workflows/release.yml`), so a manual
run can never overwrite the live release on `gh-pages`.

**SBOM generation is deliberately part of the dry run.** It was tag-only until v0.1.4, which
meant the step's first ever execution was a real release — and it failed there, after the
firmware had already been built and signed, because `actions/attest` requires a
`serialNumber` that the CycloneDX spec treats as optional. A rehearsal that skips a step does
not rehearse it. Attestation itself stays tag-only, since it mints a real Sigstore entry.

`publish_ota.sh` does the same build-and-publish locally as a fallback, requiring a clean
git tree and push access; see [Signing](#signing) above for why it needs
`OTA_ALLOW_UNSIGNED=1` to actually publish anything a device will accept.

`release.yml` and `publish_ota.sh` both build and publish `crowpanel_obd`, the only
firmware environment that ships. (`crowpanel` is the same board with mock data, for bench
work — it is not released.)

### Before you tag: link to an adapter on real hardware

**CI cannot test the OBD link.** There is no radio and no adapter on a GitHub runner, so
every check can pass on firmware that never connects to a vehicle. That is not a
hypothetical: **v0.1.0 shipped exactly that way** and had to be pulled.

If the change touches any of these, flash a board over USB and confirm the dash reaches
live gauge values *before* pushing a tag:

- the **Arduino core or IDF version** (`platform` in `platformio.ini`)
- the **NimBLE version**, or anything in `src/ble_obd_source.*`
- **WiFi or TLS** (`src/ota_update.cpp`, `src/ota_portal.cpp`)

The failure that motivated this rule is worth knowing, because no amount of code review
would have caught it: NimBLE 2.x changed `setConnectTimeout()` from **seconds to
milliseconds** while keeping the parameter `uint32_t`. `setConnectTimeout(4)` compiled
without a warning and became a 4 **millisecond** connection budget. A library major version
can silently redefine what a value *means* — the type checker has nothing to say about it,
and neither does a test suite that cannot reach the radio.

## If you shipped a bad release

It happens — a build can pass every check and still fail on a vehicle, because CI has no
radio (see [Before you tag](#before-you-tag-link-to-an-adapter-on-real-hardware)). The
procedure, in order:

1. **Fix forward, do not try to roll back.** Devices refuse older versions (semver
   comparison), so an earlier release cannot be re-served. Tag a new patch release with the
   fix; it reaches devices over the air like any other.
2. **Mark the bad GitHub Release a pre-release** and replace its notes with a warning that
   names the symptom and the cause. It loses the "Latest" badge as soon as a good release
   exists.
3. **Delete the firmware `.bin` asset from that release** so it cannot be flashed by
   mistake. Leave `manifest.txt` / `manifest.sig` — they are harmless and useful evidence.
4. **Keep the tag.** The commit is real history, the fix references it, and anyone who
   already fetched keeps their copy regardless. Deleting a tag buys nothing and breaks
   references.
5. **The OTA channel needs no cleanup.** `release.yml` deploys with `keep_files: false`, so
   the next publish overwrites the manifest and binary wholesale.

The channel serves exactly one version at a time, so step 1 is what actually protects
users — steps 2 to 4 only stop someone installing the bad build by hand.

## One image, not one per vehicle

A release is a single firmware image, `crowpanel_obd.bin`.

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
