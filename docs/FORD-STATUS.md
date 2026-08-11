---
title: "Ford 6.7L Power Stroke — Support Status"
subtitle: "Where the port actually stands, and what would move it forward"
date: "2026-07-21"
---

> ## ⚠️ This document predates the scan — read this box first
>
> **Everything below was written on 2026-07-21, BEFORE a truck was available.** It is kept
> because its reasoning about method and pitfalls still holds, but its status claims are
> superseded:
>
> - A **2021 F-350 was scanned on 2026-08-09** (census + 187-PID sweep + a 29-minute
>   cold-start drive). The data IS on HS-CAN — no MS-CAN adapter needed.
> - `src/vehicles/ford_sd_67.cpp` now ships **seven pages**. Transmission fluid temperature
>   (`221E1C`, signed int16 ÷ 16) and current gear (`221E60`) are confirmed enhanced DIDs on
>   the TCM at `7E1`.
> - Addressing is **11-bit only** — all fifteen 29-bit headers were silent, the exact inverse
>   of the Sierra.
> - Most of the remaining parameters turned out to be **standard SAE J1979 PIDs the ECM's own
>   supported-PID bitmap already advertised** — boost `0187`, rail `016D`, fuel rate `019D`,
>   DEF `019B`, DPF `017A`, pedal `0149`, CAC `0177`, EGR `0169`. An earlier draft looked for
>   `0111`/`0123`/`012C`/`015E`, found them absent, and wrongly concluded the parameters were
>   unavailable. **"PID X is not in the bitmap" does not mean "the parameter is unavailable".**
>
> **Still open:** engine oil pressure is the one parameter with no standard PID and no
> enhanced DID found, and **no dash has yet been plugged into a Super Duty** — the profile is
> validated against a replay of the capture, not against a moving truck.

# Short version

**Ford is not supported yet. No Power Stroke has ever run this firmware.**

The firmware *recognizes* F-150 / F-250 / F-350 / F-450 / F-550 by VIN and names them on the
boot splash — but they all still run the Generic profile. Recognition is not support: no
enhanced PID in this document has been confirmed against a real truck. See
[`VEHICLES.md`](VEHICLES.md) for the VIN pattern and the years it was verified over.

Everything in the screenshots is a 2025 GMC Sierra 1500 3.0L Duramax (LZ0, Global B).
The architecture is ready for a second vehicle — the PID table, decoders, thresholds,
layout and tank constants all live behind a `VehicleProfile` struct, and adding a vehicle
means adding one file under `src/vehicles/`. What does not exist is the **content** of
that file for a Ford, because nobody has measured it.

This document exists so that nobody — including future me — mistakes "architecturally
ready" for "works."

# What would actually work today

If you plugged this into a 6.7L Power Stroke right now, with the GM profile loaded, you
would get:

| Would work | Would not |
| :--- | :--- |
| **Generic OBD-II Mode-01 PIDs**: RPM, vehicle speed, coolant temperature, engine load, MAF, intake air temp, barometric pressure, battery voltage, ambient temp. These are legislated and identical across manufacturers. | **Everything enhanced.** Transmission temperature, EGT, DPF differential pressure, DPF soot, regen status, fuel rail pressure, oil pressure, DEF level, NOx, turbo vane position — all of it. These live at manufacturer-specific addresses. |
| The UI, alarms, logging, day/night, OTA — all vehicle-agnostic. | The alarm thresholds, which are Duramax numbers. |

So roughly a third of the tiles would populate and the rest would read `--`. That is not a
useful product; it is a starting point for discovery.

# Why it isn't just a table lookup

Enhanced (Mode 22 / UDS) PIDs are not standardized. Each manufacturer chooses its own
identifiers, byte layouts, scaling and module addresses, and does not publish them.

A research pass in July 2026 (19 sources, 79 candidate claims, 25 put through three-vote
adversarial verification) tried to assemble a Ford map from public sources. **19 of the 25
claims were refuted.**

The single most useful finding was a negative one. The most-cited community Ford 6.7L PID
list — a 2014 forum post about a **2013** truck — contains **no** fuel rail pressure, DEF
level, DEF quality, DEF pressure, NOx, turbo vane position, EGR position, engine oil
pressure, MAF, or torque PIDs at all. Its DPF differential-pressure entry has a **blank**
equation. Someone asked for rail pressure in that thread in 2014 and was never answered.

Those parameters are not hard to find in the community data. **They were never publicly
mapped.**

## What did survive verification

