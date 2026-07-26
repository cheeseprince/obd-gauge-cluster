# OBD Gauge Cluster

An in-cab gauge display for diesel pickups that shows data the factory cluster hides —
transmission temperature, EGT, DPF pressure, oil pressure, fuel rail pressure, DEF level —
pulled over a cheap Bluetooth OBD-II adapter and rendered on a small dashboard
screen.

![The unit on the dash, running the towing page](docs/images/dash.jpg)

One firmware image holds **every vehicle profile** and picks the right one automatically from the
car's **VIN** on connect (with a **Pick Vehicle** menu override). It's **validated on a 2025 GM
Sierra 1500 3.0L Duramax** (LZ0, Global B); a BMW 535i (F10) and an Audi Q5 (2.0T) are skeleton
profiles, and a Ford 6.7L Power Stroke is researched (see [Vehicle support](#vehicle-support)).
The enhanced parameters above are not standardized and no manufacturer publishes them, so adding
a vehicle means discovering its PID map on the vehicle itself — the tooling for that is included
(`tools/obd_scan`).

## The display

Seven pages of four tiles, grouped by task. One rotary knob drives everything: turn to
move, press to zoom a tile, hold for settings. Out-of-range values turn amber, then red.

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

Press the knob to zoom one tile, with a five-minute trend graph coloured by alarm zone.
The theme follows sunrise and sunset from the on-board clock. Units switch imperial/metric.

| Focus view | Night theme | Metric |
| :---: | :---: | :---: |
| ![Focus](docs/images/focus_day.png) | ![Night](docs/images/page0_night.png) | ![Metric](docs/images/page0_metric.png) |

Every reading is evaluated against per-stat thresholds (set in the vehicle profile): a tile's
number and bar turn **amber** on a warn crossing, **red** on a critical one, and the Focus
trend line is coloured per sample, so a warm-up or a fault shows exactly where the value
crossed each line.

| Warning tile | Error tile | Trend graph, all three zones |
| :---: | :---: | :---: |
| ![Warning](docs/images/warning_tile.png) | ![Error](docs/images/error_tile.png) | ![Alarm zones](docs/images/alarm_zones.png) |
| TRANS in the amber warn band | TRANS over the red critical line | Green → amber → red across the 5-minute window |

The top status bar shows link and logging state at a glance: a **Bluetooth** icon (blue when
the OBD adapter is linked, gray when not), the clock, the current page, and the SD-logging
indicator — green `LOG` while recording, red on a write error, grey when logging is off, and
`No SD Card` when none is inserted.

![Status-bar states](docs/images/statusbar_states.png)

Everything logs to a microSD card at 1 Hz, and the firmware updates over WiFi.

**The layout is editable** — which tiles appear, in what order, on how many pages, is one
table in the vehicle profile. See [`docs/CUSTOMIZING-VIEWS.md`](docs/CUSTOMIZING-VIEWS.md).

## Vehicle support

All profiles ship in one image; the firmware **auto-selects by VIN** on connect (the VIN's WMI →
a profile). An unrecognized VIN falls back to a generic Mode-01 profile, and **Settings → Pick
Vehicle** overrides and locks the choice.

| Manufacturer | Model | Engine | Model Year Range | Status |
| :--- | :--- | :--- | :--- | :--- |
| Audi | Q5 (typ FY) | 2.0T TFSI (EA888 gen-3) | 2018–2020 | 🟡 **Skeleton profile** — ATF, oil-temp and charge-air DIDs mapped on-car (scaling unverified); standard Mode-01 exact; auto-detects by VIN. See [`docs/AUDI-STATUS.md`](docs/AUDI-STATUS.md) |
| BMW | 535i (F10) | N55 3.0L turbo I6 | 2011–2016 | 🟡 **Skeleton profile** — mapped on-car: standard Mode-01 exact + boost from MAP; oil pressure mapped (scale unverified). The `6F1` enhanced path is gateway-blocked, so oil temp needs a cold-start drive and ATF is unreachable. Auto-detects by VIN. See [`docs/BMW-STATUS.md`](docs/BMW-STATUS.md) |
| Chevrolet | Silverado 1500 | 3.0L Duramax (LZ0, Global B) | 2023–2026 (LZ0) | ✅ **Expected to work** — same LZ0/Global-B as the Sierra (validated on the GMC, not separately on a Silverado) |
| Ford | F-250/350 Super Duty | 6.7L Power Stroke diesel | 2017–2022 | 🔬 **Researched, not yet mapped** — community data unreliable; needs a truck scan. See [`docs/FORD-STATUS.md`](docs/FORD-STATUS.md) |
| GMC | Sierra 1500 | 3.0L Duramax (LZ0, Global B) | 2023–2026 (LZ0) | ✅ **Working — validated on a real truck**; every enhanced PID measured, not guessed |

**Status:** ✅ working · 🟡 partial (skeleton profile) · 🔬 researched, needs an on-car scan.

Enhanced (Mode 22 / UDS) PIDs are manufacturer-specific and undocumented. Adding a vehicle
means discovering its PID map on the actual vehicle with `tools/obd_scan` and the method in
[`docs/PORTING-LESSONS.md`](docs/PORTING-LESSONS.md). Everything else — other GM diesels,
other BMWs, any vehicle not listed above — is not supported: generic OBD-II parameters (RPM,
speed, coolant, load) work on any vehicle, but the enhanced parameters do not.

## Help wanted — more vehicles

The firmware, transport, UI, logging and OTA are vehicle-agnostic; the only per-vehicle
part is a profile in [`src/vehicles/`](src/vehicles/) (PID table, decoders, thresholds,
layout, tank sizes). Mapping a new vehicle needs one thing this project cannot supply
remotely: **the vehicle**.

If you have a diesel (or any vehicle with enhanced PIDs) and a laptop, you can map it:

1. Run `tools/obd_scan` on the OBD port to discover which PIDs answer (about an hour parked
   plus a drive — see the runbook).
2. Correlate the results to identify each PID.
3. Add a `src/vehicles/<your_vehicle>.cpp` profile and open a pull request.

Start with [`docs/PORTING-LESSONS.md`](docs/PORTING-LESSONS.md) (the method and its
pitfalls) and [`docs/SIERRA-GATE-RUNBOOK.md`](docs/SIERRA-GATE-RUNBOOK.md) (a worked
example). [`docs/FORD-STATUS.md`](docs/FORD-STATUS.md) is a partial head-start on the Ford
6.7L Power Stroke. Issues and PRs welcome — including drive logs from a vehicle you can't
finish mapping yourself.

## Hardware

| Part | Detail |
| :--- | :--- |
| Display / MCU | [Elecrow CrowPanel Advance 3.5"](https://www.elecrow.com/crowpanel-advance-3-5-hmi-esp32-ai-display-480x320-artificial-intelligent-ips-touch-screen.html) (ESP32-S3-WROOM-1-N16R8), 480×320 IPS |
| Input | [Arduino Modulino Knob](https://store-usa.arduino.cc/products/modulino-knob) rotary encoder (I²C) |
| Encoder cable | [SparkFun Qwiic-to-Grove cable](https://www.sparkfun.com/qwiic-cable-grove-adapter-100mm.html) (the board is Grove, the encoder is Qwiic) |
| Clock | On-board PCF8563 RTC + coin cell |
| Storage | microSD (FAT32) |
| OBD adapter | Any BLE or classic-Bluetooth ELM327 — e.g. [Vgate vLinker MS](https://www.vgatemall.com/products-detail/i-79/) (dual BLE + classic-BT) |
| Power | Truck USB (switched 5 V) → board USB-C |

A classic ESP32 board (Elecrow WROVER-B) is also supported for a classic-Bluetooth adapter
via the `elecrow` build envs.

**Why this hardware** — the CrowPanel Advance is an IPS panel (400 nits) that stays
readable in a sunlit cab, where the earlier TN board did not; navigation is a rotary
encoder because it is operable by feel while driving, after resistive touch and physical
buttons were tried first. The reasoning, and the input methods tested, are in
[`docs/HARDWARE.md`](docs/HARDWARE.md).

### Wiring

The only assembly is one I²C cable from the panel to the knob — the connectors differ (a
**Grove** plug on the panel, a **Qwiic** plug on the knob), so the [SparkFun
Qwiic-to-Grove cable](https://www.sparkfun.com/qwiic-cable-grove-adapter-100mm.html) bridges
them. Everything else is on-board or plug-in. Full guide:
[`docs/WIRING.md`](docs/WIRING.md).

![Wiring: one I²C cable, panel to knob](docs/images/wiring.png)

### Case

![3D-printed case, CAD assembly](docs/images/case.png)

Three printed parts plus **8× M3 heat-set inserts** (4 mm long, 5 mm OD) and
**4× M3×10 + 4× M3×6 screws**. STLs, print settings and photos are on Printables:

**→ https://www.printables.com/model/1788789-odb-gauge-cluster-case**

BOM and dimensions: [`hardware/`](hardware/).

## First-time setup

**Prerequisites:** PlatformIO Core, Python 3.12, and git. Verified on macOS and Linux;
Windows is untested.

The first install is over USB from a computer; everything after that is done from your
phone and the knob.

**1. Flash the firmware (once, over USB).** Connect the CrowPanel to a computer with a
**data** USB-C cable and, from a clone of this repo:

```
pio run -e crowpanel_obd -t upload      # BLE dash (default)
pio run -e elecrow_obd  -t upload       # classic-Bluetooth dash
```

If PlatformIO grabs the wrong port, find it and pass it explicitly with
`--upload-port`:

```
ls /dev/cu.usbmodem*                    # macOS — lists the CrowPanel's serial port
pio run -e crowpanel_obd -t upload --upload-port /dev/cu.usbmodemXXXX
```

(Linux equivalent is typically `/dev/ttyACM*` or `/dev/ttyUSB*`.)

**2. Power it in the truck.** Plug it into a switched USB port. The dash boots to a
connecting screen.

**3. Provision over WiFi (from your phone).** Long-press the knob → **WiFi setup**. That
same long-press is the settings menu for everything else below (units, brightness, night
mode, date/time, adapter, logging):

![Settings menu, reached by a long knob-press](docs/images/settings.png)

The dash raises a per-device WiFi network **`OBD-XXXX`** (e.g. `OBD-3F9A` — `XXXX` is 4 hex
digits from the chip's MAC, unique per unit). The password is random per unit — generated on
first use and shown on the dash screen while setup runs; join the network and a setup page
opens at `http://192.168.4.1`. On that page:

- **Add your home/hotspot WiFi** — needed for over-the-air updates.
- **Set your location** — latitude, longitude (west is negative), and UTC offset. There is
  no default location, so automatic day/night stays off until you set this. US daylight
  saving is applied for you.

Tap **Done** and the dash reboots with the settings applied.

**4. Connect the OBD adapter.** Plug a BLE ELM327 into the OBD-II port. The default (BLE)
dash scans and auto-connects to **most BLE ELM327 adapters** — the Vgate vLinker MS and the
common `0xFFF0`/`0xFFE0` GATT clones — no pairing step. It is **BLE-only**: classic-Bluetooth
(PIN-pairing) adapters instead need the `elecrow_obd` build on a classic-ESP32 board, which
connects to a stored adapter MAC. To switch adapters later, use **Forget adapter** in the
settings menu.

> **Adapter compatibility.** Choose a **BLE / "Bluetooth 4.0"** ELM327 — iOS-compatible
> dongles are BLE, and most use the `0xFFE0` (CC2541/HM-10) or `0xFFF0` chipset and connect
> with no pairing step. **Validated on the dash:** Vgate **vLinker MS** (BLE). **Should
> work** (BLE with a standard GATT profile and in the adapter name-hint list, but not
> separately bench-tested): the Vgate **iCar Pro BLE 4.0** and generic CC2541/`0xFFE0`
> dongles. **Known-unsupported:** the **OBDLink MX+ / CX** — its BLE is proprietary (it
> advertises with no name and exposes no standard ELM327 GATT service, and appears to require
> OBDLink's own app to unlock), so the dash cannot connect to it. Classic-Bluetooth
> (PIN-pairing) adapters are not supported on the BLE build — use the `elecrow_obd` build
> instead. *(Note: the Vgate iCar Pro also comes in a WiFi variant, documented separately —
> this refers to the BLE 4.0 model.)*

**5. Set the clock** (optional) via **Set date/time** in the menu — it backs up to the coin
cell, so you only do it once.

That's it. Gauges appear once the adapter links. From here, updates are over-the-air (below)
— no cable.

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
