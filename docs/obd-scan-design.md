---
title: "OBD Discovery Scanner — Design Spec"
subtitle: "Host-side Python tool for empirically mapping an unknown vehicle's enhanced PIDs"
date: "2026-07-21"
---

# 1. Purpose

Build a host-side (laptop) Python tool that discovers an unknown vehicle's OBD-II
addressing and enhanced-PID map empirically, then proves the identity of what it finds by
correlation against known anchors.

**Immediate driver:** porting the OBD stat display to the Ford 6.7L Power Stroke Super
Duty. A prior research pass established that the
addressing mode, the HS-CAN vs MS-CAN topology, and essentially the entire Ford enhanced
PID map are **unknown and not credibly documented anywhere public**. The scan session is
primary evidence, not confirmation of a table.

**Available for the work:** a 2018 (6R140) and a 2021 (10R140) Super Duty, laptop in the
cab, stationary session plus a drive on each.

## 1.1 Why host-side and not the existing firmware scanner

The firmware already has a range-scanner (`BleObdSource::runScan`, serial `'x'`). It is
not the right tool once a laptop is present, and this spec explicitly **does not modify
it**:

| | Firmware `runScan` | This tool |
| :--- | :--- | :--- |
| Probe cost | ~1.4 s worst case (re-sends `AT SH` every probe, fixed 700 ms deadlines) | ~60–100 ms (header set once per block, `ATAT2` adaptive timing) |
| Iterating on findings in the cab | Flash cycle | Edit and rerun |
| Output | `Serial.printf` only — needs a USB console, nothing reaches SD | JSON + CSV + markdown/PDF report |
| Hit classification | Substring search for `"62"`/`"6C"` — false-positives on data bytes | Parsed UDS response with echo matching |
| Screen | Frozen 4–5 min | Not involved |

The firmware scanner's GM job is complete. Generalizing it would be work in service of a
tool this one makes obsolete. It stays as-is.

## 1.2 Non-goals

- No firmware changes of any kind in this project.
- No `vehicles/ford_super_duty.cpp`. The profile gets written from measured data **after**
  a scan session, as separate work.
- No MS-CAN support in v1 (see §3.1 — deferred, with a defined trigger for revisiting).
- No write/actuation capability, ever (see §5).

# 2. Architecture

Location: `tools/obd_scan/` — a Python package, alongside the existing
`tools/analyze_logs.py` and `tools/ui_snapshot/`, versioned with the firmware repo.

A package rather than a single file because the parts are genuinely separable and several
are pure functions worth testing in isolation.

| Module | Responsibility | Purity |
| :--- | :--- | :--- |
| `elm.py` | TCP socket to the adapter; ELM327 session (init, header set, command/response to the `>` prompt); adaptive timing | I/O |
| `reply.py` | Classify and parse a raw ELM reply; multi-frame ISO-TP assembly; Mode-01 supported-PID bitmap decode | **Pure** |
| `catalog.py` | Data-driven definitions: candidate headers, addressing modes, PID blocks, anchors, per-vehicle presets | Data |
| `stages.py` | The `census` / `sweep` / `log` session logic | I/O |
| `correlate.py` | Offline analysis: interpretation enumeration, correlation ranking, classifiers | **Pure** |
| `report.py` | Markdown + optional PDF emission (via pandoc if present) | I/O |
| `__main__.py` | `argparse` CLI | — |

**Transport (v1): TCP only.** Vgate iCar Pro WiFi — SoftAP `V-LINK`, transparent ELM327 at
`192.168.0.10:35000`, no pairing or bonding. A plain socket, which avoids macOS BLE
entirely. Host/port are CLI-overridable. The `elm.py` interface is written so a BLE
transport could be substituted later without touching the stages.

## 2.1 CLI

Four subcommands mapping to the four phases of a session:

```
obd_scan [--host H] [--port P] census [--vehicle V] [-o census.json]
obd_scan [--host H] [--port P] sweep  [--census census.json] [--vehicle V] [-o sweep.json]
obd_scan [--host H] [--port P] log    [--sweep sweep.json] [--vehicle V] [--pids P[,P..]] [--hz 1.0] [-o drive.csv]
obd_scan correlate drive.csv [-o report.md] [--pdf] [--workers N]

V = ford | gm | bmw | audi | auto        (default: auto — preset chosen from the VIN)
```

