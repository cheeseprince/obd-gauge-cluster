---
title: "Audi Q5 (2018, 2.0T) — Support Status"
subtitle: "The best-positioned port yet — standard UDS addressing, a real captured DID census"
date: "2026-07-24"
---

# Short version

**A skeleton Audi profile now ships** (`src/vehicles/audi_q5.cpp`), auto-selected by VIN
(WMI `WAU`/`WA1`/`WUA`/`TRU` → `audi_q5`). It was mapped from a **real on-car drive**
of a **2018 Audi Q5 (typ FY, MLB Evo, 2.0T TFSI EA888 gen-3)** — the easiest port yet: plain
**standard UDS addressing** (engine `7E0`, trans `7E1`; no BMW-style extended addressing) that a
cheap ELM327 reaches directly. Standard Mode-01 stats are exact; three enhanced DIDs are wired
with **best-guess (unverified) scaling**, alarms off, pending a cold-start confirmation drive:
ATF/gearbox temp `222104`@7E1, oil temp `2220A1`@7E0 (16-bit ×0.1 = °C), charge-air `22202A`@7E0
(mbar). The rest of this document records the research and the empirical scan that got here.

Everything in the screenshots is still a 2025 GM Sierra Duramax. This document records what a
July 2026 research pass established (20 sources, 25 claims verified, 18 confirmed) and what a
scan session would need to fill in.

# What the research established (high confidence)

| Finding | Confidence | Source |
| :--- | :--- | :--- |
| Enhanced data is **UDS Service 0x22, 2-byte DIDs, over classic 11-bit CAN** — reachable directly from the OBD-II port. No ENET/DoIP, no VAG-specific tester addressing for powertrain | High | OBDb Audi-Q5 capture; Audi-A4/A6 signalsets |
| **Engine ECM answers on standard `7E0` / response `7E8`**; **transmission/TCM on `7E1` / `7E9`** — both reachable by a plain single-bus ELM327, no gateway trick | High | OBDb Audi-Q5 `2018/command_support.yaml` (primary capture) |
| The ECM serves **both** legislated Mode-01 PIDs *and* enhanced Mode-22 DIDs on the same `7E0` header | Medium | Same capture |
| Confirmed DIDs that answer (module → DID): coolant `7E0 221135`, calculated oil temp `7E0 2211BE` / `2220A1`, radiator-outlet coolant `7E0 221626`, actual charge-air pressure `7E0 22202A`, boost-regulator feedback `7E0 2211CC`, **ATF/gearbox temp `7E1 222104`** | Medium | Same capture (identifiers proven to answer; **scaling formulas not in the dataset**) |
| A few **non-powertrain** modules (battery mgmt `710`, others `713/714/715`) use VAG-specific extended headers — but everything a gauge display wants is on the standard `7E0`/`7E1` | High | Same capture |

# Two important corrections to the premise

1. **It is not a ZF 8HP.** The captured 2018 Q5 is a **DL382 dual-clutch (S tronic)**, not a ZF
   8-speed torque-converter automatic — the transmission signals are clutch temperatures and
   pressures, not a torque-converter box. So there is **no diagnostic reuse** from the BMW F10's
   ZF 8HP. (A different Q5 trim could have the torque-converter box; confirm per car.)
2. **OBDb has the capture but not the formulas.** The `OBDb/Audi-Q5` repo's `command_support.yaml`
   is a real passive scan that proves *which DIDs answer* — but its `signalsets/default.json` is
   **empty**, so it provides **no scaling formulas**. We know the addresses; the math must be
   established on the car by correlation (the exact job the range-scanner does).

# Sourcing verdict (Ford vs BMW vs Audi)

- **Addressing / reachability:** as good as BMW, arguably better — a *primary empirical capture*
  proves standard `7E0`/`7E1` UDS works, with no special tester addressing at all. Far better
  than Ford (which had no primary source).
- **PID formulas:** unproven, like the others. The confirmed DIDs are strong scan targets, but
  their byte layouts/scaling are **TBD on-car** (empirical), because the OBDb signalset is empty.
- **Thresholds:** not covered by this pass (the run hit a limit before finishing) — the EA888
  gen-3 oil/coolant/DSG-fluid/boost/IAT limits need their own sourcing pass.

# What remains unknown (scan-session questions)

1. **The scaling formulas** for the confirmed DIDs (coolant, oil temp, ATF, charge-air, boost) —
   correlate against RPM/speed/load/coolant over a drive, plus a cold-vs-hot pass for temps.
2. **The combustion PIDs not in the capture** — fuel rail pressure, ignition timing/knock,
   lambda/AFR, cam timing — sweep for them at `7E0`.
3. **CAN-FD on some modules.** The powertrain modules answered *classic* CAN in the capture (good),
   but a plain ELM327 physically cannot read any module that has moved to CAN-FD — verify the
   ones you want still answer classic-CAN.
4. **Trim/variant** — confirm the actual gearbox (DL382 DSG vs torque-converter) and engine code
   on the specific car, which changes the transmission DID set.

# The path forward — and why it's short

Because the addressing is **standard `7E0`/`7E1`** (no BMW-style `6F1`/extended-addressing dance),
adding Audi to the scanner is trivial: an `audi` preset with the standard headers and the
confirmed DIDs as probe targets. Then the usual flow:

1. **Census** — confirm `7E0`/`7E1` answer and the known DIDs return data.
2. **Sweep** the `22xxxx` blocks at `7E0` (and `7E1`) for the combustion/trans PIDs.
3. **Log a drive** and **correlate** to nail the formulas.
4. Author `src/vehicles/audi_q5.cpp` — seed it with the confirmed DIDs, formulas from the
   correlation, sedan/SUV layout (no DEF/EGT — gas), EA888 thresholds from a follow-up pass.

The scanner is read-only by construction; nothing is written to the car.

## What is needed from the car

- **About an hour parked**, engine idling, laptop on the OBD port with a WiFi ELM327.
- **A normal drive** afterwards — ideally a cold start and some load. Nothing is installed or modified.

# Honest expectations

- A **basic Audi profile** — coolant, oil temp, ATF temp, charge-air pressure, boost — is
  probably the **fastest first-vehicle profile yet**: the DIDs are already known to answer and the
  addressing is standard, so a single scan-and-correlate session should confirm the formulas.
- The combustion extras (rail pressure, timing, lambda, cam) are a sweep-and-correlate job like
  any other vehicle.
- Confirm the gearbox type and that the modules you want still speak classic CAN before assuming
  the capture's DIDs apply to your exact car.

**If you have a 2018-ish Audi Q5 2.0T and a laptop:** this is the one where the tooling and the
prior capture line up best — a scan session should get a working basic dash quickly.
