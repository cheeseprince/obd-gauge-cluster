---
title: "Jeep Wagoneer (2022, 5.7L Hemi eTorque) — Support Status"
subtitle: "The first port mapped from a real on-car scan before any profile was written"
date: "2026-08-01"
---

# Short version

**A skeleton Jeep profile now ships** (`src/vehicles/jeep_ws.cpp`), auto-selected by VIN
(WMI `1C4`/`1J4`/`3C4` → `jeep_ws`). Unlike every previous port, it was written **after** a real
on-car scan rather than from a third-party capture: a census plus a 1024-probe sweep of a
**2022 Jeep Wagoneer (WS platform, 5.7L Hemi eTorque, ZF 8HP75)** on 2026-08-01.

Fourteen legislated Mode-01 parameters are **measured live**, and two enhanced DIDs on the
transmission **answer**: ATF temp `2204FE` and gear `22051A`. Both ship with **alarms off** — the
DIDs are confirmed, but what their bytes mean is only partly pinned down.

**The addressing is the headline finding, and it is unusual enough to be the main reason this
document exists.**

# The addressing: 11-bit is completely dead

| Header | Bits | Alive | Supported Mode-01 PIDs | Evidence |
| :--- | :--- | :--- | :--- | :--- |
| `7DF` (functional) | 11 | **No** | 0 | `NO DATA` to both `0100` and `22051A` |
| `7E0` (physical ECU 0) | 11 | **No** | 0 | `NO DATA` to both `0100` and `22051A` |
| `18DB33F1` (functional) | 29 | **Yes** | **53** | three ECUs answered `0100` |
| `18DA18F1` (TCM) | 29 | **Yes** | 12 | `0100` → `410098180001` |

This is not "the transmission moved." **The entire 11-bit diagnostic path returns nothing on this
vehicle.** Every parameter the display shows — including standard, legislated ones like RPM and
coolant — is reachable only over 29-bit CAN. A profile that targeted `7DF` would show an empty
dash and look identical to a broken adapter.

Two consequences are baked into `jeep_ws.cpp`:

1. **The addressing emitters ignore the `can29` argument** and always emit the 29-bit headers.
   `gm_sierra_lz0.cpp` switches on that flag, but no caller in this firmware ever passes
   `can29 = true` (`obdBuildQuery` defaults it false in `obd_query.h` and nothing overrides it),
   so a GM-style ternary would always emit `7DF` and this profile would read nothing at all.
2. **Each header is a two-step sequence** — `AT CP 18` then `AT SH DB33F1` / `AT SH DA18F1` —
   mirroring the AT sequence the scanner actually proved on the vehicle. The ELM327's power-on
   default for `CP` is already `0x18`, so the first step is belt-and-braces; it makes the sequence
   self-describing rather than dependent on a default.

# What the scan established

## Confirmed live (Mode-01, on `18DB33F1`)

Load `0104` · Coolant `0105` · RPM `010C` · Speed `010D` · Intake `010F` · Pedal `0111` ·
Fuel level `012F` · Baro `0133` · Volts `0142` · Ambient `0146` · Fuel rate `015E` ·
Actual torque `0162` · Reference torque `0163`

## Measured negatives — absent, not merely unidentified

These were positively absent from the census's supported-PID bitmask, which reached that range
(`0x5E`, `0x60`, `0x62`, `0x63` are all present). This is a stronger claim than "not found yet."

| PID | Parameter | Why it matters |
| :--- | :--- | :--- |
| `0x10` | MAF | The Hemi is **speed-density** — `0x0B` (MAP) is present instead. |
| `0x5C` | Engine oil temp | **Corrects an earlier claim.** The first revision of the scanner preset asserted `015C` was supported on `DB33F1`. It is not. |

## Enhanced DIDs (TCM, on `18DA18F1`)

**Gear `22051A` → `62051A DD`.** Byte-exact with the 2024 Wagoneer capture, which recorded `DD`
in Park. That is **one sample of one gear**, so the firmware deliberately displays the **raw
byte** rather than inventing a gear enum. Shown raw it is still useful: drive through P-R-N-D and
every forward gear, watch the tile, and the enum writes itself.