**`--host` and `--port` are root arguments and must come *before* the subcommand** — they
configure the ELM transport shared by the three on-vehicle stages. `obd_scan census --host H`
is a usage error, not a synonym. `correlate` is offline and reads only the CSV.

`--pids` on `log` restricts the drive to an explicit list instead of everything `sweep` found —
this is what a focused cold-start confirmation drive uses to get dense samples on a handful of
candidate DIDs rather than thin coverage across hundreds.

`--workers` on `correlate` per the project parallelism mandate; the interpretation ×
anchor cross-product is embarrassingly parallel per PID. Default `min(cpu_count(), 4)`.

# 3. Stage design

## 3.1 `census` — addressing and responder discovery

The highest-value 2 minutes of the session. Answers "who is out there and how do I talk to
them," which no source could answer for Ford.

1. **Link + protocol detect.** `ATZ`, `ATE0`, `ATL0`, `ATS0`, then `ATSP0` and a single
   `0100` with a **~10 s window**, then `ATDP`.
   *Critical:* any host character aborts the ELM's protocol auto-search. A short timeout
   here is what latched a wrong protocol on the Sierra and cost a debug cycle. The wide
   window is mandatory, not defensive.
2. **Responder census.** For every candidate header in `catalog.py`, in **both** addressing
   modes, send cheap universal probes and record the classified result:
   - 11-bit: functional `7DF`; physical `7E0`, `7E1`, `7E2` only (`ATSP6`, `ATSH nnn`)

     **A powertrain scan must never probe `7E3`–`7E7`.** The full physical range reaches
     chassis and ADAS modules: on the 2018 Audi Q5, `7E4` is a driver-assist controller and
     reading its DIDs tripped pre-sense and "RPM too high" dashboard warnings. Presets
     therefore default to the narrowed powertrain pool (`HEADERS_11BIT_PT` in `catalog.py`),
     not the full `HEADERS_11BIT`.
   - 29-bit: functional `18DB33F1`; physical `18DAxxF1` over a curated `xx` list
     (`10, 18, 1A, 28, 00, 11, 17, 58, 60, 7E`) (`ATSP7`, `ATCP18`, `ATSH DAxxF1`)
   - Probes per header: `0100` (Mode-01 support bitmap — near-universal) and one enhanced
     read from the vehicle preset.
3. **Generic PID census per live responder.** Walk the Mode-01 support bitmaps
   (`0100/0120/0140/0160/0180/01A0/01C0`) and decode them — 7 probes yields the module's
   exact supported generic-PID list, free. Also attempts the Mode-22 bitmap equivalents
   where a vehicle preset suggests one.

**Output:** `census.json` — per header: addressing mode, classification, raw replies,
decoded supported-PID list. Plus a printed summary table.

**Why negative responses matter here.** A `7F 22 31` (request out of range) *proves the
header reached a live module*. So headers split three ways, not two:

| Result | Meaning |
| :--- | :--- |
| Positive response | Live module, PID exists |
| **Negative response (`7F`)** | **Live module, that PID doesn't exist — addressing CONFIRMED GOOD** |
| Silence / `NO DATA` | No module at that address (or wrong bus) |

The firmware scanner discards this distinction; it is the single most useful signal for
the Ford addressing question.

**MS-CAN (deferred).** v1 sweeps HS-CAN only, because the chosen adapter is single-bus.
**Defined trigger for revisiting:** if the census + sweep find *no* aftertreatment/DEF/NOx
data anywhere on HS-CAN, that is the evidence that it lives on MS-CAN (pins 3/11), and the
session repeats with the multi-bus vLinker MS. Recording this as a falsifiable prediction
rather than an assumption.

## 3.2 `sweep` — block sweep of confirmed responders

Brute-forcing the full 16-bit Mode-22 space (65,536 PIDs/module) is infeasible at any
probe rate. Sweeping *known-populated blocks* is what made the GM scan tractable
(512 probes), and the community list tells us which blocks Ford populates.

Candidate Ford blocks (all UNVERIFIED for 2018/2021 — they are sweep targets, not claims):

| Block | Why |
| :--- | :--- |
| `22F4xx` | Densest known: ambient `F446`, EGT `F478`, oil temp `F45C`, regen `F48B`, fuel level `F42F` |
| `2204xx` | Distance since regen `0434`, soot `042C` |
| `2211xx` | DPF ΔP `116C` |
| `221Exx` | Trans fluid temp `1E1C` |
| `2200xx` | The GM mirror block — cheap to test whether Ford does the same |

