---
title: "BMW F10 535i (N55) — Support Status"
subtitle: "Where the port stands, and why the reachability question is largely already answered"
date: "2026-07-23"
---

# ⚠ 2026-08-23 — OIL TEMPERATURE FOUND, in a block the preset never swept

A cold-start drive plus a targeted probe on the same F10 535i. **The oil-temperature
question is answered, and the reason it stayed open for a month is structural.**

`BMW_F10.blocks` swept `22DAxx`, `2258xx`, `2242xx`, `2245xx`. The community table further
down this document names oil temp (after filter) as **`4402`** — block **`0x2244`**, which
nothing swept. The candidate this project was hunting was never in a candidate list.

| DID | Reads | Cross-check at the same moment |
| :--- | ---: | :--- |
| **`224402` oil temp** | **91.5 °C** | coolant `0105` = 99 °C — oil 7.5 °C below, correct sign at warm idle |
| `224300` "coolant" | 94.5 °C | legislated `0105` = 99 °C — **anchors the ×0.75−48 scale** |
| `224AB0` boost setpoint | 1004 hPa | idling, no boost — it *must* be atmospheric. Validates ×0.0390625 hPa |
| `224421` oil-press regulator P | 93 | — |
| `224307` electric water pump | 4 | state enum |
| `224418` oil condition | 0 | unpopulated, or another scale |

**The `×0.75 − 48` scale is no longer assumed.** Two DIDs from the same blocks land on
physically forced values.

**CONFIRMED ACROSS THE FULL RANGE (cold start, 2026-08-25).** A genuine cold start — 24 °C
coolant against 17 °C ambient — gives `224402` a **49.5 °C ramp, 24.8 → 74.2 °C** over 1.6
miles, 147 of 147 rows answered. Combined with the warm drive's 93.0–115.5 °C, the DID is now
observed from cold soak to full operating temperature.

**Two DIDs on unrelated scale families agree to half a degree across that ramp.** `224408`
decodes as **×0.1** (raw 936–1155 warm; on ×0.75−48 the same raw would read 654–818 °C), and
against `224402` over the 50 °C cold swing: **r = 0.99978, mean offset 0.44 °C, sd 0.33**.
Two different formulas cannot track each other that closely across 50 °C unless both the
identity and the scale are right.

**The thermal lag is textbook, in both directions:**

| | oil − coolant |
| :--- | ---: |
| cold start, minute 0 | −4.9 °C |
| cold start, minute 5 (maximum lag) | **−17.4 °C** |
| cold start, minute 7 (gap closing) | −16.0 °C |
| warm drive, under sustained load | **+11 °C** |

Coolant is thermostatically held, so it reaches target first; oil is heated by load and
carries the larger thermal mass, so it lags on warm-up and overtakes under sustained load.
Nothing but an engine-oil temperature behaves that way against a thermostatted coolant.

⚠️ Alarms stay OFF regardless: a threshold needs a **sourced N55 limit**, which no amount of
our own data supplies. What is no longer missing is the evidence of identity.

### `010F` is the charge-air (post-intercooler) sensor

The N55 has one intake-air sensor and on an F10 it sits after the intercooler, so there is no
separate `Cac` DID to find. The cold drive demonstrates it rather than assuming it: `010F`
held **20–21 °C for the whole drive** against 17 °C ambient — but read **59 °C at warm idle**
in the 2026-08-23 probe. Same sensor: heat-soaked when stationary, near-ambient once airflow
starts. The existing INTAKE tile is already the reading people ask for.

### `22582F` / `225896` are modelled CATALYST temperature, not gas temperature

From cold, `22582F` rose 23 → 125 raw over seven minutes, tracking coolant. Real exhaust gas
heats in seconds; a catalyst has large thermal mass and warms over minutes. The community name
— "exhaust gas temperature according to KAT **from model**" — matches that behaviour. At ×2
the range is ~46 → 250 °C, consistent with the 235 °C read warm. Worth knowing before anyone
wires it to an `Egt` tile expecting live exhaust temperature.

**The drive's own candidates are closed out.** `225817` and `2258EB` are byte-identical on
99.51 % of 1427 rows, span only 9 distinct values while coolant moved 30 → 107 °C, and
`correlate` ranks both against **ambient**. Air-side, not oil. `22587E` ramps genuinely but
scores r = 0.972 against coolant — by this project's own rule, evidence of *another
coolant-circuit sensor*.

