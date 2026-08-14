# Vehicle support

One firmware image carries every vehicle profile. This page covers how the display picks one
automatically, what each profile currently supports, and what "not supported" actually means.

## How a vehicle is chosen

On first OBD connect, the firmware requests the VIN over **Mode-09 PID `0902`** and parses the
17-character VIN out of the reassembled multi-frame ISO-TP reply (`parseVinReply` in `src/vin.cpp`). The first three characters — the WMI — are looked up in a fixed table
(`MAP` in `profileKeyFor`, `src/vin.cpp`): `1GT`/`3GT`/`1GC`/`3GC` → `gm_sierra_lz0`,
`WBA`/`WBS`/`5UX`/`4US` → `bmw_f10_535i`, `WAU`/`WA1`/`WUA`/`TRU` → `audi_q5`,
`1C4` → `jeep_ws`, `1FT`/`3FT` → `ford_sd_67`. A WMI match is necessary but not
sufficient — each row also carries a predicate over the rest of the VIN (engine, platform,
model year), so a Ford that is not a 2020–26 6.7L Power Stroke fails the row it matched.

**Falling through does not mean Generic.** There are two tables, and they fail differently:

- A vehicle in the **identification** table but with no profile match gets **Standard+** —
  `std_diesel` or `std_gas` depending on the engine the VIN named (`src/vin.cpp`, the
  `profileKeyFor(vin)` fallback). Same legislated PIDs as Generic, laid out for the engine.
- Only a vehicle in **neither** table falls all the way back to **Generic**.

An engine the table could not name falls back to the *gas* layout, because diesel-only tiles
on a gas truck look broken while the reverse merely omits data.

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
the first without being the second — the dash names the truck on its boot splash while still
running Generic or Standard+ gauges, instead of showing nothing merely because no profile
exists. The detected name is persisted so it survives a power cycle — the splash is drawn
before the OBD link is up. The **strings** are stored, not the VIN.

The Ford Super Duty is now an example of the two coming apart in the other direction: the
**2020–26 6.7L Power Stroke** was scanned on 2026-08-09 and has its own profile, while the gas
Super Dutys and the **pre-2020 diesels** remain identify-only on Standard+. The year split is
deliberate and load-bearing — the transmission generation break is 2019→2020 (6R140 → 10R140),
and the profile decodes gear as ten positions.

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

