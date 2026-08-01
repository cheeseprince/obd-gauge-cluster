# Adapter compatibility

Which Bluetooth OBD-II adapter to buy, why some are unsupported, and how the firmware
picks one out of a crowded parking-lot scan.

## Short answer

Buy a **BLE / "Bluetooth 4.0"** ELM327. The default `crowpanel_obd` build only speaks BLE —
classic-Bluetooth (PIN-pairing) adapters are **not supported at all** — that path was on a different
board (see the matrix below).

## The matrix

| Adapter | Transport | Build | Status | Notes |
| :--- | :--- | :--- | :--- | :--- |
| Vgate vLinker MS | BLE | `crowpanel_obd` | **Validated on the dash** | **Needs a one-time mode switch before it works:** it ships Classic/MFi-only and will not advertise over BLE until you set it to **BT+BLE** with Vgate's own updater app on a phone. Until then the dash scan never sees it, which is indistinguishable from a broken dash. Once switched — GATT captured on the truck: service `0x18f0`, notify `0x2af0`, write `0x2af1`, the first profile the firmware tries (the `SVC_UUID` / `NOTIFY_UUID` / `WRITE_UUID` constants, and the `PROFILES`
table in `bindChars`). |
| Vgate iCar Pro BLE 4.0 | BLE | `crowpanel_obd` | **Validated on the dash** | **Works out of the box** — no mode switch, unlike the vLinker MS. Confirmed on firmware v0.1.2, 2026-07-27. Standard GATT profile, and `icar` is in the name-hint list (`bleNameLooksLikeObd` in `src/ble_rank.cpp`), so it ranks ahead of unrelated BLE devices during scan. See the WiFi-variant footnote below — buy the **BLE 4.0** model, not the WiFi one. |
| Generic CC2541 / `0xFFE0` / `0xFFF0` clones | BLE | `crowpanel_obd` | Should work — not bench-tested | Firmware tries both `0xFFF0` (3-characteristic) and `0xFFE0` (notify+write share one `0xFFE1` characteristic) profiles (the `PROFILES` table in `bindChars`). Coded from spec, never confirmed on hardware. |
| Any PIN-pairing classic-BT ELM327 | Classic BT | — | ❌ **Unsupported** | The CrowPanel Advance is an ESP32-**S3**, which has no classic-Bluetooth radio — BLE only, in hardware. A classic-BT path existed on the retired ESP32-WROVER board and was removed with it. There is no build that will talk to these adapters. |
| OBDLink MX+ / CX | BLE (proprietary) | — | **Unsupported** | Investigated and abandoned 2026-07-25. See below — this is not a missing-GATT-profile bug the project can fix from source alone. |

**Do not upgrade any verdict above.** "Should work" means coded against the adapter's published
GATT spec and never tested against the physical device — a real bench/road test is the only thing
that moves a row from "should work" to "validated."

## Supported GATT profiles

`BleObdSource::bindChars()` tries four known BLE-ELM327 GATT layouts in order, and binds the
first one whose service and both characteristics exist on the connected device
(`bindChars` in `src/ble_obd_source.cpp`):

| Order | Tag | Service UUID | Notify (device→client) | Write (client→device) |
| :- | :--- | :--- | :--- | :--- |
| 1 | `vlinker 18f0` | `0x18f0` | `0x2af0` | `0x2af1` |
| 2 | `clone fff0` | `0xfff0` | `0xfff1` | `0xfff2` |
| 3 | `clone ffe0` | `0xffe0` | `0xffe1` | `0xffe1` (shared) |
| 4 | `nordic-uart` | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |

Profile 1 (`0x18f0`) is the vLinker MS's actual GATT layout, captured on the truck (comment at
the `SVC_UUID` constant in `src/ble_obd_source.cpp`). Profiles 2–3 cover the common CC2541/HM-10-family clones. Profile 4
is the Nordic UART Service, present on some BLE-ELM327 clones built on Nordic silicon. If none of
the four match, `bindChars()` returns false (`bindChars`) and its caller
disconnects and moves on to the next scanned candidate (`connectAndSetup`).

These four are **exactly** what the firmware tries — nothing else is in scope for `crowpanel_obd`.

## How adapters are chosen

On every connect attempt without a cached address, the firmware scans for 6 seconds, then ranks
every device seen before trying to connect (`connectAndSetup` in `src/ble_obd_source.cpp`, and `src/ble_rank.cpp`).

**Ranking is name-hint first, RSSI second.** `bleNameLooksLikeObd()` checks the advertised name
(case-insensitive substring) against a fixed hint list:

```
"obd", "vlink", "elm", "icar", "veepeak", "konnwei", "carista", "obdlink"
```

(`bleNameLooksLikeObd` in `src/ble_rank.cpp`, quoted verbatim). Any device whose name contains one of these sorts
ahead of every device that doesn't — regardless of signal strength — and within each group,
stronger RSSI sorts first (`rankKey` in `src/ble_rank.cpp`). The sort is a stable insertion sort, so
devices with identical rank keep their scan order (`bleRankCandidates` in `src/ble_rank.cpp`).

The rationale, from the source comment: the adapter is plugged in right next to the board, but in
a crowded RF environment (a parking lot) a strong nearby phone or watch shouldn't be tried ahead
of a named OBD dongle (`connectAndSetup`). After ranking, the firmware attempts to
connect to the top 12 candidates in order, stopping at the first one that both connects and binds
one of the four GATT profiles above (`connectAndSetup`).

Note that `obdlink` is in the hint list — the ranking would happily try an OBDLink device first if
it advertised a matching name. It doesn't; see below.

## Why the OBDLink MX+ / CX cannot work

Investigated and abandoned per field testing on 2026-07-25; the findings below are **not
independently verifiable from this repository's source** — they come from bench notes on the
physical adapter, not from anything checkable in `src/`:

- **It advertises with no name.** The scan list would show it as a bare MAC address among ~26
  other devices in a typical scan — nothing for the name-hint ranking to catch, so it sorts
  purely on RSSI and may not even appear in the top 12 tried.
- **None of the four GATT profiles above bound to it.** The dash's own scan-and-connect attempt
  never found a matching service among the four profiles this firmware tries. This was a
  dash-side connect attempt, not a full GATT service enumeration — the adapter was never
  inspected with a general-purpose BLE tool, so whether it exposes some *other*, unrelated
  service is unknown.
- **LightBlue on iOS could not connect to it either.** This was checked independently of this
  firmware, using a generic BLE inspector app, as a sanity check against "the firmware's NimBLE
  stack is doing something wrong" — ruling that out as the cause without itself being a full
  GATT enumeration. The adapter's BLE service appears to require OBDLink's own app to unlock.

This is not a missing-profile problem the project can fix by adding a fifth entry to the
`PROFILES` table. **It will not be revisited without an actual nRF Connect or LightBlue GATT dump
from the device** — i.e., someone with an MX+/CX in hand running a full service/characteristic
enumeration (which has not yet been done), so a real profile (if one exists to unlock) can be
identified before writing any code against it.

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