## Addressing, settled by the ECU's own bitmaps

| Header | Result |
| :--- | :--- |
| **`7DF`** | **42 PIDs supported** — matching the "42 confirmed live" figure below |
| **`7E0`** | **silent.** No bitmap; every probe NO DATA, including DIDs known-good on `7DF` |
| **`7E1`** | **13 PIDs** — a second reachable module, not previously recorded |
| `7E2` | nothing |

**Do not enable the DME's physical address on an F10 without probing it.** OBDb's crowd map
showing ~44 PIDs at `7E0` and 3 at `7DF` is inverted for this car.

## Fuel: no fuel-rate PID, but MPG works anyway

`015E` and `019D` both return NO DATA, and **`015E` is absent from the DME's own supported
bitmap** — the ECU stating it, not a probe failure. MAF (`0110`) is live (1426/1427 rows on
the drive, r = +0.873 vs load), so fuel rate is derived from it at stoichiometry: **24.0 mpg
over a 13.6-mile cold mixed drive**, against an EPA 19 city / 29 hwy car, idle 1.57 L/h.
⚠️ λ=1 breaks under boost, so it over-reads mpg at high load. `0144` EQ_RAT answers on this
car (λ 0.9978 at idle) and is the correction, not yet applied.

## Oil pressure: ABSOLUTE, confirmed

Engine off it reads **1058 mbar** against baro 1000; engine-off rows inside the drive read
**0.3 psi** once corrected; the final row as the engine died reads `03D6` = 982 mbar.
`decBmwOilPress` must subtract ambient baro — the overstatement is **14.5 psi**.

---

# ⚠ SCAN RESULTS — supersedes the pre-scan research below

An F10 535i was scanned on-car (census + sweep + a ~15 min warm drive,
`census.json` / `sweep.json` / `bmw_drive.csv` / `report.md`). **The car
overturned the central premise of the research below: the BMW `6F1` addressing
is _not_ reachable through a plain ELM327 on this vehicle.**

| Pre-scan expectation | What the car actually did |
| :--- | :--- |
| DME reachable at `6F1`→`612`, EGS at `6F1`→`618`; ZGW routes UDS transparently | **Both `612` and `618` are SILENT** (`NO DATA`). The gateway does not pass `6F1` physical addressing to a cheap adapter here. |
| Enhanced DIDs `DA25` (oil temp), `DA12` (ATF), EOP `586F` live behind `6F1` | `22DA25` on the functional broadcast returns `7F 22 22` (NRC 0x22, conditions-not-correct). The `DAxx` block does **not** answer. |
| Enhanced data needs BMW-specific headers or it returns nothing | **The DME answers both Mode-01 AND enhanced Mode-22 on the standard `7DF` functional broadcast** — in the `58xx`/`42xx`/`45xx` block, a _different_ namespace than the community `DAxx` map. |

## What shipped in the profile (`src/vehicles/bmw_f10_535i.cpp`)

- **Addressing collapsed to a single header — `7DF` functional.** The `6F1`
  DME/EGS headers were removed; every active row uses header 0.
- **Guaranteed tier — standard Mode-01 (42 PIDs confirmed live):** coolant
  `0105`, RPM `010C`, speed `010D`, IAT `010F`, load `0104`, MAP `010B`,
  baro `0133`, ambient `0146`, fuel level `012F`, volts `0142`. This is the
  working core of the dash and needs no enhanced data.
- **BOOST** is derived from standard **MAP `010B`** (gauge vs a fixed
  101.325 kPa baseline), not an enhanced DID.
- **OIL PRESSURE = `22586F` byte 0 — ACTIVE but UNVERIFIED scale.** Over the
  drive byte 0 rose monotonically with RPM (9.2 @ idle → ~12 @ 2000 rpm): the
  oil-pressure signature, and `586F` is the community-named EOP DID. Shown as
  raw byte == psi (identity), alarms OFF — the magnitude is a hypothesis (no
  cluster oil-pressure readout exists on the F10 to calibrate against, same as
  the GM `decOilPsi`). **Decoded byte-0-only on purpose:** on `7DF` a second
  module appends a `7F2222` NAK after the positive frame, which
  `obd_parse.cpp` concatenates — a `u16` read would ingest the NAK.
