---
title: "Ford 6.7L Power Stroke — Scan Session Runbook"
subtitle: "Discovery on a truck nobody has mapped, with one visit to get it right"
date: "2026-08-09"
---

# Why this exists

[`SIERRA-GATE-RUNBOOK.md`](SIERRA-GATE-RUNBOOK.md) proves the scanner works on a truck
where every answer is already known. **This document is what happens next**, on a truck
where none of them are.

The difference matters more than it sounds. On the Sierra, a bad result is recognisable.
On a Power Stroke, **a broken scanner and a quiet ECU produce the same empty hit list**,
and you will not find out which one you had until you are home. Almost everything below
exists to keep that ambiguity from ever arising.

The second constraint is that some of this is unrepeatable. A cold start happens once per
visit, and it is the strongest identity confirmation the tool can produce. Skip it and it
is gone for this vehicle.

> **The rule: run the Sierra gate before you leave. An unvalidated scanner must not be
> the thing you discover is broken while parked in front of somebody else's truck.**

---

# This vehicle

| | |
| :--- | :--- |
| **Truck** | 2021 Ford F-350, 6.7L Power Stroke |
| **Transmission** | **10R140.** MY2020+ is the 10R140, MY2019 and earlier the 6R140 — the break is 2019→2020, *not* the 2023 facelift |
| **CAN-FD** | **Not a question here.** Pre-2023, so the unanswered 2023+ CAN-FD reachability issue does not apply |
| **Preset** | `--vehicle ford` |
| **Prior art** | None. No Power Stroke has ever run this firmware. See [`FORD-STATUS.md`](FORD-STATUS.md) |

**Calibrate your expectations before you go.** The Ford community data behind this preset
is unusually weak: of 25 claims put through three-vote adversarial verification, **19 were
refuted**. The most-cited list is a 2014 forum post about a 2013 truck with no rail
pressure, DEF, NOx, turbo-vane or oil-pressure entries at all, and a blank equation for
DPF differential pressure. Exactly one claim survived at Medium confidence — transmission
fluid temperature at `221E1C`.

So the realistic goal of this session is **a hit list and a good drive log**, not a
finished profile. Identification happens later, at a desk, from correlation.

---

# What to bring

| Item | Notes |
| :--- | :--- |
| MacBook | Synced to `main`, venv at `~/obd-venv`. Verify with the preflight below |
| **Vgate iCar Pro WiFi** | The scanner talks TCP only. **The vLinker MS cannot drive the scanner** — there is no BLE transport in `obd_scan` |
| **vLinker MS + the dash unit** | For the separate Standard+ check in step 6. The MS is bonded to the dash |
| Something to write on | Cold-start clock time, when you hit hills, anything odd |

---

# Before you leave the house

Two commands. Neither is optional.

**1. Preflight the laptop.**

-=-=-=-=-=-=-=-
```bash
cd ~/obd-gauge-cluster && git pull && cd tools && ~/obd-venv/bin/python -m pytest obd_scan/tests -q
```
-=-=-=-=-=-=-=-

**2. Run the Sierra acceptance gate on your own truck.** Fifteen minutes, parked, no drive
needed — see [`SIERRA-GATE-RUNBOOK.md`](SIERRA-GATE-RUNBOOK.md). Skipping this is how you
end up unable to tell a scanner regression from a silent Ford.

This is not boilerplate caution. Run it *whenever the toolchain moved*, and it has: a
Python upgrade, a rebuilt venv, a new pandas major version, or any change under
`tools/obd_scan/` all invalidate the last known-good result.

---

# Procedure

Every command runs from `~/obd-gauge-cluster/tools`, using the venv Python **by absolute
path** — no `activate`, so there is no "did I activate it?" ambiguity in a truck.

## 1. Plug in and join the adapter's network

Adapter into the OBD-II port, **engine running** (not just key-on — the port is unpowered
on some vehicles otherwise). Then join the adapter's SoftAP:

```
SSID: V-LINK   (open network, no password)
ELM327 over TCP at 192.168.0.10:35000
```

macOS will try to roam back to a known network, because `V-LINK` has no internet.
**Re-check the WiFi menu immediately before every command.**

If a stage reports `ADAPTER NOT REACHED` (exit 3), read the diagnosis — it distinguishes
wrong-WiFi from wrong-port from a wedged adapter, and each has a different fix. In
particular, "never answered `ATZ`" usually means another app still holds the adapter:
most WiFi ELM327s accept only one client at a time.

## 2. Census — who answers, and how

-=-=-=-=-=-=-=-
```bash
cd ~/obd-gauge-cluster/tools && ~/obd-venv/bin/python -m obd_scan census --vehicle ford -o ford_census.json
```
-=-=-=-=-=-=-=-

**Read the `alive:` line before doing anything else.** This is the single most important
output of the visit, because it decides whether the rest is even possible.

| What you see | What it means |
| :--- | :--- |
| 11-bit headers alive (`7E0`/`7E1`) | Normal SAE addressing. Proceed |
| Only 29-bit alive (`18DAxxF1`) | Also fine, and precedented — the Jeep Wagoneer's 11-bit path was **entirely dead** while its TCM answered on `18DA18F1`. Addressing is adapter-dependent, not just vehicle-dependent |
| `alive: (none)` **with** an `UNDETERMINED (transport fault)` banner | **Not a finding.** The link failed; nothing was learned. Fix the adapter and re-run |
| `alive: (none)` on a clean run | Genuine. Re-check engine running, then treat as a real result |

The preset sends three go/no-go probes per header: `0100`, plus `22F446` and `221E1C`.
**`221E1C` answering is the good sign** — it is the best-sourced Ford DID we have, so it
is the strongest available evidence that enhanced Mode-22 works on this truck. `22F446`
coming back silent is weak evidence of anything, since it comes from the list that mostly
did not survive verification.