**ATF temp `2204FE` → `6204FE 6F 75 76`.** This DID had **never been probed on a Wagoneer** — it
was the single reason the scanner preset was written. It answers. But it returns **three bytes**
where the Grand Cherokee signalset implies one:

| Byte | Raw | `A − 40` |
| :--- | :--- | :--- |
| A | `0x6F` | **71 °C** / 160 °F |
| B | `0x75` | **77 °C** / 171 °F |
| C | `0x76` | **78 °C** / 172 °F |

All three decode to plausible, tightly-clustered ZF 8HP temperatures — sump / converter-out /
cooler-out is the usual three-sensor arrangement on this gearbox. **Which byte is the sump is
undetermined** from a single warm, stationary sample. Byte A is wired to the TRANS tile with
alarms off, matching the Audi `222104` precedent. Bytes B and C are not discarded, just not
displayed.

# Confidence

| Claim | Confidence | Basis |
| :--- | :--- | :--- |
| 11-bit path is dead; 29-bit required | **High** | Direct measurement, both headers, two request types |
| The 14 Mode-01 PIDs are live | **High** | Census supported-PID bitmask |
| `0x10` / `0x5C` unsupported | **High** | Measured absence from the same bitmask |
| `2204FE` answers with 3 bytes | **High** | Raw reply captured |
| All three bytes are temperatures | **Medium-high** | `A−40` yields plausible clustered values for each; no alternative formula tested |
| Byte A is specifically the sump | **Low** | Wired by choice, not by evidence — alarms off for this reason |
| `0xDD` = Park | **Medium** | Two independent captures agree, but only for Park |
| eTorque BPCM is behind the Security Gateway | **Unverified** | Asserted from the 2024 capture; **never tested on-car**. A separate DTC sweep of nine modules found **zero** `securityAccessDenied`, so treat this as untested rather than confirmed. |

# What remains unknown

1. **Which ATF byte is the sump.** A **cold-start warm-up log** settles it — the sump lags the
   others warming up and leads them under load.
2. **The gear enum.** One drive through every gear position.
3. **Oil pressure, EGR, fuel rail.** No DID identified; the sweep only covered `2204xx`/`2205xx`.
4. **Thresholds.** No Hemi or 8HP75 limits have been sourced, which is why every tile except
   VOLTS ships with alarms off.
5. **The 11 errored probes.** The sweep logged 15 hits, 998 negatives and **11 transport errors**
   out of 1024. Those 11 are **undetermined, not negative** — if any fell in `2204xx`, a hit could
   have been missed. A re-sweep would close that 1.1% gap.

# The dash it produces

4 pages, 16 tiles (`JEEP_PAGES` / `JEEP_PAGE_NAMES` in `jeep_ws.cpp`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | TEMPS | Trans · Coolant · Intake · Ambient |
| 2 | DRIVE | Rpm · Speed · Load · Pedal |
| 3 | POWER | Torque · RefTq · Hp · Fuel rate |
| 4 | MISC | Gear · Fuel% · Volts · Baro |

**The POWER page is what makes this skeleton richer than the BMW and Audi ones**, and every
input on it was measured rather than guessed. Torque `0162`, reference torque `0163` and fuel
rate `015E` are all in the census bitmask. **HP** carries no command of its own — it is
`RF_COMPUTED`, derived by `updateComputedReadouts()` from actual torque, reference torque and
RPM, all three of which are now scheduled because they sit on a page.

Putting fuel rate on a page has a useful side effect: it starts the Economy integrator (which
consumes fuel rate + speed), so the computed **MPG** rows go live even though this layout does
not display them. A test asserts each computed tile's inputs are actually scheduled — an HP tile
fed by unpolled inputs would render a permanently blank gauge.

# Reproducing the scan

The scanner is read-only by construction; nothing is written to the car.

```
python3 -m obd_scan census --vehicle jeep -o census.json
python3 -m obd_scan sweep  --vehicle jeep --census census.json -o sweep.json
```

## What is needed from the car

- **About 20 minutes parked**, ignition on, laptop on the OBD port with a WiFi ELM327.
- **A cold-start drive** afterwards to resolve the ATF byte and the gear enum. Nothing is
  installed or modified.
