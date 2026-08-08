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

### What a recognized-but-unprofiled vehicle actually shows

Being identified adds **a caption, not a reading**. Concretely, a recognized Ford, Ram or GM
pickup gets:

| | |
| :--- | :--- |
| **Boot splash** | Make and model (e.g. "Ford F-250"), plus the engine where the code is unambiguous ("6.7L Power Stroke") |
| **Gauges** | **Standard+** — every parameter SAE J1979 legislates, in a diesel or gas layout chosen from the VIN's engine code |
| **Alarm thresholds** | Off on every tile except coolant and volts — no thresholds have been sourced for these vehicles |
| **Manufacturer-specific data** | **None.** No transmission temp, DPF differential pressure, regeneration state or DEF level |

## Standard+

`src/vehicles/standard_plus.cpp`. Two profiles (`std_diesel`, `std_gas`) sharing one readout
table — the PID set is identical, only the pages differ.

| Page | Diesel | Gas |
| :--- | :--- | :--- |
| ENGINE | Rpm · Speed · Coolant · Load | Rpm · Speed · Coolant · Load |
| THERMAL | Oil · EGT · Intake · Ambient | Oil · Intake · Ambient · Baro |
| POWER | ActTq · RefTq · FuelRate · Rail | ActTq · FuelRate · Maf · Volts |
| AIR | Baro · EGR · NOx · Volts | — |

| PID | Parameter | Scaling (from the standard) |
| :--- | :--- | :--- |
| `015C` | Engine oil temperature | `A − 40` °C |
| `0178` | Exhaust gas temperature | `(A·256+B)/10 − 40` °C per sensor |
| `015E` | Engine fuel rate | `(A·256+B)/20` L/h |
| `0123` | Fuel rail gauge pressure | `(A·256+B)·10` kPa |
| `0162` | Actual engine percent torque | `A − 125` % |
| `0163` | Engine reference torque | `A·256+B` N·m |
| `0133` | Absolute barometric pressure | `A` kPa |
| `0146` | Ambient air temperature | `A − 40` °C |
| `012C` | Commanded EGR | `A·100/255` % |
| `0183` | NOx sensor | `(B·256+C)` ppm |

**Why this is allowed to exist without a scan.** Every other profile in `src/vehicles/` was
built from a real vehicle — see the header of `jeep_ws.cpp`. Standard+ is different in kind:
these are *legislated* PIDs whose scaling is defined by SAE J1979, not manufacturer-specific
DIDs and not guesses. The decoders are the same ones already proven against the Sierra.

**It cannot show a wrong number.** A vehicle that does not support a PID answers NO DATA and the
tile stays blank. Blank is honest; a wrong reading is not. So listing a PID a given truck lacks
costs an empty tile, never a misleading one — which is exactly why an unscanned vehicle may
safely be given this profile and may not be given an invented one.

**Diesel vs gas comes from `vin[7]`**, the engine code the identity lookup already reads. An
engine we could not name falls back to the gas layout: diesel-only tiles on a gas truck look
broken, while the reverse merely omits data the truck does not have.

**A scanned profile always wins.** A Sierra 1500 3.0L Duramax keeps `gm_sierra_lz0`; only
vehicles with no scanned profile fall through to Standard+. One consequence worth noting: a
**gas** Sierra now moves *off* the Duramax profile onto Standard+ gas, where previously it was
left alone. That is a deliberate improvement — the Duramax profile's enhanced tiles can never
populate on a gas engine.

⚠️ **Not hardware-validated.** Standard+ is verified by host tests and builds, but no Ford, Ram
or gas/HD GM truck has yet run it. Which of these PIDs a given truck actually answers is
unknown until someone plugs one in — the tiles that go blank are the ones to report.

**Identity and profile selection are separate.** `vinIdentify()` answers "what is this
vehicle", `vinToProfileKey()` answers "which gauge profile does it get", and a vehicle can be
the first without being the second. That is the case for the Ford Super Duty below: the dash
names the truck on its boot splash while still running Generic gauges, instead of showing
nothing merely because no profile exists. The detected name is persisted so it survives a
power cycle — the splash is drawn before the OBD link is up. The **strings** are stored, not
the VIN.