GM blocks (`2200xx`, `2219xx`) stay in the catalog as the **regression fixture** (§6.3).

Per block: set the header **once**, sweep `00`–`FF`, classify each reply, record raw hex
for positives. Progress and ETA to stdout. Link-loss aborts cleanly with partial results
written, rather than burning the remaining probes on timeouts.

**Output:** `sweep.json` — every hit with header, PID, raw reply, payload length,
classification.

## 3.3 `log` — live capture during the drive

Round-robin poll of the sweep's hit list plus the anchor set, target ~1 Hz per PID
(actual rate is bounded by hit count × probe latency and is recorded in the CSV header).

**CSV schema:** wide. `iso_time`, `uptime_ms`, one column per hit PID holding **raw hex**,
plus decoded anchor columns:

`rpm_010C` · `speed_010D` · `load_0104` · `coolant_0105` · `maf_0110` · `baro_0133` ·
`ambient_0146`

Raw hex is stored deliberately: a wrong decode guess in the field must not destroy data
that a better guess at home could use. Anchors are generic Mode-01 PIDs near-certain to
exist on any OBD-II vehicle, so correlation always has references even on a fully
unmapped platform.

Append-and-flush per row (the firmware SD logger's lesson — a lost session is worse than a
slow one).

## 3.4 `correlate` — identity proof

A sweep proves a PID *answers*. This stage proves what it *is*. It automates the manual
reasoning from a prior oil-pressure identification pass.

**Core method — enumerate interpretations rather than assume one:**

> For each unknown PID, enumerate every plausible reading of its payload — `u8` at each
> byte offset, `u16` big-endian at each offset pair, and `s16` where sign is plausible —
> then correlate **each interpretation** against **each anchor**. Report the
> best-scoring (interpretation, anchor) pair with its coefficient.

This is what turns "`22F478` answered with 9 bytes" into "bytes 1–2 as `u16` correlate
0.94 with RPM, span 300–1000 — an EGT sensor."

**Supporting classifiers:**

- **Constant across the drive** → not a live sensor. Removes most sweep noise.
- **Sentinel-only** (`0x00`, `0xFF`, `0xFFFF`) → unsupported slot. Directly matches the
  NOx-65535 and intake-`0x00` traps already hit on the GM.
- **Plausible-range check** once a scale is proposed.

**Output:** ranked markdown + PDF report — per candidate: best interpretation, best anchor
and coefficient, observed range, classifier verdicts, and a suggested identity with an
explicit confidence rating.

**Procedural note for the runbook (not code):** the LZ0 oil-pressure identity was finally
proven by a **cold-vs-hot comparison at the same RPM** — thick oil reads higher. One cold
start per Ford is worth more than an extra hour of sweeping.

# 4. Reply classification

Replaces the firmware's `scanIsPositive()` substring search, which matches `"62"` anywhere
in the reply and therefore false-positives on data bytes.

| Class | Detection | Meaning |
| :--- | :--- | :--- |
| `POSITIVE` | First byte `0x62` **and** echoed PID bytes match the request | Real data |
| `NRC_OUT_OF_RANGE` | `7F 22 31` | Module alive, PID absent |
| `NRC_PENDING` | `7F 22 78` | Response pending — wait and re-read |
| `NRC_OTHER` | `7F 22 <nrc>` | Service unsupported (`11`/`12`), security (`33`), etc. |
| `NO_DATA` | `NO DATA` or empty | Nothing answered |
| `ELM_ERROR` | `?`, `BUFFER FULL`, `CAN ERROR`, `STOPPED`, `UNABLE TO CONNECT` | Transport fault — retry; never recorded as a result |

**Echo matching is the key upgrade:** a `22F446` request must be answered by
`62 F4 46 ...`. This eliminates the false-positive class entirely.

**Multi-frame ISO-TP assembly** is ported from `src/obd_parse.cpp` — the `LLL\r0:xx\r1:xx`
format, length truncation, dropped-fragment rejection, and `0x55` pad stripping. The Ford
EGT block is a multi-sensor payload and **will** be multi-frame.

**`AT SH` returns its own `OK>`**, which must be consumed separately or it is mistaken for
the data reply — a bug already paid for once on the GM build.

# 5. Safety — read-only by construction

The tool will be used on **vehicles belonging to other people**.

