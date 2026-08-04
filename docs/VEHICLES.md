# Vehicle support

One firmware image carries every vehicle profile. This page covers how the display picks one
automatically, what each profile currently supports, and what "not supported" actually means.

## How a vehicle is chosen

On first OBD connect, the firmware requests the VIN over **Mode-09 PID `0902`** and parses the
17-character VIN out of the reassembled multi-frame ISO-TP reply (`parseVinReply` in `src/vin.cpp`). The first three characters — the WMI — are looked up in a fixed table
(`MAP` in `vinToProfileKey`, `src/vin.cpp`): `1GT`/`3GT`/`1GC`/`3GC` → `gm_sierra_lz0`,
`WBA`/`WBS`/`5UX`/`4US` → `bmw_f10_535i`, `WAU`/`WA1`/`WUA`/`TRU` → `audi_q5`,
`1C4`/`1J4`/`3C4` → `jeep_ws`. An unrecognized WMI returns no match
(`vinToProfileKey` returns `nullptr`), and the display falls back to the Generic profile.

**A WMI alone is often not enough.** A WMI identifies a manufacturer and a broad vehicle class,
not an engine — so a row may carry an **optional extra predicate** over the full VIN. If that
predicate fails, the row fails *closed* (`nullptr`) rather than falling through to another row:
a WMI belongs to one manufacturer, so a failed discriminator means "cannot tell", not "try the
next entry". Failing closed matters because a wrong profile is worse than no profile — the dash
polls enhanced PIDs at headers that do not exist on that vehicle, and the tiles read nothing or,
worse, something misinterpreted.

**The GM rows use one.** `1GT`/`3GT`/`1GC`/`3GC` are GM light-truck-wide: they also cover the
gasoline Silverado/Sierra 1500 (L84 5.3 V8, L87 6.2 V8, L3B 2.7 turbo-4) and Sierra/Silverado HD
(L5P 6.6 Duramax), none of which this profile fits. NHTSA's **vPIC** database keys the 1500's
engine off **VIN position 8** (`vin[7]`), gated on positions 4–5:

| Pattern | Engine | Profile |
| :--- | :--- | :--- |
| `[NPRUV][HU]**8` | LZ0 3.0L I6 turbo diesel | `gm_sierra_lz0` |
| `[NPRUV][HU]**D` | L84 5.3L V8 gas | none — Generic |
| `[NPRUV][HU]**L` | L87 6.2L V8 gas | none — Generic |
| `[NPRUV][HU]**K` | L3B 2.7L I4 turbo gas | none — Generic |
| `[NPRUV][89]*E` | Sierra/Silverado HD | none — Generic |