## 3. Sweep — find every PID that answers

-=-=-=-=-=-=-=-
```bash
cd ~/obd-gauge-cluster/tools && ~/obd-venv/bin/python -m obd_scan sweep --vehicle ford --census ford_census.json -o ford_sweep.json
```
-=-=-=-=-=-=-=-

**This is the slow stage — budget roughly 100 seconds per live header** for the five-block
Ford preset. The `progress` line updates every probe. **Do not kill it thinking it hung.**

Nothing here identifies a parameter. A hit is an address that answered, full stop.

## 4. Cold-start log — the unrepeatable one

**Do this on the next cold start, before anything else that day.** Thick oil reads higher
pressure, and a cold-versus-hot comparison at the same RPM is what finally identified oil
pressure on the GM truck when correlation alone was ambiguous. There is no redoing it once
the engine is warm.

Start the log, *then* crank, and let it idle a couple of minutes.

-=-=-=-=-=-=-=-
```bash
cd ~/obd-gauge-cluster/tools && ~/obd-venv/bin/python -m obd_scan log --vehicle ford --sweep ford_sweep.json -o ford_cold.csv
```
-=-=-=-=-=-=-=-

Ctrl-C to stop. Everything already flushed stays on disk.

## 5. Drive log — with varied load

-=-=-=-=-=-=-=-
```bash
cd ~/obd-gauge-cluster/tools && ~/obd-venv/bin/python -m obd_scan log --vehicle ford --sweep ford_sweep.json -o ford_drive.csv
```
-=-=-=-=-=-=-=-

**Load is what makes parameters separate from each other.** Idle, cruise, hills or hard
throttle, some braking. A flat idle-only log makes every real sensor read as `no-signal`
or near-constant, and the whole correlation step is then worthless.

If the sweep produced a large hit list, narrow it with `--pids` — each cycle round-trips
every PID sequentially over one link, so fewer PIDs means denser samples per PID. Roughly
20–30 candidates is the sweet spot for a 15–20 minute drive.

## 6. Standard+ on the dash — free, and never yet done

Independent of the scan, and worth ten minutes. Plug the dash in with the vLinker MS.
**No flashing required**: `v0.4.0` already identifies Ford Super Duty by VIN and selects
the `std_diesel` profile.

**Standard+ has never run on real hardware.** Alan's Sierra uses its scanned Duramax
profile, so no release has ever exercised it. What to record:

- Does the splash name the truck correctly — "Ford F-350"?
- **Which tiles stay blank?** That is the actual signal. A blank tile means a legislated
  SAE PID this truck does not answer, and that is the data that improves the profile.

---

# What to bring home

| File | Why |
| :--- | :--- |
| `ford_census.json` | Settles the addressing question permanently |
| `ford_sweep.json` | The hit list |
| `ford_cold.csv` | Unrepeatable |
| `ford_drive.csv` | The load data correlation needs |
| Notes | Cold-start time, terrain, blank dash tiles |

`correlate` needs no adapter and runs at a desk afterwards:

-=-=-=-=-=-=-=-
```bash
cd ~/obd-gauge-cluster/tools && ~/obd-venv/bin/python -m obd_scan correlate ford_drive.csv -o ford_report.md --pdf
```
-=-=-=-=-=-=-=-

---

# Known limits — decide in advance, not in the cab

**MS-CAN may be where the interesting data lives.** Ford has historically put some
aftertreatment, DEF and NOx parameters on MS-CAN (pins 3/11) rather than HS-CAN (6/14). A
single-bus adapter physically cannot see it. Two honest gaps: whether the iCar Pro reaches
MS-CAN is **unverified**, and the vLinker MS — which *is* multi-bus — **cannot drive the
scanner at all**, because `obd_scan` speaks TCP and the MS is BLE.

So if DEF/NOx/aftertreatment PIDs are simply absent from the sweep, **that is not a
finding about the truck.** It is an open question requiring a multi-bus WiFi adapter and a
second visit. Record it as unresolved rather than concluding the data is not exposed.

**Some parameters may genuinely not be readable.** On the GM truck, DPF soot load, regen
status and turbo vane position were swept for and never found at any address tried. The
same may be true here.

**Never probe `7E3`–`7E7`.** The preset already excludes them, so this only matters if you
hand-edit a census: on the 2018 Audi Q5, reading a driver-assist module's DIDs tripped
pre-sense and dashboard warnings on somebody else's car. `_rehydrate_alive_headers` refuses
headers a preset never declared, precisely so an edited `census.json` cannot steer a sweep
onto a chassis module.

---

# Triage

Ordered by how often each actually bit us.

| Symptom | Likely cause | What to do |
| :--- | :--- | :--- |
| `ADAPTER NOT REACHED`, timed out | macOS roamed off `V-LINK` | Re-join the SoftAP. It has no internet, so this happens constantly |
| `ADAPTER NOT REACHED`, never answered `ATZ` | Another client holds the adapter | Close Car Scanner / Torque on your phone. Power-cycle the adapter |
| **Everything returns NO DATA** | ELM protocol auto-search latched wrong | Power-cycle the adapter (unplug 10 s). A latched protocol survives a soft reset |
| Census finds nothing on either addressing mode | Engine off, or wrong network | Confirm the engine is *running*. Re-check WiFi |
| Sweep looks frozen | It isn't | ~100 s per header. Watch the `progress` line |
| Lots of hits, all constant in `correlate` | Idle-only drive | Re-drive with real load. This is the most common way a session is wasted |
| `221E1C` silent but 11-bit alive | Enhanced may be gated or addressed differently | Record it. Do not conclude Mode-22 is unsupported from one DID |