- **OIL TEMP and ATF/gearbox TEMP are STUBBED.** ATF needs the EGS, which is
  gateway-blocked (likely never reachable via a plain ELM327). Oil-temp
  candidates exist on `7DF` (`225817` / `2258EB` both sat 91–98 °C, byte 0,
  tracking coolant) but the drive was **warm-started** — no
  cold→hot ramp to pin them. They stay off until a cold-start focused drive.

## Alarm thresholds — researched 2026-07-26

The N55 runs deliberately hot, which is why the profile shipped with alarms off rather than
inheriting a generic limit. BMW's cooling documentation gives the DME's own targets:

| DME mode | Target |
| :--- | :--- |
| Economy | **108 °C / 226 °F** |
| Normal | 104 °C / 219 °F |
| High | 95 °C / 203 °F |
| High, characteristic-map thermostat | 90 °C / 194 °F |

**226 °F is normal operation on this engine.** A generic 235 °F warn would leave nine degrees
of headroom above the DME's own Economy target and nuisance-fire on a hot day.

**COOLANT is now the one alarmed row:** warn **239 °F (115 °C)**, critical **248 °F (120 °C)**.
The critical value sits deliberately *below* the ~125 °C at which the car raises its own
warning and drops into limp — this display exists to show what the cluster hides, so it should
lead the OEM warning rather than echo it.

**Confidence: HIGH** on the DME target range (BMW service documentation). **MEDIUM** on the
warn/critical values themselves — they are derived from that range plus owner reports of the
band outside normal operation, *not* from a published OEM fault threshold. Revisit if an
ISTA/TIS threshold table becomes available.

### Rows deliberately left with alarms off

| Row | Why |
| :--- | :--- |
| **OIL PRESSURE** | ⚠️ **Scale RESOLVED 2026-08-15 — see "Oil pressure is 16-bit" below.** It is a u16 in millibar, not byte 0. Alarms still off: a low-pressure limit needs a sourced N55 minimum **and** a hot-idle sample (the lowest-pressure state), neither of which the warm drive provides |
| TRANSMISSION (ATF) | Lives on the EGS module, gateway-blocked on this car |
| OIL (temp) | Candidate DIDs never pinned — the only drive was warm-started |
| BOOST / LOAD / INTAKE / AMBIENT | Decode is sound, but no N55-specific limits are established. Inventing a number buys nothing |

## Oil pressure is 16-bit millibar, not byte 0 — corrected 2026-08-15

`22586F` was decoded byte-0-only, on the theory that a second module's `7F2222` NAK was
appended to the value byte. **That was wrong**, and it made a running engine read ~10 psi.

**Evidence, from captures already on disk — no car needed:**

| Source | What it shows |
| :--- | :--- |
| `obd-display/bmw_drive.csv` | 159 samples, **all exactly 2 bytes**, none containing `7F2222`, low byte spread across `0x87`–`0xF9` (a NAK is a fixed constant; a byte taking 100+ values is data) |
| `sweep.json` | `"raw": "62586F03FF\r7F2222\r\r"` — the NAK is **its own line**, after a **two-byte** value |
| Distinctness | 144 distinct values in 159 samples — the resolution of a 16-bit sensor, not of a byte |

Read as **u16 millibar** the same drive gives **2324–4776 mbar (33.7–69.3 psi), median 40.4**,
rising monotonically by RPM band — **36.7 / 40.4 / 45.3 psi** across idle / 900–1500 / 1500–2500
— with Pearson **r = 0.653**. Non-linearity against RPM is expected: the N55's map-controlled
variable-displacement pump regulates to a demand target rather than tracking engine speed.

**Why the original reasoning passed its own check.** It justified byte-0 with *"byte0 rose
monotonically with RPM — the oil-pressure signature."* True, and useless: **the high byte of a
rising 16-bit value also rises.** The shape test passed while the magnitude was wrong ~4×. A
shape check cannot validate a scale.

### ⚠️ Still open: absolute or gauge?