**Every profile row now carries one.** All four were derived from
[NHTSA vPIC](https://vpic.nhtsa.dot.gov/) — a US Government database, public domain, and the
authoritative source for which VIN positions encode which attribute:

| Profile | Positions 3–7 | Verified years | What it now excludes |
| :--- | :--- | :--- | :--- |
| `gm_sierra_lz0` | `[NPRUV]` `[HU]` `*` `*` `8` | — | L84 5.3 / L87 6.2 / L3B 2.7 gas, Sierra HD |
| `bmw_f10_535i` | `F` `[RU]` `7` `C` `5` | 2011–2015 | 528i, 550i, M cars, X-series |
| `audi_q5` | `[ABC]` `N` `*` `F` `Y` | 2018–2020 | SQ5 3.0 V6, Q3, Q7, Q8, 2021+ facelift |
| `jeep_ws` | `*` `J` `[RSUV]` `[ABD]` `T` | 2022–2023 | Grand Wagoneer, Grand Cherokee, Cherokee, Wrangler, **Ducato / ProMaster vans** |

Two of these are worth understanding before changing them:

- **BMW cannot be discriminated on displacement.** The 528i is also 3.0 L / 6-cylinder in
  2011–13, so only the model digit `vin[5]` separates it from the 535i's N55.
- **The Jeep WMIs are Stellantis-wide, not Jeep-wide.** `vin[4]='F'` is a Fiat Ducato and
  `'R'` is a Ram ProMaster. Without the predicate, a work van would have been driven with the
  Wagoneer's 29-bit addressing.

Each pattern is also self-limiting in time: the BMW frame decodes to nothing from 2016 (F10 →
G30), the Audi one from 2021 (facelift moved the Q5 to `vin[4]='A'`), and the Jeep engine code
`T` disappears in 2024 (3.0 Hurricane). None of them needs an explicit model-year check.

**Adding or tightening one.** Derive the discriminator from vPIC **offline** — decode partial
VINs (8 characters plus `modelyear` is enough) and sweep one position at a time to see which
positions change the reported model or engine. Commit only the resulting predicate: the firmware
should carry a handful of character comparisons, never a decoding table. Then add both a
positive and a negative case to `test/test_vin.cpp`, and add any new synthetic VIN to
`ALLOWED_VINS` in `scripts/check_no_pii.py`.

**Settings → Pick Vehicle** overrides all of this and locks the choice: with auto-select off,
VIN-based switching is skipped entirely, regardless of what VIN is read on the wire (the
`vehicleAuto` gate in `vinAutoTarget`, `src/vin.cpp`).

**Never commit a real VIN.** Every VIN in this repository is synthetic and allowlisted; CI fails
on any VIN-shaped token that is not. The example shape used in the tests is
`3GTUUEE8012345678` — note that the older `1GT0123456789ABCD` now correctly resolves to *no
match*, because `vin[3]` is `0` rather than one of `[NPRUV]`.

## Audi

Ships as a skeleton profile (`src/vehicles/audi_q5.cpp`), auto-detected by VIN (WMI
`WAU`/`WA1`/`WUA`/`TRU`). Standard Mode-01 stats are exact. Three enhanced DIDs are wired —
ATF/gearbox temp, oil temp, charge-air pressure — but their scaling is a **best-guess, unverified
on-car**, so they ship with **alarms off**. See [`AUDI-STATUS.md`](AUDI-STATUS.md) for the
research behind those DIDs and what a confirmation drive would need to establish.

3 pages, 12 tiles (`AUDI_PAGES` / `AUDI_PAGE_NAMES` in `audi_q5.cpp`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | TEMPERATURES | Trans · Oil · Coolant · Boost |
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

3 pages, 11 tiles — the last page has one empty cell (`PAGES` / `PAGE_NAMES` in `bmw_f10_535i.cpp`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | ENGINE | Coolant · OilP · Boost · Load |
| 2 | DRIVE | Rpm · Speed · Intake · Ambient |
| 3 | MISCELLANEOUS | Baro · FuelLevel · Volts · *(empty)* |

## Chevrolet / GMC

Chevrolet and GMC share the same `gm_sierra_lz0` profile — same LZ0/Global-B 3.0L Duramax
powertrain. **GMC Sierra 1500 (2023–2026, LZ0): working, validated on a real truck** — every
enhanced PID was measured, not guessed. **Chevrolet Silverado 1500 (2023–2026, LZ0): expected
to work**, same LZ0/Global-B as the Sierra, but validated only on the GMC, not separately on a
Silverado.

7 pages, 27 tiles — the last page has one empty cell (`PAGES` / `PAGE_NAMES` in `gm_sierra_lz0.cpp`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | TOWING | Trans · Coolant · OilP · Egt |
| 2 | POWER | Boost · Hp · Rpm · Load |
| 3 | REGENERATION | DpfDp · FuelRate · Nox · Rail |
| 4 | RANGE | FuelLevel · DslFill · Def · DefFill |
| 5 | TRIP | MpgInst · MpgAvg · Gal100mi · L100km |
| 6 | DIAGNOSTICS | Maf · Egr · Cac · Intake |
| 7 | MISCELLANEOUS | Speed · Volts · Oil · *(empty)* |

## Ford

**No profile ships yet.** Ford has no WMI entry in `vinToProfileKey` — the comment at
the `MAP` table in `vinToProfileKey` notes a Ford row (e.g. `1FT`) returns no match "until that profile is
registered." A Ford vehicle currently lands on the Generic profile below: standard Mode-01
parameters populate, and the enhanced Power Stroke parameters (transmission temp, EGT, DPF,
fuel rail pressure, DEF, NOx) do not exist in this firmware yet. See
[`FORD-STATUS.md`](FORD-STATUS.md) for the research and what a truck scan session would need to
establish before that profile can be written.

## Jeep

Ships as a skeleton profile (`src/vehicles/jeep_ws.cpp`), auto-detected by VIN (WMI
`1C4`/`1J4`/`3C4`). Mapped from a **real on-car scan** of a **2022 Jeep Wagoneer (WS platform,
5.7L Hemi eTorque, ZF 8HP75)** — the first port where the scan came before the profile rather
than after. Fourteen standard Mode-01 parameters are measured live and are exact.

**This vehicle's addressing is unlike every other profile: the 11-bit path is completely dead.**
`7DF` and `7E0` both return `NO DATA` with zero supported PIDs, while 29-bit `18DB33F1` carries
53 Mode-01 PIDs and the transmission answers at `18DA18F1`. Everything — even legislated
parameters — is 29-bit only, so this profile's addressing emitters send 29-bit headers
unconditionally.

Two enhanced transmission DIDs answer, and both ship with **alarms off** for different reasons:

- **Trans temp is reachable but ambiguous.** `2204FE` returns **three** bytes (`6F 75 76` →
  71/77/78 °C) where the Grand Cherokee signalset implies one — most likely sump, converter-out
  and cooler-out. Byte A is displayed; **which byte is the sump is unverified**. A cold-start
  warm-up drive settles it.
- **Gear is reachable but unmapped.** `22051A` returned `DD`, matching a 2024 capture that
  recorded `DD` in Park. One sample of one gear is not a gear enum, so the tile shows the **raw
  byte** rather than an invented mapping — drive through every gear and the enum writes itself.

Engine oil temp and MAF are **not available**: PIDs `0x5C` and `0x10` are positively absent from
the census bitmask (the Hemi is speed-density, using `0x0B` MAP instead). Being naturally
aspirated, it also has no boost, DPF, DEF, EGT or NOx.

See [`JEEP-STATUS.md`](JEEP-STATUS.md) for the full scan results and confidence table.

4 pages, 16 tiles (`JEEP_PAGES` / `JEEP_PAGE_NAMES` in `jeep_ws.cpp`):

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | TEMPERATURES | Trans · Coolant · Intake · Ambient |
| 2 | DRIVE | Rpm · Speed · Load · Pedal |
| 3 | POWER | Torque · RefTq · Hp · Fuel rate |
| 4 | MISCELLANEOUS | Gear · FuelLevel · Volts · Baro |

The POWER page is why this skeleton is richer than the BMW and Audi ones — torque, reference
torque and fuel rate are all measured live, and **Hp** is computed from actual torque, reference
torque and RPM rather than read. Fuel rate also feeds the trip-economy integrator, so the
computed MPG values go live even though this layout does not show them.

## Generic

2 pages, 8 tiles (`GEN_PAGES` / `GEN_PAGE_NAMES` in `generic_obd.cpp`).

The fallback profile for any VIN whose WMI isn't in the table above, or for any vehicle scanned
with **Settings → Pick Vehicle** left on auto. Standard Mode-01 only — no manufacturer-specific
parameters, no vehicle-tuned alarm thresholds.

2 pages, 8 tiles:

| # | Page | Tiles |
| :- | :--- | :--- |
| 1 | ENGINE | Rpm · Speed · Coolant · Load |
| 2 | AIR | Intake · Pedal · Maf · Volts |

Fuel level (`012F`) is polled live on this profile but scheduled as an internal helper value,
not shown as a tile — both pages are already full (`GEN_HELPERS` in `generic_obd.cpp`).

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
- [`JEEP-STATUS.md`](JEEP-STATUS.md) — Jeep Wagoneer WS on-car scan: 29-bit-only addressing,
  the three-byte ATF reply, and the measured negatives
