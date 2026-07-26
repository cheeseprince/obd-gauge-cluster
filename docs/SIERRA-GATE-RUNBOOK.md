---
title: "Sierra Acceptance Gate — Runbook"
subtitle: "Prove the scanner works on a truck where we already know every answer"
date: "2026-07-22"
---

# Why this exists

The scanner is about to be used on vehicles belonging to other people, with limited
time and no second chances. On an unmapped truck, **a broken scanner and a quiet ECU
look exactly the same** — you come home with an empty hit list and no way to tell which
one you had.

The Sierra is the only vehicle where every answer is already known. It is therefore the
only place where a failure of this tool is *recognisable*.

> **The gate: the scanner must rediscover, from scratch, the PIDs already shipping in
> the firmware's vehicle profile. If it cannot find what we know is there, it does not
> go near a borrowed truck.**

Time: about 15 minutes, parked. No drive needed for the gate itself.

---

# What to bring

| Item | Notes |
| :--- | :--- |
| Laptop | Any laptop with this repo and Python 3.12 |
| **Vgate iCar Pro WiFi adapter** | The one bought for the knob. **Not** the vLinker MS — that one is bonded to the dash |
| The truck | Parked, engine **running** (idle is fine) |

Engine running matters: several enhanced PIDs return nothing with the key merely on.

---

# Procedure

## 1. Plug in and join the adapter's network

Plug the iCar Pro into the OBD-II port under the dash. Wait ~10 seconds for it to boot,
then join its WiFi from the laptop:

-=-=-=-=-=-=-=-
```bash
# SSID: V-LINK   (open network, no password)
# The adapter serves ELM327 over TCP at 192.168.0.10:35000
```
-=-=-=-=-=-=-=-

Confirm you can reach it before going further:

-=-=-=-=-=-=-=-
```bash
nc -vz 192.168.0.10 35000
```
-=-=-=-=-=-=-=-

> **If this fails**, nothing downstream will work. Check you actually joined `V-LINK`
> (laptops love to hop back to a remembered network) and that the adapter's light is on.

## 2. Census — who answers, and how

-=-=-=-=-=-=-=-
```bash
cd tools
python3 -m obd_scan census --vehicle gm -o sierra_census.json
```
-=-=-=-=-=-=-=-

**What you should see:** a table of candidate headers with an `evidence` column, then an
`alive:` line.

**The interesting part, and the thing worth photographing:** which *addressing mode*
answers. On this truck the two adapters disagree — the vLinker MS reads it on **11-bit**
(`7DF`/`7E0`/`7E2`), while the iCar Pro previously only answered on **29-bit**
(`18DB33F1`/`18DAxxF1`). So expect the `7Ex` headers to come back silent and the
`18DAxxF1` ones alive.

**That disagreement is itself a passing result** — it means the census is correctly
reporting the addressing rather than assuming it. Note which mode won.

> **Read the banners, not just the `alive:` line.** If you see
> `*** SCAN INCOMPLETE ***` or a line about headers being UNDETERMINED, the link
> failed — that is not a finding about the truck. Re-seat the adapter and start again.

## 3. Sweep — find every PID that answers

-=-=-=-=-=-=-=-
```bash
python3 -m obd_scan sweep --census sierra_census.json --vehicle gm -o sierra_sweep.json
```
-=-=-=-=-=-=-=-

This sweeps the GM blocks (`2200xx` and `2219xx`) at each live header. **Expect roughly
100 seconds per header** — it is not hung. Progress prints as it goes.

## 4. The gate

-=-=-=-=-=-=-=-
```bash
python3 -m obd_scan.check_expect sierra_sweep.json obd_scan/tests/sierra_expect.json
```
-=-=-=-=-=-=-=-

It checks six known-true PIDs plus a minimum hit count:

| PID | What it proves |
| :--- | :--- |
| `221940` | Transmission temperature — the project's original crux, matches the dash |
| `220078` | EGT block — **multi-frame ISO-TP**, so this one proves fragment assembly works |
| `22000B` | MAP — only findable because the enhanced range mirrors the generic block |
| `22000F` | Intake air temp — the other half of that discovery |
| `220023` | Fuel rail pressure |
| `22009B` | DEF level |

**`GATE PASSED`** → the tool works. It can go to the Fords.

**`GATE FAILED`** → see triage below. Do not take it to a borrowed truck.

**`GATE INVALID`** → the sweep was truncated by a link failure. The results mean nothing
either way; fix the link and re-run.

---

# If the gate fails

Work down this list — it is ordered by how often each cause actually bit us.

| Symptom | Likely cause | What to do |
| :--- | :--- | :--- |
| **Everything returns NO DATA** | The ELM's protocol auto-search got aborted and latched the wrong protocol. Any host character during the search kills it. | The tool gives the first `0100` a 10 s window specifically to avoid this. If it still happens, power-cycle the adapter (unplug 10 s) and re-run — a latched protocol survives a soft reset. |
| **Census finds nothing on either addressing mode** | Wrong network, dead adapter, or engine off | Re-check step 1. Confirm the engine is *running*, not just key-on. |
| **Some expected PIDs found, `220078` missing** | Multi-frame assembly | `220078` is the only fixture PID whose reply spans frames. If everything else passes and it alone fails, that isolates the bug to fragment handling. |
| **Fewer hits than expected but the six are found** | Possibly fine | `min_hits` is a sanity floor, not a specification. If all six named PIDs are present, the tool is working; note the count and move on. |
| **Gate passes on 11-bit, not 29-bit (or vice versa)** | Not a failure | Adapter-dependent addressing is expected on this truck. What matters is that the census *reported* which one worked. |

---

# Optional, and worth doing while you are there

Neither is required for the gate, but both are cheap once the laptop is already in the cab:

**A. Log a short drive.** Even ten minutes of varied load exercises the `log` stage and
gives `correlate` real data to chew on:

-=-=-=-=-=-=-=-
```bash
python3 -m obd_scan log --sweep sierra_sweep.json -o sierra_drive.csv
# Ctrl-C when done
python3 -m obd_scan correlate sierra_drive.csv -o sierra_report.md --pdf
```
-=-=-=-=-=-=-=-

Because we already know what these PIDs *are*, the correlation report is checkable: if
`221940` doesn't come back correlated with something temperature-shaped, the analysis
half of the tool has a problem the gate alone wouldn't catch.

**B. A cold-start capture.** If the truck has been sitting overnight, run `log` for the
first few minutes from cold. A cold-versus-hot comparison at the same RPM is the single
strongest identity confirmation available — it is what finally proved oil pressure — and
having a known-good example makes the Ford analysis easier to trust.

---

# After the gate passes

1. Note which addressing mode the iCar Pro used — that goes in the Ford session notes,
   since it is the first thing to try there.
2. Keep `sierra_sweep.json`. It is the reference output: if the tool ever behaves oddly
   on another vehicle, diffing against a known-good sweep is the fastest way to tell
   whether the tool or the vehicle changed.
3. The Ford runbook is `PORTING-LESSONS.md` §9 — same four commands, unknown answers.