**Hard request whitelist, enforced in code:** only modes `01`, `03`, `09`, `22` (all
reads) and an explicit AT-command allowlist may be transmitted.

**Rejected unconditionally:** `2E` (write data by identifier), `31` (routine control), `2F`
(input/output control), `11` (ECU reset), `14` (clear DTCs), `27` (security access), `10`
(session control beyond default), `28` (communication control), `85` (control DTC
setting).

Enforcement points:
1. `elm.py` refuses to transmit any non-whitelisted request — the last line of defence.
2. `catalog.py` validation runs at **startup**, so a malformed block or a typo'd hex digit
   fails before the session begins rather than mid-sweep.

A mistyped sweep range must not be able to become a write or start a routine on a borrowed
vehicle. This is a correctness requirement, not a convention.

# 6. Testing

## 6.1 Unit tests — pure functions, no hardware

Plain `assert`-style to match the firmware repo's test idiom:

- `reply.py`: every classification row in §4; echo mismatch rejection; multi-frame
  assembly incl. dropped fragments and pad stripping; Mode-01 bitmap decode
- `correlate.py`: interpretation enumeration completeness; correlation math against
  synthetic signals with a known planted answer; constant/sentinel classifiers
- `catalog.py`: whitelist validation rejects a non-read mode

## 6.2 Fake ELM327 server — full flow on the bench

A local socket server replaying canned responses, including the failure modes:
multi-frame replies, `7F 22 31`, `BUFFER FULL`, protocol-search timing, and a mid-sweep
disconnect. Lets `census`→`sweep`→`log` be exercised end-to-end with no vehicle — which is
what makes the tool trustworthy in a stranger's truck.

## 6.3 Sierra regression gate — the real acceptance test

A `--expect` fixture of known-true LZ0 answers. The tool must rediscover, on your own
truck, before it goes to either Ford:

- `221940` @ trans header — transmission temp
- `220078` @ ECM header — EGT block (multi-frame ISO-TP, 3 sensors behind a presence mask,
  so it also proves fragment assembly works)
- `22000B` / `22000F` @ ECM header — MAP and intake (the Mode-22 mirror discovery)
- `220023` @ ECM header — fuel rail pressure
- `22009B` @ ECM header — DEF level (byte[3]; byte[1] is concentration and reads stuck)
- The `2200xx` mirror block broadly
- The known 29-bit-vs-11-bit adapter-dependent behavior

The fixture itself is `tools/obd_scan/tests/sierra_expect.json`, and its `must_find` list is
the authoritative version of the table above — update that file, not this prose.

**Rationale:** the Sierra is the only vehicle where every answer is already known. A tool
that cannot rediscover them is broken, and that must be found in your own driveway rather
than during a borrowed truck's one session.

# 7. Acceptance criteria

1. All unit tests pass; the fake-ELM server exercises `census`→`sweep`→`log` end-to-end
   with no hardware.
2. The safety whitelist is enforced and tested — a catalog containing a write mode fails
   at startup.
3. Run against the Sierra, `census` correctly reports the adapter's addressing mode and
   the live module headers.
4. Run against the Sierra, `sweep` rediscovers every item in the §6.3 fixture.
5. `correlate` over a fresh Sierra `log` capture identifies RPM-correlated candidates and
   flags constants/sentinels correctly. (Note: the firmware's own SD-card drive logs use a
   *decoded* schema, not this tool's raw-hex schema — they are not valid input to
   `correlate`; use a capture from `obd_scan log`.)
6. A probe averages well under 200 ms against a real adapter, making a multi-block Ford
   sweep practical within a stationary session.
7. Report output follows the md + PDF convention.

# 8. Open items deliberately left out of scope

- **MS-CAN** — deferred with the falsifiable trigger in §3.1.
- **CAN-FD (2023+ Super Duty)** — irrelevant to the 2018/2021 trucks; unresolved in
  research; revisit only if a 2023+ truck becomes available.
- **The Ford vehicle profile itself** — written from measured data after a session.
- **Ford alarm thresholds** — every threshold claim except DEF staging was refuted in
  research; needs its own sourcing pass, modelled on the GM alarm-threshold review.
- **`VehicleProfile` struct changes** that Ford will force (per-VIN tank capacity across
  six fuel sizes; range-based vs level-based DEF alarm policy). Recorded here so they are
  not lost, but they belong to the profile work, not the scanner.