The sweep sample is `03FF` = **1023 mbar ≈ 1 atmosphere**. If that reading was taken key-on /
engine-off, this sensor reports **absolute** pressure — an engine-off gauge sensor would read
~0. Absolute would also put warm idle at ~1.5 bar gauge, which matches BMW's documented
low-demand pump target better than 2.5 bar gauge does. But the sweep captured no RPM anchor and
the drive log contains no engine-off sample (602–2146 rpm throughout), so **this is unresolved
and the displayed value may be ~15 psi high.**

**It is settled by ONE sample, and needs no drive:** with the key on and the engine OFF, read
`22586F`.

| Reading | Meaning | Fix |
| :--- | :--- | :--- |
| ~`03E8`–`0410` (≈1000 mbar) | **Absolute** | Subtract ambient baro in `decBmwOilPress` |
| ~`0000`–`0100` (≈0) | **Gauge** | Current decode is already correct |

Do that before spending a drive on it.

## Next step to finish the profile

A **cold-start focused drive** (`log --pids 22586F,225817,2258EB,22587E,…`,
cold → warm, with a couple of 3–4k-rpm pulls) is the one thing that converts the
oil-temp candidates into a formula and confirms the oil-pressure magnitude and
its high-RPM curve. Everything else the profile can carry today.

---

# Short version

**BMW is not supported yet. No F10 has ever run this firmware.**

Everything in the screenshots is a 2025 GMC Sierra 1500 3.0L Duramax (LZ0, Global B).
The architecture is ready for a second vehicle — the PID table, decoders, thresholds,
layout and tank constants all live behind a `VehicleProfile` struct, and adding a vehicle
means adding one file under `src/vehicles/`. What does not exist is the **content** of
that file for a BMW, because the combustion PIDs have not been measured on the car.

But the BMW picture is **materially better than the Ford one** in the place that matters
most: the *reachability and addressing* question — the one that could have made this
project impossible with a cheap adapter — is largely answered on paper, and the answer is
favorable. This document records what a July 2026 research pass established (19 sources, 65
candidate claims, 25 put through three-vote adversarial verification, 19 confirmed) and
what still has to be settled on the actual car.

# The good news, up front

Three questions that were wide-open for Ford are effectively closed for BMW:

| Question | Answer | Confidence |
| :--- | :--- | :--- |
| **Does it need the ENET/ISTA Ethernet path?** | **No.** Enhanced data is reachable over classic D-CAN from the OBD-II port. The "8HP needs E-Sys/ENET" claim was actively **refuted (0–3)**. | High |
| **What protocol?** | Standard **UDS Service `0x22`** (ReadDataByIdentifier) with 2-byte DIDs — the *same service the GM firmware already speaks and reassembles*. BMW also mixes in some legacy Service `0x21` reads. | High |
| **Can a plain ELM327 reach the modules?** | **Yes**, with one catch (addressing, below). The ELM327 already provides what's needed: arbitrary request headers via `AT SH`, and automatic ISO-TP flow control so multi-frame replies reassemble on their own. | High |

**Sourcing honesty:** these three answers are corroborated by primary standards (the
ELM327 datasheet, ISO 14229/15765), the authoritative `ediabaslib` BMW adapter project,
*and* multiple independent community signalsets (OBDb) that agree on the addressing. This
is genuinely better-sourced than the Ford effort. The weak layer is the specific N55
*combustion* PID formulas — see "Where BMW is no better than Ford" below.

# The one catch: BMW addressing is not SAE addressing

This is the single thing that will make a naive scan return **nothing**.

Standard OBD-II uses functional broadcast `7DF`, physical requests `7E0–7E7`, responses
`request+8`. **BMW does not use this for enhanced diagnostics.** It uses:

- **Tester (request) ID `0x6F1`**
- **Response base `0x600` + the module address**

| Module | Address | Response ID | ELM327 header |
| :--- | :--- | :--- | :--- |
| **DME** (engine) | `0x12` | `0x612` | `AT SH 6F1`, target the DME |
| **EGS** (ZF 8HP transmission) | `0x18` | `0x618` | reached through the ZGW gateway |
| **KOMBI** (cluster) | `0x0D` | `0x60D` | — |
| Battery management | — | `0x607` | — |

So the scanner must be **told the BMW headers** (`0x6F1` tester, `0x600+addr` responses) or
the DME will never answer on the default OBD functional query. This also **confirms the DME
address of `0x12`** that we'd seen referenced.

