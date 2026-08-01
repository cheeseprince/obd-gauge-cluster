# Contributing

Thanks for your interest. The most valuable contribution this project can get is
**support for another vehicle** — because that needs something the maintainer can't do
remotely: your vehicle.

## Ways to help

- **Map a new vehicle** (the big one) — discover its enhanced PIDs and add a profile.
- **Fix or improve** the firmware, the scanner, or the docs.
- **Report** a bug or share a drive log from a vehicle you couldn't finish mapping.

## Adding a vehicle

Enhanced (Mode 22 / UDS) PIDs are manufacturer-specific and undocumented, so they have to
be measured on the actual vehicle. The tooling and method are included:

1. Read [`docs/PORTING-LESSONS.md`](docs/PORTING-LESSONS.md) — the method and the traps
   (adapter-dependent addressing, decode traps that produce *plausible wrong answers*, why
   correlation is not identity). [`docs/SIERRA-GATE-RUNBOOK.md`](docs/SIERRA-GATE-RUNBOOK.md)
   is a worked example.
2. Run `tools/obd_scan` on the vehicle: `census` → `sweep` → `log` (a drive) → `correlate`.
   It is **read-only by construction** — it can only read, never write to the vehicle.
3. Identify the PIDs from the correlation, then add a `src/vehicles/<vehicle>.cpp` profile
   (PID table, decoders, thresholds, layout, tank sizes) modeled on the existing GM
   profile. See [`docs/CUSTOMIZING-VIEWS.md`](docs/CUSTOMIZING-VIEWS.md) for the layout.
4. Open a pull request. Even a partial map or a raw drive log is useful — attach it to an
   issue using the "New vehicle" template.

## Development

PlatformIO builds the firmware; the tools and tests are Python 3.12.

```
pio run -e crowpanel                     # dash UI with mock data (no hardware)
cd test && make                          # firmware logic tests + fuzz
cd tools/obd_scan && python3 -m pytest tests -q
cd tools/hil && python3 -m pytest tests -q
```

That last suite is the pure-logic half of the **hardware-in-the-loop rig** (`tools/hil/`),
which flashes a real board over USB and asserts on its boot serial. Those tests need no
board — they cover the log parsing and port selection — and CI runs them, so they can fail
your PR. Running the rig against actual hardware is optional and documented in
[`tools/hil/README.md`](tools/hil/README.md).

The host tests and scanner are verified in CI on **Linux and macOS** (g++ and
clang, Python 3.12). The PlatformIO device build is host-OS-independent.

### Knowing which build is actually running

At the end of `setup()` the firmware prints a two-line identity banner to `Serial`:

```
[BOOT] env=… ver=… git=… profile=…
[BOOT] psram=…MB flash=…MB reset=… heap=…
```

Read it before debugging anything, because it settles the question that wastes the most
time — *is the board even running the build I think it is?* A bench `pio run` stamps `ver`
and `git` as `local`; only a tagged CI release carries a real version. `reset=` is the
ESP-IDF reset reason, so a panic reboot is distinguishable from a clean USB reset at a
glance, and `profile=` shows which vehicle profile the VIN or your manual pick selected.

### The build platform is pinned to a URL, deliberately

`platformio.ini` does not use the `espressif32` platform from PlatformIO's registry. It
pins a **pioarduino** release by URL:

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13/platform-espressif32.zip
```

pioarduino is the community continuation of the Espressif platform; this release gives
**Arduino core 3.1.3 on ESP-IDF 5.3**. The registry's `espressif32` is a different, older
platform (core 2.x), which no longer receives IDF security fixes.

**Do not "simplify" this to `platform = espressif32`.** The bare name resolves to whatever
is installed — which on a developer machine with pioarduino installed globally is the right
thing, and on a clean CI runner is PlatformIO's core-2.x platform. The build then fails on
core-3-only symbols with no hint as to why. That has already happened once.

The first build after cloning downloads the platform and toolchain, so expect it to take a
few minutes.

Match the style of the surrounding code. Keep changes focused.

### Local files: use `.private/`

Anything you don't want committed — scratch notes, generated reports, drive logs,
experiments — goes in **`.private/`** at the repo root. The whole directory is
git-ignored, so nothing in it needs its own rule and no new filename can slip
through. It is not created by the repo; make it when you need it.

Real vehicle data has a stronger rule of its own: `tools/obd_scan` census/sweep/drive
files contain your VIN, and those patterns are ignored by name as well — see
[Secrets & PII](#secrets--pii--install-the-pre-commit-hooks) below.

## Repository layout

```
platformio.ini          PlatformIO project
src/                     firmware
  vehicles/              per-vehicle PID tables, decoders, thresholds, layout
