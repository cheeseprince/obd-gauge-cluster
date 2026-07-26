# Adapter compatibility

Which Bluetooth OBD-II adapter to buy, why some are unsupported, and how the firmware
picks one out of a crowded parking-lot scan.

## Short answer

Buy a **BLE / "Bluetooth 4.0"** ELM327. The default `crowpanel_obd` build only speaks BLE —
classic-Bluetooth (PIN-pairing) adapters need the separate `elecrow_obd` build on a different
board (see the matrix below).

## The matrix

| Adapter | Transport | Build | Status | Notes |
| :--- | :--- | :--- | :--- | :--- |
| Vgate vLinker MS | BLE | `crowpanel_obd` | **Validated on the dash** | GATT captured on the truck: service `0x18f0`, notify `0x2af0`, write `0x2af1` — the first profile the firmware tries (`src/ble_obd_source.cpp:17-20`, `:384`). |
| Vgate iCar Pro BLE 4.0 | BLE | `crowpanel_obd` | Should work — not bench-tested | Standard GATT profile, and `icar` is in the name-hint list (`src/ble_rank.cpp:21`), so it ranks ahead of unrelated BLE devices during scan. Never tried against real hardware. See the WiFi-variant footnote below. |
| Generic CC2541 / `0xFFE0` / `0xFFF0` clones | BLE | `crowpanel_obd` | Should work — not bench-tested | Firmware tries both `0xFFF0` (3-characteristic) and `0xFFE0` (notify+write share one `0xFFE1` characteristic) profiles (`src/ble_obd_source.cpp:385-386`). Coded from spec, never confirmed on hardware. |
| Any PIN-pairing classic-BT ELM327 | Classic BT | `elecrow_obd` | Needs the other board | The `crowpanel_obd` build is BLE-only; classic Bluetooth (SPP, PIN pairing) is a different radio stack entirely. `elecrow_obd` targets the retired ESP32-WROVER board with ELMduino over classic BT instead (`platformio.ini:52-90`). |
| OBDLink MX+ / CX | BLE (proprietary) | — | **Unsupported** | Investigated and abandoned 2026-07-25. See below — this is not a missing-GATT-profile bug the project can fix from source alone. |

**Do not upgrade any verdict above.** "Should work" means coded against the adapter's published
GATT spec and never tested against the physical device — a real bench/road test is the only thing
that moves a row from "should work" to "validated."

## Supported GATT profiles

`BleObdSource::bindChars()` tries four known BLE-ELM327 GATT layouts in order, and binds the
first one whose service and both characteristics exist on the connected device
(`src/ble_obd_source.cpp:382-406`):

| Order | Tag | Service UUID | Notify (device→client) | Write (client→device) |
| :- | :--- | :--- | :--- | :--- |
| 1 | `vlinker 18f0` | `0x18f0` | `0x2af0` | `0x2af1` |
| 2 | `clone fff0` | `0xfff0` | `0xfff1` | `0xfff2` |
| 3 | `clone ffe0` | `0xffe0` | `0xffe1` | `0xffe1` (shared) |
| 4 | `nordic-uart` | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |

Profile 1 (`0x18f0`) is the vLinker MS's actual GATT layout, captured on the truck (comment at
`src/ble_obd_source.cpp:17`). Profiles 2–3 cover the common CC2541/HM-10-family clones. Profile 4
is the Nordic UART Service, present on some BLE-ELM327 clones built on Nordic silicon. If none of
the four match, the connect attempt is abandoned and the firmware moves on to the next scanned
candidate (`src/ble_obd_source.cpp:406`).

These four are **exactly** what the firmware tries — nothing else is in scope for `crowpanel_obd`.

## How adapters are chosen

On every connect attempt without a cached address, the firmware scans for 6 seconds, then ranks
every device seen before trying to connect (`src/ble_obd_source.cpp:285-323`, `src/ble_rank.cpp`).

**Ranking is name-hint first, RSSI second.** `bleNameLooksLikeObd()` checks the advertised name
(case-insensitive substring) against a fixed hint list:

```
"obd", "vlink", "elm", "icar", "veepeak", "konnwei", "carista", "obdlink"
```

(`src/ble_rank.cpp:20-22`, quoted verbatim). Any device whose name contains one of these sorts
ahead of every device that doesn't — regardless of signal strength — and within each group,
stronger RSSI sorts first (`src/ble_rank.cpp:31-34`). The sort is a stable insertion sort, so
devices with identical rank keep their scan order (`src/ble_rank.cpp:39-48`).

The rationale, from the source comment: the adapter is plugged in right next to the board, but in
a crowded RF environment (a parking lot) a strong nearby phone or watch shouldn't be tried ahead
of a named OBD dongle (`src/ble_obd_source.cpp:307-310`). After ranking, the firmware attempts to
connect to the top 12 candidates in order, stopping at the first one that both connects and binds
one of the four GATT profiles above (`src/ble_obd_source.cpp:341-364`).

Note that `obdlink` is in the hint list — the ranking would happily try an OBDLink device first if
it advertised a matching name. It doesn't; see below.

## Why the OBDLink MX+ / CX cannot work

Investigated and abandoned 2026-07-25. Three independent findings, none of them fixable from this
project's source:

- **It advertises with no name.** The scan list would show it as a bare MAC address among ~26
  other devices in a typical scan — nothing for the name-hint ranking to catch, so it sorts
  purely on RSSI and may not even appear in the top 12 tried.
- **It exposes no standard ELM327 GATT service.** None of the four profiles above (or any other
  service visible during inspection) match what the MX+/CX presents.
- **LightBlue on iOS could not connect to it either.** This was checked independently of this
  firmware, using a generic BLE inspector app — ruling out "the firmware's NimBLE stack is doing
  something wrong" as the cause. The adapter's BLE service appears to require OBDLink's own app to
  unlock.

This is not a missing-profile problem the project can fix by adding a fifth entry to the
`PROFILES` table. **It will not be revisited without an actual nRF Connect or LightBlue GATT dump
from the device** — i.e., someone with an MX+/CX in hand exporting its full service/characteristic
list, so a real profile (if one exists to unlock) can be identified before writing any code
against it.

## Reporting a working adapter

Tested a BLE ELM327 not in the matrix above? Open an issue with:

- **Exact model** (brand, model number/name as printed on the adapter or its listing).
- **Advertised BLE name** — what the scan shows it as (check the on-screen scan list, or a tool
  like nRF Connect / LightBlue).
- **GATT service UUID** it exposes (from nRF Connect / LightBlue), especially if it's not one of
  the four already listed.
- **Whether it connected** — did the dash link up and start showing live gauges, or fail, and at
  which step (scan-visible, connect, GATT bind, ELM init)?

That's enough to add a new row to the matrix — either confirming another "should work" adapter as
validated, or documenting a fifth GATT profile if the device uses one this firmware doesn't yet
try.

---

*The Vgate iCar Pro also exists as a **WiFi** variant. This page — and the `crowpanel_obd` build —
only speak BLE; the firmware has no WiFi-OBD transport in the public build. Buy the **BLE 4.0**
model, not the WiFi one.*