The EGS/transmission lives on PT-CAN, not the OBD D-CAN — but BMW's **ZGW central gateway
routes UDS/Mode-22 to it transparently**, so a plain adapter reaches it via `0x18 → 0x618`.
It is **not** a reachability blocker. (Source: `ediabaslib`; spoolas.eu F10 8HP70
reverse-engineering write-up.)

# What survived verification

| Finding | Confidence | Source family |
| :--- | :--- | :--- |
| BMW enhanced addressing = tester `0x6F1` / response `0x600+addr` (DME `0x612`, EGS `0x618`, cluster `0x60D`); **not** `7E0/7E8` | High | OBDb signalsets + spoolas + ediabaslib, independent agreement |
| Enhanced reads are **UDS `0x22`, 2-byte DID, over classic 11-bit CAN** — no ENET needed. Some signals use legacy `0x21` | High | OBDb signalsets + UDS/ISO 14229 |
| **Confirmed OBDb DIDs**: engine oil temp `22 DA25` (16-bit signed, `°C = raw − 48`); ATF/transmission fluid temp `22 DA12` at `rax 618`; engine oil pressure (EOP) at `rax 612` | High | OBDb BMW-5-Series `default.json` |
| A plain ELM327 is **D-CAN only** — which is exactly what the F10 OBD port exposes. It cannot reach BMW's other proprietary protocols (DS1/DS2/BMW-FAST) | High | ediabaslib |
| Some DIDs are **session-gated**: a read-only tool that never sends `0x10` will get a negative response (`NRC serviceNotSupportedInActiveSession`) for those. Default-session DIDs still read | High | ISO 14229-1 |

Note what the confirmed OBDb set gives you cleanly: **oil temp, oil pressure, coolant, and
transmission fluid temp** — with real, verified 2-byte BMW DIDs. That already covers the
"TOWING page" core of the GM layout. What OBDb **lacks** is every high-value N55 *combustion*
signal (boost/MAP, charge-air temp, fuel rail pressure, ignition timing/knock, lambda/AFR,
VANOS cam position, Valvetronic lift, 12 V IBS state-of-charge). Those must come from the
community N55 tables and be verified on the car.

# The crux — the one thing only the car can answer

**How much of the enhanced PID set answers in the *default* diagnostic session?**

Our scanner is read-only by construction (services 01/03/09/22 only) and **will not send
`0x10`** (session control) or security access — by design, because it runs on other
people's vehicles. Per ISO 14229 a UDS server boots into the default session, and BMW may
gate some DIDs behind the *extended* session. Any gated DID returns a negative response to
our tool.

**This is settled empirically, cheaply, in the first minute of a scan:** set the BMW DME
header (`AT SH 6F1`, listen on `0x612`), send **one `22 DA25`** (oil temp — a confirmed
default-session DID). If the DME answers, the default-session read path is proven and the
normal census → sweep → log → correlate flow proceeds. Then the open question becomes
*which* of the combustion DIDs also answer in default session vs. which are gated.

If a wanted parameter turns out to be gated behind `0x10`, that is a **design decision for
Alan** — it turns the read-only tool into something that changes ECU session state, and it
does **not** get added quietly.

# Where BMW is no better than Ford

The community N55 combustion PID formulas come from a neocities blog
(`thesecretingredient`) and a single GitHub CSV (`Shooooooooo/bmw_pid_data`). They are
reverse-engineered, **generic-N55 (not F10-confirmed)**, carry **no module addresses**, and
are unverified on a real car. Treat every one as a *candidate to confirm*, not fact:

| Parameter | Candidate DID | Community formula | Note |
| :--- | :--- | :--- | :--- |
| Coolant temp | `4300` | `raw × 0.75 − 48` °C | apply DME header `0x612` |
| Oil temp (after filter) | `4402` | `raw × 0.75 − 48` °C | |
| Boost pressure (actual) | `4205` | `raw × 0.078125` hPa | |
| Boost **setpoint** | `4AB0` | `raw × 0.0390625` hPa | **known transcription trap** — some lists mislabel this as boost actual |
| Ambient pressure | `4201` | `raw × 0.0390625` hPa | |
| VANOS inlet / exhaust | `4506` / `4507` | `raw × 0.1` °KW | |
| Rail pressure (HPFP) | `58EF` / `58F0` | `raw × 0.0005` MPa | |
| Lambda | `582C` | `raw × 0.000244140625` | |
| Oil-pressure regulator P/I/D | `4421`–`4423` | — | |
| Oil condition sensor | `4418` | — | |
| Electric water pump status | `4307` | — | |