| Finding | Confidence | Source |
| :--- | :--- | :--- |
| Transmission fluid temp = Mode 22 PID `0x1E1C`, 16-bit, `°C = raw / 16`. **Decode as `int16`** — the common unsigned form reads ~4096 °C on a sub-zero cold start | Medium | ScanGauge + OBDLink + community formulas, three independent families agreeing |
| Transmission generation break is **MY2019 → MY2020** (6R140 → 10R140). The 2023 facelift is *not* a transmission break | Medium | dieselhub + Wikipedia; Ford primary sources unreachable |
| Diesel tank capacity is a **six-way** split — 26.5 / 29 / 34 / 40 / 48 / 66.5 gal — not the commonly cited "34 or 48" | High | Ford 2025 Super Duty Owner's Manual, p.209 |
| DEF tank 7.5 gal (complete) / 7.2 gal (incomplete); DEF burn ≈ 2–6 % of fuel | High | Same manual, p.222 |
| **DEF warnings are distance-based, not level-based** — and Ford states they can appear with more than half a tank remaining, *"regardless of the level indicated on the diesel exhaust fluid gauge"* | High | Same manual, pp.217–223 |

Note the last one carefully: a DEF alarm driven off tank percentage would **disagree with
the truck's own dash**. That is not a threshold to tune, it is a different alarm policy,
and the vehicle profile will have to be able to express it.

# What remains unknown

Every one of these is a scan-session question. No credible source answered any of them.

1. **Addressing.** 11-bit (`7E0`/`7E1`) or 29-bit (`18DAxxF1`)? And does it differ *by
   adapter*? On the GM truck it surprisingly did — a vLinker MS reads it on 11-bit while a
   Vgate iCar Pro WiFi only answers on 29-bit, same vehicle, same PIDs.
2. **Bus topology — the highest-leverage unknown.** Do the aftertreatment/DEF/NOx
   parameters live on HS-CAN (pins 6/14), or on Ford's MS-CAN (pins 3/11)? A single-bus
   adapter physically cannot see MS-CAN. If that is where the data is, a multi-bus adapter
   becomes mandatory hardware for the Ford build.
3. **CAN-FD on 2023+.** Required to *read*, or only to *flash*? On GM Global B the
   "needs CAN-FD" claim turned out to be **false** for reading. Zero credible sources
   either way for Ford. Not blocking for pre-2023 trucks.
4. **Whether the 2013-era PIDs still answer** on a 2020+ truck, and whether byte layouts
   shifted across generations.
5. **Alarm thresholds.** Every threshold claim except the DEF staging was refuted. This
   needs its own sourcing pass, modelled on the GM alarm-threshold review.

# The path forward

The method is not guesswork — it is the same one that produced the working GM table, where
no public map existed either.

1. **Census.** Probe every candidate header in both addressing modes and record who
   answers. A negative response (`7F 22 31`, "request out of range") is as informative as a
   positive one: it proves a real module received the request, which confirms the
   addressing.
2. **Sweep.** Brute-force the known-populated PID blocks at each live module and log every
   address that answers, with its raw bytes.
3. **Log a drive.** Poll every hit at ~1 Hz alongside known anchors (RPM, speed, load,
   coolant) and write raw hex to CSV.
4. **Correlate.** For each unknown PID, enumerate every plausible byte interpretation and
   score each against every anchor. "Bytes 1–2 as a big-endian u16 track RPM at r = 0.94,
   spanning 300–1000" is how an unlabelled address becomes "that's an EGT sensor."
5. **Confirm.** The strongest single confirmation is a **cold-versus-hot comparison at the
   same RPM** — thick oil reads higher pressure. That is what finally identified oil
   pressure on the GM truck after correlation alone left it ambiguous.

The tool that does steps 1–4 is being built now (`tools/obd_scan/`). It is
read-only by construction: only OBD read services can be transmitted, enforced in code,
because it is intended to run on other people's vehicles.

## What is needed from a truck

- **About an hour parked**, engine idling, laptop on the OBD port.
- **A normal drive** afterwards — ideally including a cold start, and some load (hills or a
  trailer). Load is what makes correlated parameters separate from each other.
- Nothing is installed and nothing is modified.

Two trucks are lined up: a **2018** (6R140) and a **2021** (10R140). That pair straddles
the transmission generation break, which means the 2017–19 vs 2020–22 PID delta can be
established by *diffing two sweeps* rather than trusting forum consensus.

# Honest expectations

- A first Ford profile covering the same ground as the GM one is **a scan session plus a
  drive plus a few evenings of decode work** — assuming the data is on HS-CAN.
- If it turns out to be on MS-CAN, add an adapter purchase and a repeat session.
- Some parameters may simply not be exposed. On the GM truck, DPF soot load, regen status
  and turbo vane position were swept for and **never found** — they are not readable on
  that vehicle at any address tried. The same may be true here.
- A 2023+ truck is a separate question that nobody has answered, including this document.

**If you have a Power Stroke and are curious:** the useful thing to know first is the model
year and engine. A 2017–19 and a 2020+ are different enough inside that they are nearly two
projects, and both are worth doing.