### How these patterns were verified

Every pattern was derived from **NHTSA vPIC** (US Government, public domain) by decoding
partial VINs — never guessed from a VIN-decoder blog. The method is a sweep: hold a known-good
seed VIN, vary one position across the whole 33-character VIN alphabet, and record what Model,
Series, Displacement and Fuel come back. A position whose value never changes the answer is
noise and **must not** become a gate, because a needless gate silently rejects valid trucks.

Two things that sweep caught, both of which had already shipped or nearly shipped:

- **Reading the series from the wrong position.** The first Ford table used `vin[3]`, which
  does not encode the series at all — `7`, `8`, `B` and `R` all decode as F-250. It appeared
  to work only because real VINs happen to correlate `vin[3]` with the true series digit at
  `vin[5]`.
- **Trusting a one-position sweep.** Varying a single position gives an answer valid only for
  that seed's other positions. "`vin[5]` is the series" holds for a Super Duty seed but breaks
  against an F-150 seed, because positions 3–5 interact.

vPIC also returns **self-contradictory results for under-specified partial VINs** (a `Model`
of F-150 alongside a `Series` of "Super Duty — Dual Rear Wheel"). Model and Series agreeing is
used as a validity check; where they disagree the combination is treated as unverified.

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

| Profile | WMI | Positions 3–7 | Model year `vin[9]` | What it excludes |
| :--- | :--- | :--- | :--- | :--- |
| `gm_sierra_lz0` | `1GT` `3GT` `1GC` `3GC` | `[NPRUV]` `[HU]` `*` `*` `8` | **`P R S T`** (2023–26) | gas L84 / L87 / L3B, Sierra HD, **2011–12 Express van** |
| `bmw_f10_535i` | `WBA` `WBS` `5UX` `4US` | `F` `[RU]` `7` `C` `5` | — (frame is year-unique) | 528i, 550i, M cars, X-series |
| `audi_q5` | `WAU` `WA1` `WUA` `TRU` | `[ABC]` `N` `*` `F` `Y` | — (frame is year-unique) | SQ5 3.0 V6, Q3, Q7, Q8, 2021+ facelift |
| `jeep_ws` | **`1C4` only** | `*` `J` `[RSUV]` `[ABD]` `T` | **`N P`** (2022–23) | Grand Wagoneer, Grand Cherokee, Cherokee, Wrangler, **Ducato / ProMaster vans**, **Voyager 2001–03** |

Two of these are worth understanding before changing them:

- **BMW cannot be discriminated on displacement.** The 528i is also 3.0 L / 6-cylinder in
  2011–13, so only the model digit `vin[5]` separates it from the 535i's N55.
- **The Jeep WMIs are Stellantis-wide, not Jeep-wide.** `vin[4]='F'` is a Fiat Ducato and
  `'R'` is a Ram ProMaster. Without the predicate, a work van would have been driven with the
  Wagoneer's 29-bit addressing.

**Two of these needed an explicit model-year check, and two did not.** That distinction was
established by sweeping vPIC across every model-year code, not assumed — an earlier version of
this document claimed all four patterns were "self-limiting in time", and that was **wrong for
GM and Jeep**:

- **BMW and Audi genuinely are self-limiting.** The BMW frame decodes to nothing from 2016
  (F10 → G30) and the Audi one from 2021 (the facelift moved the Q5 to `vin[4]='A'`). Verified
  against every year code: neither matches anything unexpected.
- **GM was not.** A **2011–12 Chevrolet Express van with the 6.6 L Duramax** satisfies
  `1GC` + `[NPRUV]` + `H` + `8` exactly. A working van is likelier to meet an OBD dongle than
  most false positives.
- **Jeep was not.** The Wagoneer rule also matched a Chrysler **Voyager** (2001–03).