150+ such parameters exist in these tables with linear scaling, but at this layer BMW data
is about as reliable as Ford's — i.e. verify everything with the range-scanner and
anchor-correlation before trusting it.

# What remains unknown

Every one of these is a scan-session (or a separate-research) question.

1. **Which combustion DIDs answer in the default session** — the crux above. Only the car
   settles it.
2. **Whether the generic-N55 community PIDs match the F10 535i DME software variant**, and
   the correct module header for each (all under DME `0x612`, or some on EGS `0x618`?). The
   CSVs carry no addresses.
3. **Alarm thresholds — not covered by this research pass.** No verified N55 numbers
   surfaced for oil temp, coolant, ZF 8HP fluid temp (including what temperature is actually
   harmful vs. the "lifetime fluid" controversy), oil pressure, stock boost, or charge-air
   temp. This needs its own sourcing pass against BMW TIS/ISTA and ZF docs vs. community
   consensus — modelled on the GM `alarm-threshold-review.md`.
4. **Model-year (2011–2016) and engine-variant (N55 vs N20/N47/N57/S63) PID deltas** — only
   weakly addressed. The OBDb schema supports year overrides, but no specific F10 deltas
   were established.
5. **Whether Valvetronic lift and per-cylinder knock retard are exposed via `0x22` at all**,
   or only through BMW-proprietary (non-`0x22`) services our tool won't reach.

# The path forward

Same method that produced the working GM table, with the BMW addressing baked in from the
start:

1. **Census — with BMW headers.** Set `AT SH 6F1` and probe the DME (`0x612`), EGS
   (`0x618`), and cluster (`0x60D`). A negative response is as informative as a positive
   one: `7F 22 xx` proves a real module received the request, confirming the addressing.
   **First probe is the default-session `22 DA25` test.**
2. **Sweep.** Brute-force the candidate DID blocks (`0x42xx` / `0x45xx` / `0x58xx` for the
   DME; the OBDb `DAxx` set) at each live module and log every address that answers, with
   raw bytes.
3. **Log a drive.** Poll every hit at ~1 Hz alongside known anchors (RPM, speed, load,
   coolant) and write raw hex to CSV.
4. **Correlate.** For each unknown DID, enumerate every plausible byte interpretation and
   score it against every anchor — that's how an unlabelled address becomes "that's the
   VANOS position sensor."
5. **Confirm.** Cold-vs-hot at the same RPM confirms oil pressure/temp; boost separates from
   MAP under load.

The tool that does steps 1–4 (`tools/obd_scan/`) is **read-only by construction**: only OBD
read services can be transmitted, enforced in code. The one BMW-specific addition it needs
is the ability to set the `0x6F1`/`0x600+addr` headers — see `docs/PORTING-LESSONS.md`.

## What is needed from a car

- **About an hour parked**, engine idling, laptop on the OBD port.
- **A normal drive** afterwards — ideally a cold start and some load (hills). Load is what
  makes correlated parameters separate from each other.
- Nothing is installed and nothing is modified.

# Honest expectations

- Because oil temp, oil pressure, coolant and transmission-temp are **already confirmed as
  default-session BMW DIDs**, a *basic* F10 profile could come together fast — potentially
  before a full drive-correlation session.
- The turbo combustion signals (boost, rail pressure, VANOS, lambda, knock) are the real
  work: community candidates exist but are unverified and session-gating may block some.
- Some parameters may simply not be exposed via `0x22`. On the GM truck, several swept-for
  PIDs were **never found**. The same may be true for Valvetronic lift and per-cylinder
  knock on the N55.
- Alarm thresholds need their own research pass before the profile is trustworthy.

**If you have an F10 535i and are curious:** the useful things to know first are the exact
build year and whether it's the N55. The reachability is no longer the question — the
question is which enhanced DIDs your specific DME answers in the default session.
