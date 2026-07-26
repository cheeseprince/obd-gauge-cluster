# OBD Gauge Cluster

An in-cab gauge display that shows what your factory dash doesn't. It reads standard OBD-II
on any vehicle, and where a profile exists it also reads the **manufacturer-specific**
parameters — transmission temperature, oil pressure, EGT, DPF pressure, fuel rail pressure,
DEF level — over a cheap Bluetooth adapter on a small dashboard screen.

*Built with substantial help from an AI coding assistant (Claude) — CI-gated, and every
vehicle profile validated on a real vehicle. [Details](#ai-assistance).*

![The unit on the dash, running the towing page](docs/images/dash.jpg)

One firmware image holds **every vehicle profile** and picks the right one automatically from the
car's **VIN** on connect (with a **Pick Vehicle** menu override). It's **validated on a 2025 GM
Sierra 1500 3.0L Duramax** (LZ0, Global B); a BMW 535i (F10) and an Audi Q5 (2.0T) are skeleton
profiles, and a Ford 6.7L Power Stroke is researched (see [Vehicles it works on](#vehicles-it-works-on)).
The enhanced parameters above are not standardized and no manufacturer publishes them, so adding
a vehicle means discovering its PID map on the vehicle itself — the tooling for that is included
(`tools/obd_scan`).

## What it does

Tiles grouped by task, one page at a time. One rotary knob drives everything: turn to move,
press to zoom a tile, hold for settings. Out-of-range values turn amber, then red.

**Every screenshot below is the GMC Sierra Duramax profile — 7 pages, 27 tiles.** Other
vehicles ship fewer and different pages; see [Vehicles it works on](#vehicles-it-works-on).

| TOW | POWER |
| :---: | :---: |
| ![TOW](docs/images/page0_day.png) | ![POWER](docs/images/page1_day.png) |
| Transmission · coolant · oil pressure · EGT | Boost · horsepower · RPM · engine load |

| REGEN | RANGE |
| :---: | :---: |
| ![REGEN](docs/images/page2_day.png) | ![RANGE](docs/images/page3_day.png) |
| DPF Δp · fuel rate · NOx · rail pressure | Fuel and DEF level + gallons-to-fill |

| TRIP | DIAG | MISC |
| :---: | :---: | :---: |
| ![TRIP](docs/images/page4_day.png) | ![DIAG](docs/images/page5_day.png) | ![MISC](docs/images/page6_day.png) |
| Instant + average economy | MAF · EGR · CAC temp · intake temp | Speed · volts · oil temp |

Press the knob to zoom one tile, with a rolling trend graph coloured by alarm zone — a
**5-minute** window on the CrowPanel builds, 3.5 minutes on `elecrow_obd`. The theme follows
sunrise and sunset from the on-board clock; units switch imperial/metric.

| Focus view | Night theme | Metric |
| :---: | :---: | :---: |
| ![Focus](docs/images/focus_day.png) | ![Night](docs/images/page0_night.png) | ![Metric](docs/images/page0_metric.png) |

Every reading is checked against per-stat thresholds set in the vehicle profile: amber on a
warn crossing, red on a critical one, with the trend line coloured per sample.

| Warning tile | Error tile | Trend graph, all three zones |
| :---: | :---: | :---: |
| ![Warning](docs/images/warning_tile.png) | ![Error](docs/images/error_tile.png) | ![Alarm zones](docs/images/alarm_zones.png) |
| TRANS in the amber warn band | TRANS over the red critical line | Green → amber → red across the window |

The status bar shows link and logging state at a glance: Bluetooth icon, clock, page, and the
SD-logging indicator.

![Status-bar states](docs/images/statusbar_states.png)

Everything logs to a microSD card at 1 Hz. **Full detail** — every tile, the alarm model, the
status-bar states: [`docs/DISPLAY.md`](docs/DISPLAY.md). **The layout is editable** (one table
in the vehicle profile): [`docs/CUSTOMIZING-VIEWS.md`](docs/CUSTOMIZING-VIEWS.md).

## Vehicles it works on

All profiles ship in one image; the firmware **auto-selects by VIN** on connect. An unrecognized
VIN falls back to a generic Mode-01 profile, and **Settings → Pick Vehicle** overrides and locks
the choice.

| Manufacturer | Model | Engine | Years | Pages | Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Audi | Q5 (typ FY) | 2.0T TFSI EA888.3 | 2018–20 | **3** — TEMPS · DRIVE · AIR | 🟡 Skeleton — [details](docs/VEHICLES.md#audi) |
| BMW | 535i (F10) | N55 3.0L turbo I6 | 2011–16 | **3** — ENGINE · DRIVE · MISC | 🟡 Skeleton — [details](docs/VEHICLES.md#bmw) |
| Chevrolet | Silverado 1500 | 3.0L Duramax LZ0 | 2023–26 | 7 — same profile as the Sierra | ✅ Expected, not separately tested |
| Ford | F-250/350 Super Duty | 6.7L Power Stroke | 2017–22 | — none yet | 🔬 Researched, needs a scan |
| GMC | Sierra 1500 | 3.0L Duramax LZ0 | 2023–26 | **7** — TOW · POWER · REGEN · RANGE · TRIP · DIAG · MISC | ✅ **Validated on a real truck** |
| *(any other)* | — | — | — | **2** — ENGINE · AIR | ⚪ **Generic** — standard OBD-II only |

Gas cars have no DPF, DEF, EGT or regeneration, so the truck pages don't exist for them — a BMW
or Audi gets three pages of what its engine actually reports.

Enhanced (Mode 22 / UDS) PIDs are manufacturer-specific and undocumented, so adding a vehicle
means discovering its PID map on the actual vehicle. Standard OBD-II parameters (RPM, speed,
coolant, load) work anywhere; the enhanced ones only where a profile exists. **Per-vehicle
detail and how VIN selection works:** [`docs/VEHICLES.md`](docs/VEHICLES.md).

## Hardware you need

| Part | Detail |
| :--- | :--- |
| Display / MCU | [Elecrow CrowPanel Advance 3.5"](https://www.elecrow.com/crowpanel-advance-3-5-hmi-esp32-ai-display-480x320-artificial-intelligent-ips-touch-screen.html) (ESP32-S3-WROOM-1-N16R8), 480×320 IPS |
| Input | [Arduino Modulino Knob](https://store-usa.arduino.cc/products/modulino-knob) rotary encoder (I²C) |
| Encoder cable | [SparkFun Qwiic-to-Grove cable](https://www.sparkfun.com/qwiic-cable-grove-adapter-100mm.html) (the board is Grove, the encoder is Qwiic) |
| Clock | On-board PCF8563 RTC + coin cell |
| Storage | microSD (FAT32) |
| OBD adapter | A **BLE** ELM327 — see [Adapters](docs/ADAPTERS.md) |
| Power | Truck USB (switched 5 V) → board USB-C |

**Assembly is one cable.** The panel has a **Grove** I²C plug and the knob has a **Qwiic** plug,
so the SparkFun adapter cable bridges them. Everything else is on-board or plug-in. Full guide:
[`docs/WIRING.md`](docs/WIRING.md).

![Wiring: one I²C cable, panel to knob](docs/images/wiring.png)

**Case** — three printed parts plus **8× M3 heat-set inserts** (4 mm long, 5 mm OD) and
**4× M3×10 + 4× M3×6 screws**. STLs, print settings and photos:

**→ https://www.printables.com/model/1788789-odb-gauge-cluster-case**

![3D-printed case, CAD assembly](docs/images/case.png)

BOM and dimensions: [`hardware/`](hardware/). **Why this board and this input method**, and what
was tried first: [`docs/HARDWARE.md`](docs/HARDWARE.md).

A classic ESP32 board (Elecrow WROVER-B) also builds via the `elecrow` envs for a
classic-Bluetooth adapter, but it has no over-the-air updates — it is USB-flash only.

## Install it

**Prerequisites:** PlatformIO Core, Python 3.12, and git. Verified on macOS and Linux; Windows
is untested. The first install is over USB; everything after that is done from your phone and
the knob.

**1. Flash the firmware (once, over USB).** Connect the CrowPanel with a **data** USB-C cable
and, from a clone of this repo:

```
pio run -e crowpanel_obd -t upload      # BLE dash (default)
pio run -e elecrow_obd  -t upload       # classic-Bluetooth dash
```

Wrong port? Find it with `ls /dev/cu.usbmodem*` (macOS) or `ls /dev/ttyACM*` (Linux) and pass
it with `--upload-port`.

**2. Power it in the vehicle.** Plug into a switched USB port. The dash boots to a connecting
screen.

**3. Provision WiFi and location (from your phone).** Long-press the knob → **WiFi setup**. The
dash raises a per-device network **`OBD-XXXX`** with a random password shown on screen; join it
and a setup page opens at `http://192.168.4.1`. Add your WiFi (needed for updates) and your
location (enables automatic day/night). Tap **Done** and the dash reboots.

![Settings menu, reached by a long knob-press](docs/images/settings.png)

**4. Plug in the OBD adapter.** Get a **BLE / "Bluetooth 4.0"** ELM327 — the dash scans and
auto-connects, no pairing step.

| Adapter | Transport | Build | Status |
| :--- | :--- | :--- | :--- |
| Vgate **vLinker MS** | BLE | `crowpanel_obd` | ✅ **Validated on the dash** |
| Vgate **iCar Pro BLE 4.0** | BLE | `crowpanel_obd` | 🟡 Should work — not bench-tested |
| Generic CC2541 / `0xFFE0` / `0xFFF0` clones | BLE | `crowpanel_obd` | 🟡 Should work — not bench-tested |
| Any PIN-pairing classic-BT ELM327 | Classic BT | `elecrow_obd` | ⚠️ Needs the `elecrow_obd` build |
| **OBDLink MX+ / CX** | BLE (proprietary) | — | ❌ **Unsupported** |

Why each verdict, the GATT profiles supported, and how to report a working adapter:
[`docs/ADAPTERS.md`](docs/ADAPTERS.md). To switch adapters later, use **Forget adapter** in the
settings menu.

**5. Set the clock** (optional) via **Set date/time** — it backs up to the coin cell, so you
only do it once.

Gauges appear once the adapter links. From here, updates are over-the-air — no cable.
**Full walkthrough, per-OS notes and troubleshooting:** [`docs/INSTALL.md`](docs/INSTALL.md).

## Updates (OTA)

The dashboard self-updates over WiFi. Pushing a version tag (e.g. `v1.1.0`) builds a
release in CI, stamps that tag as the firmware's version, and publishes it to this repo's
`gh-pages` branch, served by GitHub Pages as `manifest.txt` + the firmware `.bin`. A
device's **Check update** menu fetches the manifest, compares its version against its own
running build, and flashes the new image into a spare slot only after verifying its
SHA-256.

If the `OTA_SIGNING_KEY` repo secret is set, releases also publish a `manifest.sig`
(ECDSA P-256/SHA-256 signature over `manifest.txt`), and devices with a real key compiled
into `src/ota_pubkey.h` refuse any manifest that doesn't verify — before even downloading
the `.bin`. Until both the secret and `src/ota_pubkey.h` are set, this is a no-op
("transition mode"): the SHA-256 `.bin` check is still enforced either way.

Cut a release by pushing a tag (any increasing `vMAJOR.MINOR.PATCH`):

```
git tag vX.Y.Z && git push origin vX.Y.Z
```

`publish_ota.sh` does the same build-and-publish locally as a fallback. CI
(`.github/workflows/ci.yml`) also runs the full host-test suite and a device build on every
push and pull request.

A release is **one firmware image per board type** (`crowpanel_obd.bin`, `elecrow_obd.bin`),
not per vehicle — a device installs the image for its board. As multi-vehicle support lands
(runtime profile selection by VIN), a single image will cover every supported vehicle.

To host updates for a fork: enable GitHub Pages on the `gh-pages` branch and point
`OTA_BASE_URL` (in `platformio.ini`) at your Pages URL.

## The discovery tool

`tools/obd_scan/` maps an unknown vehicle's enhanced PIDs from a laptop:

1. **census** — probe candidate CAN headers in both 11-bit and 29-bit addressing to find
   which modules answer (a negative response proves a module is alive, which confirms the
   addressing);
2. **sweep** — brute-force the PID blocks at each live module;
3. **log** — poll every hit plus known anchors (RPM, speed, load, coolant) through a drive,
   storing raw hex;
4. **correlate** — enumerate every plausible byte interpretation of each unknown PID and
   rank it against the anchors, so "address 22F478 answered with 9 bytes" resolves to
   "bytes 1–2 track RPM — an EGT sensor."

It is **read-only by construction**: only OBD read services and an allow-listed set of AT
commands can be transmitted — writes, routines, resets and clears are rejected in code —
because it runs on vehicles that may not be yours.

```
cd tools
python3 -m obd_scan census    --vehicle gm -o census.json
python3 -m obd_scan sweep      --census census.json -o sweep.json
python3 -m obd_scan log        --sweep sweep.json -o drive.csv   # during a drive
python3 -m obd_scan correlate  drive.csv -o report.md
```

## Building from source

PlatformIO builds the firmware; the tools and tests are Python 3.12.

```
pio run -e crowpanel                     # dash UI with mock data (no adapter)
cd test && make                          # firmware logic tests + fuzz (no hardware)
cd tools/obd_scan && python3 -m pytest tests -q   # scanner tests
```

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

## Documentation

- [`docs/PORTING-LESSONS.md`](docs/PORTING-LESSONS.md) — the method for mapping a new
  vehicle, and the traps: transport quirks, decode traps that produce plausible wrong
  answers, why correlation is not identity, and why community PID lists cannot be trusted.
- [`docs/SIERRA-GATE-RUNBOOK.md`](docs/SIERRA-GATE-RUNBOOK.md) — a worked example of the
  discovery method on the reference truck, used as an acceptance gate for the scanner.
- [`docs/FORD-STATUS.md`](docs/FORD-STATUS.md) — status of the Ford 6.7L Power Stroke port
  and what it would take to finish.
- [`docs/obd-scan-design.md`](docs/obd-scan-design.md) — the scanner's design.
- [`docs/HARDWARE.md`](docs/HARDWARE.md) — why this board and this input, and the input
  methods tested first.
- [`docs/WIRING.md`](docs/WIRING.md) — the one I²C cable, and what is on-board.
- [`docs/CUSTOMIZING-VIEWS.md`](docs/CUSTOMIZING-VIEWS.md) — rearranging the tiles and pages.

## Safety and scope

This reads diagnostic data; it does not modify the vehicle, and the scanner cannot transmit
a write. It is a hobby project provided **without warranty** (see [`LICENSE`](LICENSE)) —
you are responsible for what you plug into your own vehicle.

Enhanced PID values are facts measured from a vehicle through its legislated OBD-II port; no
manufacturer documentation was copied to produce them. Some commercial tools compute derived
parameters (air density, corrected horsepower) under patent — those are not implemented
here.

## AI assistance

This project was built with substantial help from an AI coding assistant (Anthropic's Claude)
— firmware, the scanner tooling, tests, and documentation. Every change is reviewed and gated
by CI (host + fuzz tests, device builds, secret/PII scans), and each vehicle profile is
validated against a real vehicle before it is trusted. Nothing here is auto-generated and left
unchecked.

## License

MIT — see [`LICENSE`](LICENSE).