`vin[9]` is VIN position 10, the model-year code: `N`=2022, `P`=2023, `R`=2024, `S`=2025,
`T`=2026. **Year codes repeat every 30 years** (`N` is also 1992), so the gate alone cannot
separate a 1992 vehicle from a 2022 one on the same WMI — which is why `jeep_ws` also dropped
`1J4` and `3C4`. At the Wagoneer year codes vPIC resolves `1J4` to a 1992–96 Cherokee, and
`3C4` to no vehicle at all; only `1C4` is a Wagoneer.

A model year outside the verified range **fails closed**: a 2027 truck gets the Generic profile
until somebody confirms the profile still fits it.

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
`3GTUUEE80S2345678` — note that the older `1GT0123456789ABCD` now correctly resolves to *no
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

**Identified, but no profile ships yet — those are two different things.**

A Super Duty or F-150 is recognized by VIN and the dash captions itself on the boot splash.
It still runs the **Generic** profile: standard Mode-01 parameters populate, and the enhanced
Power Stroke parameters (transmission temp, EGT, DPF, fuel rail pressure, DEF, NOx) do not
exist in this firmware yet. Naming the truck is not the same as reading it — nothing here has
been measured on a Power Stroke.

| Positions | Meaning |
| :--- | :--- |
| `1FT` / `3FT` | Ford truck, US- or Mexico-built |
| `vin[5]` | **series** — `1`=F-150, `2`=F-250, `3`=F-350, `4`=F-450, `5`=F-550 |
| `vin[7]` | engine (Super Duty only) — `T`=6.7L Power Stroke, `6`=6.2L V8, `N`=7.3L V8 |

`vin[5]` is the series digit — position 6 in Ford's 1-indexed VDS layout. **`vin[3]`, `vin[4]`
and `vin[6]` are deliberately not gated:** they encode a class code, the cab/body style and
the drive type. An earlier version of this table read the series from `vin[3]` and required
`vin[4]='W'` and `vin[6]='B'`, which rejected every 4x2 Super Duty and every cab style but
one. See the note on verification below — that bug is why the method changed.

**Engine codes are per line, not per make.** `vin[7]='T'` is a 6.7L Power Stroke on a Super
Duty and a **3.5L EcoBoost** on an F-150. The F-150 therefore ships with **no engine string**:
its codes are year-dependent and only sparsely confirmed, so the dash names the truck and says
nothing about what is under the hood.

Verified spans: **Super Duty 2011–2026**, **F-150 2010–2023**. Outside those, unidentified.

## Ram

**Identified, no profile.** `1C6` / `3C6`, with `vin[4]='R'` as a **brand gate — the `1C6` WMI
is shared with Jeep**, so without it a Grand Cherokee would be captioned as a pickup.

| Positions | Meaning |
| :--- | :--- |
| `vin[5]` | series — `6`,`7`,`B`,`E`,`F`=1500 · `4`,`5`=2500 · `2`,`3`=3500 |
| `vin[7]` | engine — `T`=5.7L HEMI · `L`=6.7L Cummins · `G`=3.6L V6 · `J`=6.4L HEMI · `9`=6.2L HEMI · `M`=3.0L EcoDiesel |

The 1500 is a **set** of series codes, not one. Verified span: **2013–2024**.

## Chevrolet / GMC (beyond the profiled Sierra)

The Sierra 1500 3.0L Duramax has a full profile (above). Every other GM pickup in the verified
span is **identified only** — named on the splash, running Generic gauges.

| Line | Gate | Engine `vin[7]` | Verified |
| :--- | :--- | :--- | :--- |
| Sierra / Silverado **1500** | `vin[3]∈NPRUV`, `vin[4]∈HU` | `8`=3.0L Duramax · `D`=5.3L V8 · `K`=2.7L I4 Turbo · `L`=6.2L V8 | 2022–2026 |
| Sierra / Silverado **HD** | `vin[3]∈0–5`, `vin[4]∈{8,9}` | `Y`=6.6L Duramax · `7`=6.6L V8 | 2020–2024 |

**HD is not split into 2500 and 3500.** That distinction is a joint function of `vin[4]`+`vin[5]`
which vPIC resolves for only 16 of 1,089 combinations — not enough to be confident, so the dash
says "Sierra HD" and stops rather than guessing the tonnage.

## Jeep## Jeep

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
