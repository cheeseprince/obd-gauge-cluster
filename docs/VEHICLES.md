# Vehicle support

One firmware image carries every vehicle profile. This page covers how the display picks one
automatically, what each profile currently supports, and what "not supported" actually means.

## How a vehicle is chosen

On first OBD connect, the firmware requests the VIN over **Mode-09 PID `0902`** and parses the
17-character VIN out of the reassembled multi-frame ISO-TP reply (`src/vin.cpp:123-130`,
`parseVinReply`). The first three characters — the WMI — are looked up in a fixed WMI→profile
table: `1GT`/`3GT`/`1GC`/`3GC` → `gm_sierra_lz0`, `WBA`/`WBS`/`5UX`/`4US` → `bmw_f10_535i`,
`WAU`/`WA1`/`WUA`/`TRU` → `audi_q5` (`src/vin.cpp:132-147`, `vinToProfileKey`). An unrecognized
WMI returns no match (`src/vin.cpp:152-153`), and the display falls back to the Generic
profile.

**Settings → Pick Vehicle** overrides this and locks the choice: with auto-select off, VIN-based
switching is skipped entirely, regardless of what VIN is read on the wire (`src/vin.cpp:150`,
the `vehicleAuto` gate in `vinAutoTarget`).

Example VIN shape used in this repo's own tests (synthetic, not a real vehicle):
`1GT0123456789ABCD`.

## Audi

Ships as a skeleton profile (`src/vehicles/audi_q5.cpp`), auto-detected by VIN (WMI
`WAU`/`WA1`/`WUA`/`TRU`). Standard Mode-01 stats are exact. Three enhanced DIDs are wired —
ATF/gearbox temp, oil temp, charge-air pressure — but their scaling is a **best-guess, unverified
on-car**, so they ship with **alarms off**. See [`AUDI-STATUS.md`](AUDI-STATUS.md) for the
research behind those DIDs and what a confirmation drive would need to establish.

3 pages, 12 tiles (`audi_q5.cpp:139`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | TEMPS | Trans · Oil · Coolant · Boost |
| 2 | DRIVE | Rpm · Speed · Load · Volts |
| 3 | AIR | Intake · Pedal · Maf · FuelLevel |

## BMW

Ships as a skeleton profile (`src/vehicles/bmw_f10_535i.cpp`), auto-detected by VIN (WMI
`WBA`/`WBS`/`5UX`/`4US`). Standard Mode-01 stats are exact, and boost is derived from standard
MAP rather than an enhanced DID. Oil pressure is mapped, but its scale is an **unverified best
guess**, so it ships with **alarms off**.

Two parameters are missing, for two *different* reasons:

- **ATF / gearbox temp is unreachable.** It lives on the EGS module, which is only addressable
  through the enhanced `6F1` path — and that path is gateway-blocked on this car. No drive
  will fix this; it needs different addressing.
- **Oil temp is reachable but unpinned.** Its candidate DIDs answer on the standard `7DF`
  functional broadcast, not through `6F1`. They read plausibly but were only ever sampled on a
  warm-started drive, so there is no cold-to-warm ramp to identify which candidate is actually
  oil temperature. A dedicated **cold-start** drive settles it.

See [`BMW-STATUS.md`](BMW-STATUS.md) for the on-car scan results.

3 pages, 11 tiles — the last page has one empty cell (`bmw_f10_535i.cpp:147`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | ENGINE | Coolant · OilP · Boost · Load |
| 2 | DRIVE | Rpm · Speed · Intake · Ambient |
| 3 | MISC | Baro · FuelLevel · Volts · *(empty)* |

## Chevrolet / GMC

Chevrolet and GMC share the same `gm_sierra_lz0` profile — same LZ0/Global-B 3.0L Duramax
powertrain. **GMC Sierra 1500 (2023–2026, LZ0): working, validated on a real truck** — every
enhanced PID was measured, not guessed. **Chevrolet Silverado 1500 (2023–2026, LZ0): expected
to work**, same LZ0/Global-B as the Sierra, but validated only on the GMC, not separately on a
Silverado.

7 pages, 27 tiles — the last page has one empty cell (`gm_sierra_lz0.cpp:158`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | TOW | Trans · Coolant · OilP · Egt |
| 2 | POWER | Boost · Hp · Rpm · Load |
| 3 | REGEN | DpfDp · FuelRate · Nox · Rail |
| 4 | RANGE | FuelLevel · DslFill · Def · DefFill |
| 5 | TRIP | MpgInst · MpgAvg · Gal100mi · L100km |
| 6 | DIAG | Maf · Egr · Cac · Intake |
| 7 | MISC | Speed · Volts · Oil · *(empty)* |

## Ford

**No profile ships yet.** Ford has no WMI entry in `vinToProfileKey` — the comment at
`src/vin.cpp:142-143` notes a Ford row (e.g. `1FT`) returns no match "until that profile is
registered." A Ford vehicle currently lands on the Generic profile below: standard Mode-01
parameters populate, and the enhanced Power Stroke parameters (transmission temp, EGT, DPF,
fuel rail pressure, DEF, NOx) do not exist in this firmware yet. See
[`FORD-STATUS.md`](FORD-STATUS.md) for the research and what a truck scan session would need to
establish before that profile can be written.

## Generic

2 pages, 8 tiles (`generic_obd.cpp:81`).

The fallback profile for any VIN whose WMI isn't in the table above, or for any vehicle scanned
with **Settings → Pick Vehicle** left on auto. Standard Mode-01 only — no manufacturer-specific
parameters, no vehicle-tuned alarm thresholds.

2 pages, 8 tiles:

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | ENGINE | Rpm · Speed · Coolant · Load |
| 2 | AIR | Intake · Pedal · Maf · Volts |

Fuel level (`012F`) is polled live on this profile but scheduled as an internal helper value,
not shown as a tile — both pages are already full (`generic_obd.cpp:82-84`).

## What "not supported" means

Standard OBD-II Mode-01 parameters — RPM, vehicle speed, coolant temperature, engine load — are
legislated and identical across manufacturers, so they work on **any** vehicle, including one
with no profile at all (via the Generic profile above). What doesn't work without a profile is
the manufacturer-specific enhanced data: transmission temp, EGT, DPF pressure, oil pressure,
DEF, NOx, and similar. "Not supported" here means **no profile exists yet for that vehicle**,
not that the firmware refuses to run on it — plug it into any OBD-II-compliant vehicle and the
standard parameters populate immediately.

## Index

- [`AUDI-STATUS.md`](AUDI-STATUS.md) — Audi Q5 research and on-car DID capture
- [`BMW-STATUS.md`](BMW-STATUS.md) — BMW F10 535i research and on-car scan results
- [`FORD-STATUS.md`](FORD-STATUS.md) — Ford 6.7L Power Stroke research