test/                    host unit tests (+ fuzz)
tools/
  obd_scan/              the discovery scanner (read-only)
  analyze_logs.py        drive-CSV self-audit + alarm replay
  ui_snapshot/           pixel-exact host render of the real UI
  stl_render.py          isometric STL preview
hardware/                3D-printable enclosure — BOM + Printables link
docs/                    porting method, per-vehicle status, acceptance runbook, images
publish_ota.sh          local build + publish (CI does this on a tag)
.github/workflows/      CI (tests + build) and release (build + publish)
```

Adding a vehicle touches exactly two of these: a new file under `src/vehicles/` and one
line in the profile registry. See [Adding a vehicle](#adding-a-vehicle) above.

## Pull requests

`main` is protected — every change goes through a PR, and **seven** checks must pass before it
can merge:

| Check | What it enforces |
| :--- | :--- |
| **Host tests (ubuntu-latest)** | Firmware logic tests + fuzz, the scanner tests, and the HIL rig's pure-logic tests |
| **Host tests (macos-latest)** | The same suites again on clang — the host tests are cross-platform |
| **Device build (PlatformIO)** | The firmware actually compiles for the dash boards |
| **Lint (ruff)** | Python style and errors in `tools/obd_scan` and `tools/hil` |
| **Scan images/PDFs for sensitive metadata** | No tracked image or PDF carries GPS/EXIF/author metadata |
| **PII guard (no real VINs)** | Only synthetic test VINs appear in tracked files |
| **Secret scan (gitleaks)** | No keys, tokens, or passwords committed |

The last three are the reason a seemingly innocent commit can be rejected: a phone photo with
GPS EXIF, a real VIN pasted into a test, or a key in a scratch file. The pre-commit hooks below
catch the last two **before** you commit.

You will also see two checks that are **not** required and cannot block a merge:

| Check | What it is |
| :--- | :--- |
| **Fuzz (address)** / **Fuzz (undefined)** | Coverage-guided fuzzing of the OBD parse and decode path via ClusterFuzzLite, on any PR touching `src/`. Time-boxed to a few minutes per sanitizer; findings go to the Security tab. Advisory by design — a slow runner must not block an unrelated merge |

The host suite's own `fuzz` target is different and *is* required: a fixed-seed sweep that
runs in `make`. One is a regression gate, the other explores. Both are kept.

Please also:

- **Strip metadata from any image you add** before committing:
  `exiftool -all= <file>` (or re-save it without metadata). The check will block the merge
  otherwise — this is how a GPS-tagged phone photo is kept out of the repo.
- Run the tests locally first (above).
- Update the relevant docs if you change behavior.

## Secrets & PII — install the pre-commit hooks

To keep real VINs, keys, tokens, and passwords out of the repo, secrets are checked at three
points. The local pre-commit hooks catch a leak **before it is ever committed**; the CI jobs
of the same name are the non-optional backstop. **Install them once per clone:**

```bash
pip install pre-commit
pre-commit install          # then the hooks run on every `git commit`
pre-commit run --all-files  # optional: scan the whole tree now
```

| Guard | Blocks | Also in CI |
| :---- | :----- | :--------: |
| `gitleaks` | keys / tokens / passwords | ✅ |
| `scripts/check_no_pii.py` | any real VIN — only synthetic test VINs (real WMI + a `0123456789ABCD` tail) are allowed | ✅ |
| `.gitignore` | scan outputs (`census.json`, `sweep*.json`, `*_drive.csv`) and `*.pem`/`*.key`/`.env` — never stageable | — |

**Never commit real vehicle data.** `tools/obd_scan` census/sweep/drive files contain the VIN
and live vehicle data; they are git-ignored — keep them local. Need a new test VIN? Use a
synthetic one (real WMI + `0123456789ABCD`) and add it to the allowlist in
`scripts/check_no_pii.py`. Report a security issue via [`SECURITY.md`](SECURITY.md).

## Questions

Open a [Discussion](../../discussions) for questions; use Issues for bugs and vehicle
contributions.
