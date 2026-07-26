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
```

The host tests and scanner are verified in CI on **Linux and macOS** (g++ and
clang, Python 3.12). The PlatformIO device build is host-OS-independent.

Match the style of the surrounding code. Keep changes focused.

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

`main` is protected — every change goes through a PR, and three checks must pass before it
can merge:

- **Metadata check** — no tracked image or PDF may carry GPS/EXIF/author metadata.
- **Host tests** — firmware logic + scanner tests.
- **Device build** — the firmware compiles.

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