**One Ford has a profile; the rest are identified only.** Those are two different things —
see [Standard+](#standard) for why.

The **2020–26 Super Duty 6.7L Power Stroke** ships a real profile
(`src/vehicles/ford_sd_67.cpp`, registry key `ford_sd_67`), mapped from a **real scan on
2026-08-09** of a 2021 F-350: a census, a 187-PID sweep and a 29-minute cold-start drive.
Transmission fluid temperature (`221E1C`, signed int16 ÷ 16) and current gear (`221E60`) are
confirmed enhanced DIDs on the TCM at `7E1`; boost, rail pressure, fuel rate, DEF, DPF
differential pressure, pedal, charge-air temp and EGR turned out to be **standard SAE J1979
PIDs** the ECM's own supported-PID bitmap already advertised. Addressing is **11-bit only** —
every 29-bit header was silent, the exact inverse of the Sierra.

**Eight pages, 31 tiles:**

| Page | Tiles |
| :--- | :--- |
| ENGINE | RPM · SPEED · COOLANT · ENGINE LOAD |
| THERMAL | TRANSMISSION · OIL · EXHAUST GAS · CHARGE AIR |
| POWER | BOOST · RAIL PRESSURE · ACTUAL TORQUE · GEAR |
| TRIP | FUEL RATE · MPG · AVERAGE MPG · GAL/100 MI |
| AIR | AIR FLOW · INTAKE · ACCEL PEDAL · VOLTAGE |
| EMISSIONS | DPF PRESSURE · EGR VALVE · NOx · *(empty)* |
| AMBIENT | AMBIENT · BAROMETRIC · REF TORQUE · HORSEPOWER |
| RANGE | FUEL LEVEL · DIESEL FILL · DEF LEVEL · DEF FILL |

**DIESEL FILL reads `SET UP` until you set a tank size.** The factory tank is 29 / 34 / 48 US
gal depending on **wheelbase**, and the VIN encodes neither wheelbase nor bed length, so the
dash cannot derive it — see [step 7 of the install guide](INSTALL.md). DEF needs no setup: that
tank does not vary by cab or bed, so the profile carries the constant (7.4 gal).

⚠️ **Engine oil pressure is still absent** — no standard PID and no enhanced DID for it was
found, and **no dash has yet been plugged into a moving Super Duty**: the profile is validated
against a replay of the capture and the HIL rig, not a drive. See
[`FORD-STATUS.md`](FORD-STATUS.md).

**Everything else Ford is identify-only** and runs Standard+ or Generic: the F-150, the gas
Super Dutys, and the **pre-2020 diesels** (those are the 6R140, a different transmission, and
this profile decodes gear as ten positions).

**The profile gate reads series and engine only** — `vin[5]` ∈ `2345` and `vin[7]='T'` — plus
the model-year rule below. Cab style and axle deliberately do **not** gate it: neither changes
which PIDs a truck answers.

> **This was wrong until 2026-08-13.** The gate also required `vin[4]='W'` and `vin[6]='B'`. A
> vPIC 2-D sweep of both positions across the full 33-character alphabet — 1089 combinations —
> found **24** valid pairs for a 6.7L Super Duty, and that gate accepted **one** of them:
>
> | Position | Encodes | Values |
> | :--- | :--- | :--- |
> | `vin[4]` | cab style | `F`=Regular · `W`=Crew · `X`=SuperCab |
> | `vin[6]` | rear wheels + drive | `A`/`E`=SRW 4x2 · `B`/`F`=SRW 4WD · `C`/`G`=DRW 4x2 · `D`/`H`=DRW 4WD |
>
> So it admitted Crew Cab / single-rear-wheel / 4WD and silently dropped every Regular Cab,
> every SuperCab, every dually and every 4x2 to Standard+ — the same shape as the
> identification bug fixed earlier, which "rejected every 4x2 Super Duty and every cab style
> but one". Checking no series digit, it also wrongly *accepted* an F-600. `test_vin.cpp` now
> asserts all 24 combinations select the profile.

The **F-600** (`vin[5]='6'`, a Class-6 chassis cab that also takes the 6.7L) is excluded on
purpose: nothing of that class has been scanned, so it fails closed like any unverified
vehicle. The sweep also showed the series set is cab-dependent — an F-250 has no dually
configuration, and F-600 only appears on one — which is why a **one-position** sweep is not
enough to derive a rule here.

| Positions | Meaning |
| :--- | :--- |
| `1FT` / `3FT` | Ford truck, US- or Mexico-built |
| `1FD` | Ford truck — the **F-450/F-550** (and F-350 chassis-cab) WMI. Decodes identically to `1FT`; there is no `3FD` |
| `vin[5]` | **series** — `1`=F-150, `2`=F-250, `3`=F-350, `4`=F-450, `5`=F-550 |
| `vin[7]` | engine, **2011+** (Super Duty only) — `T`=6.7L Power Stroke, `6`=6.2L V8, `N`=7.3L V8 |
| `vin[7]` | engine, **2003–2009** — `P`=6.0L Power Stroke, `R`=6.4L Power Stroke, `5`=5.4L V8, `Y`=6.8L V10 |

**Two Super Duty eras, one series position.** `vin[5]` means the same thing in both, but the
engine alphabet is completely different, so each era is its own table row. The model year tells
them apart with no ambiguity: **2001–2009 are digits, 2010 onward are letters.**

The pre-2010 trucks are **identify-only** — they get Standard+, never `ford_sd_67`, which
decodes 10R140 gear positions and reads DIDs confirmed on a 6.7L. Verified per model year
rather than generalised across the span: `P` on 2003/04/05/06/07 and `R` on 2008/09. **2010
(year code `A`) returned no result** for either diesel code, so the row stops at 2009 — the
6.4L was built into 2010, so that is a gap in vPIC's data, not evidence the truck does not
exist.

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
| Sierra / Silverado **1500**, 2022+ | `vin[3]∈NPRUV`, `vin[4]∈HU` | `8`=3.0L Duramax (LZ0) · `D`=5.3L V8 · `K`=2.7L I4 Turbo · `L`=6.2L V8 | 2022–2026 |
| Sierra / Silverado **1500**, T1XX | `vin[4]∈{8,9}`, `vin[5]∈ABCDEFG` | `T`=3.0L Duramax (LM2) · `D`/`F`=5.3L V8 · `H`=4.3L V6 · `K`=2.7L I4 Turbo · `L`=6.2L V8 | 2019–2021 |
| Sierra / Silverado **HD** | `vin[3]∈0–5`, `vin[4]∈{8,9}`, `vin[5]∈LMNPRSTUVW` | `Y`=6.6L Duramax · `7`=6.6L V8 | 2020–2024 |

**Tonnage is `vin[5]`** — `ABCDEFG`=1500, `LMNPR`=2500, `STUVW`=3500 — and this is load-bearing,
not trivia. Swept identically on MY2020, 2021, 2022, 2023 and 2024.

> ⚠️ **`vin[3]`+`vin[4]` do not separate a 1500 from an HD.** In the T1XX era the 1500 sits in
> the *same* `vin[3]`/`vin[4]` space as the HD, so the HD gate — which checked neither tonnage
> nor anything else unique — matched light-duty trucks and **named a 2020 Sierra 1500 "GMC
> Sierra HD"**, with a blank engine because its `vin[7]` was not in the HD engine table. Fixed
> 2026-08-13 by adding the `vin[5]` check. This page previously stated that "`vin[4]` is what
> separates 1500 from HD"; that was the wrong rule, and it shipped.

The 2019–2021 trucks are **identify-only**. Their 3.0L Duramax is the **LM2**, a different engine
generation from the **LZ0** the `gm_sierra_lz0` profile was scanned on, so they get Standard+ —
the profile's own model-year rule (2023–2026) already excludes them.

**HD is still not split into 2500 and 3500 on the splash**, though the sweep above shows `vin[5]`
resolves it cleanly (`LMNPR` vs `STUVW`) — contradicting an earlier conclusion that the split was
a joint `vin[4]`+`vin[5]` function vPIC could resolve for only 16 of 1,089 combinations. Naming
the tonnage is now *possible*; whether the splash should say it is a separate call.

MY2024 additionally showed `X`/`Z` as 2500 and `Y` as 3500. Seen on one model year only, so they
are **not** admitted — a 2024 truck with those codes fails closed, as everywhere else here.

## Jeep

Ships as a skeleton profile (`src/vehicles/jeep_ws.cpp`), auto-detected by VIN (WMI `1C4` only). Mapped from a **real on-car scan** of a **2022 Jeep Wagoneer (WS platform,
5.7L Hemi eTorque, ZF 8HP75)** — the first port where the scan came before the profile rather
than after. Thirteen standard Mode-01 parameters are measured live and are exact.

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
