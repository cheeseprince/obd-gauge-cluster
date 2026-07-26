---
title: "Porting to a New Vehicle — Lessons Learned"
subtitle: "Hard-won findings from mapping a 2025 GM Duramax from scratch. Read before starting any new vehicle."
date: "2026-07-21"
---

# Who this is for

You are adding support for a vehicle nobody has mapped yet — a BMW F10, a Ford Power
Stroke, another GM engine. You have the firmware, the host scan tool, and a car.

This document is everything the GM port learned the expensive way. Most of it is not
obvious, several items cost a debugging session each, and two of them produced *wrong
readings that looked correct*. Read it before you write a line of vehicle profile.

**The single most important sentence in this document:** the public PID data for your
vehicle is probably wrong, and you must measure rather than trust.

---

# 1. The method

Enhanced (Mode 22 / UDS) PIDs are not standardized. Each manufacturer picks its own
identifiers, byte layouts, scaling and module addresses, and does not publish them. There
is no lookup table to find. There is only measurement.

The loop that works:

| Step | What | Why |
| :--- | :--- | :--- |
| **1. Census** | Probe every candidate header in both addressing modes; record who answers | Establishes addressing before you waste time sweeping the wrong address |
| **2. Sweep** | Brute-force known-populated PID blocks at each live module; log every address that answers with raw bytes | Produces the candidate set |
| **3. Log** | Poll every hit at ~1 Hz alongside known anchors during a real drive; store **raw hex** | Movement is what distinguishes a live sensor from a constant |
| **4. Correlate** | For each unknown, enumerate every byte interpretation and score each against every anchor | Turns "address 22F478 answered" into "bytes 1–2 as u16 track RPM at r=0.94" |
| **5. Confirm** | Prove identity with a physical experiment, not just statistics | Correlation shows association, not identity — see §4 |

The host tool at `tools/obd_scan/` implements steps 1–4.

## 1.1 A negative response is a *finding*, not a failure

`7F 22 31` (requestOutOfRange) means **a real module received your request and rejected
that identifier**. That is proof the addressing is correct. Silence means nothing is
there.

So headers sort into three groups, not two:

- **Positive reply** → module alive, PID exists
- **Negative reply** → module alive, PID does not exist — *addressing confirmed*
- **Silence / NO DATA** → nothing at that address, or wrong bus

The original on-device scanner threw away negative responses, which made "wrong address"
and "right address, wrong PID" indistinguishable. Do not repeat that.

## 1.2 "Nothing answered" and "we couldn't ask" must never look the same

A review of our own census caught this, and it is the most dangerous class of bug a
discovery tool can have.

If the adapter is unplugged mid-census — or the socket drops, or the link wedges — a naive
implementation records that header as *silent*. Transport failures persist, so every
remaining header is recorded silent too, and the tool cheerfully reports **"this vehicle
has no modules."** You are sitting in someone's driveway on borrowed time; you conclude
the car exposes nothing and pack up. The real fault was a loose connector.

Worse, the same failure occurring a moment later — during a probe rather than a header
set — crashed the run outright. Identical fault, opposite outcome, decided by timing.

**Rules:**

- Give "we could not determine this" its own result class, distinct from both "answered"
  and "did not answer."
- Guard every transport call the same way. Inconsistent error handling turns one fault
  into two different wrong answers.
- On a persistent transport failure, **stop and say so loudly**. Continuing produces a
  page of meaningless "silent" rows that read exactly like a real negative result.
- Make "probed and found silent" distinguishable from "never reached" in whatever you
  write out. Those support opposite conclusions.

Absence of evidence is a finding. Failure to gather evidence is not — and a tool that
cannot tell them apart will confidently report the wrong one.

---

# 2. Transport traps

Every one of these produced a session of confusion on the GM truck.

## 2.1 Addressing is adapter-dependent — this one is genuinely surprising

**The same vehicle answered different addressing depending on which adapter was plugged
in.** A Vgate vLinker MS reads the truck on **11-bit** headers (`7DF` functional, `7E0`
engine, `7E2` transmission). A Vgate iCar Pro WiFi, on the same truck reading the same
parameters, only ever answered on **29-bit** (`18DB33F1` functional, `18DAxxF1` physical).

Consequence: **"it returns NO DATA" never proves the vehicle lacks the data.** Sweep both
addressing modes before concluding anything. If a whole class of data is missing, suspect
addressing or bus before suspecting the vehicle.

## 2.2 The protocol auto-search is aborted by any host character

After `ATSP0`, the adapter performs a protocol search that can take **several seconds**.
Any byte you send during it aborts the search and can latch the adapter to a *wrong*
protocol, after which every PID returns NO DATA forever.

Give the first `0100` after `ATSP0` a **~10 second** window, not a normal query timeout.
A 400 ms timeout here cost a full debugging session, and the symptom — everything reads
NO DATA — looks exactly like "this vehicle doesn't support anything."

## 2.3 `AT SH` returns its own `OK>` prompt

Setting a header emits an acknowledgement. If you do not consume it separately, your next
read grabs `OK` instead of the data reply and every query appears to fail. Same class of
symptom, different cause.

## 2.4 Enhanced replies are multi-frame

Anything longer than a few bytes arrives as ISO-TP fragments: a bare length line (3 hex
digits = total byte count) then `N:<hex>` fragment lines. If you do not assemble them, the
long and interesting PIDs — multi-sensor blocks especially — simply never parse, while
short ones work fine. That asymmetry is a strong hint you are missing assembly.

Two rules learned the hard way:

- **Truncate to the declared length.** Padding sits beyond it.
- **Do not strip padding bytes before the length check.** A payload whose genuine last
  byte equals the pad value gets shortened, fails the check, and is reported empty — a
  silently missed PID.

## 2.5 Set the header once per block, not per probe

Re-sending `AT SH` before every probe roughly doubles the command count. With adaptive
timing (`ATAT2`) and a short `ATST`, a NO DATA returns in ~60–100 ms instead of a fixed
timeout. This is the difference between a sweep that fits in a parked session and one
that does not.

---

# 3. Decoding traps — the ones that produce *plausible wrong answers*

These are worse than crashes, because the display shows a believable number.

## 3.1 Sentinel and short frames decode as convincing garbage

Real cases from the GM truck:

| Raw | Naively decodes as | Actually |
| :--- | :--- | :--- |
| `0xFF` in a temperature byte | 419 °F — triggered a critical alarm | Unsupported / no data |
| `0x00` in a temperature byte | −40 °F — logged on 10% of rows | Sensor not reporting |
| `0xFFFF` in a NOx word | 65535 ppm | Padding |
| Zero-length / truncated frame | 0 V — full-screen voltage alarm | Frame loss |

**Guard every decoder**: reject sentinels, reject frames shorter than the fields you are
reading, and return NaN rather than a number. Then keep the last good value instead of
displaying the NaN.

## 3.2 The byte you want may not be the byte you assumed

The DEF level tile read a plausible ~52% and **never moved after a refill**. The PID
returned four bytes and the decoder was reading byte[1]; that byte was DEF
*concentration* (~32.5% urea, correctly near-constant). The actual level was byte[3].

A value that looks reasonable but does not respond to a change you *caused* is the
signature of a wrong byte offset. Test by changing the world — fill the tank, let it warm
up, load the engine — and confirm the number follows.

## 3.3 A sensor's floor is not necessarily zero

Oil pressure raw values bottomed at 36, not 0. The first scale hypothesis (raw ÷ 2)
implied **18 psi with the engine off**, and its low-pressure alarm threshold sat *below*
the sensor's physical floor — meaning the alarm could never fire under any condition. The
correct form was `(raw − 36) × 0.6`.

Before fitting a scale, find the floor: sample the parameter with the engine **off**.

## 3.4 Engine-off is harder to detect than you think

On a truck with auto stop-start, **RPM reads 500–700 during spin-down**, so RPM cannot
distinguish "idling" from "just shut off." Fuel flow can — it goes to 0.00 immediately.

This mattered because "warm idle" samples were contaminated with engine-off rows, which
skewed a scale fit. Whatever vehicle you are on, find a parameter that is unambiguously
zero when the engine is not running, and use it to filter.

## 3.5 Check whether the enhanced range mirrors the generic range

On the GM truck, several generic Mode-01 PIDs returned NO DATA while the **same
parameters answered at `2200xx` in Mode 22** — a mirror of the `01xx` block. That is how
boost (`22000B`) and intake temp (`22000F`) were finally found after the generic PIDs
appeared unsupported.

Worth an explicit test on any new vehicle: sweep `2200xx` and compare against the generic
PIDs you already know work.

---

# 4. Identity: correlation is not proof

Correlation tells you two signals move together. It does not tell you which one you are
looking at. Boost, MAF, load and fuel rate all correlate strongly with each other and with
RPM; a high `r` narrows the field but does not pick a winner.

**What actually settled it on the GM truck: a physical experiment with a known direction.**

Oil pressure was confirmed by a **cold-versus-hot comparison at the same RPM** — thick
cold oil reads higher pressure. Same RPM, same load, different temperature, different
value, in the direction physics requires. Nothing else in the candidate set behaves that
way.

Design one such experiment per parameter class:

| Parameter | Discriminating experiment |
| :--- | :--- |
| Oil pressure | Cold start vs warm, same RPM — cold reads higher |
| Any temperature | Warm-up curve from cold — monotonic rise, then plateau |
| Boost / MAP | Throttle transient — must move *fast*, and sit near barometric at idle |
| Transmission temp | Compare against the vehicle's own display if it has one |
| Gear | RPM ÷ output-shaft speed against the published ratio ladder |
| Fuel level | Refuel and confirm it climbs |

**Get one cold start.** It is worth more than an extra hour of sweeping.

---

# 5. Do not trust community PID lists

A structured research pass on Ford (19 sources, 79 candidate claims, 25 adversarially
verified) **refuted 19 of 25**. The most-cited community Ford 6.7L list turned out to be a
2014 forum post about a 2013 truck, containing **no** rail pressure, DEF, NOx, VGT, EGR,
oil pressure, MAF or torque PIDs at all, and with a *blank* equation for DPF differential
pressure.

Rules that follow:

- Treat any list as a **hypothesis set for the scanner**, never as an answer.
- Check the model year the data came from. A list for a vehicle five years older than
  yours is a different ECU.
- Community entries marked "still testing" are exactly that.
- Cross-vendor agreement (two independent tool databases with algebraically identical
  formulas) is meaningfully stronger evidence than a forum post.
- **Signedness is a real trap.** One verified transmission-temp formula reads ~4096 °C on
  a sub-zero cold start if decoded unsigned. Use signed types where a value could
  plausibly go negative.

Also: some parameters are **calculated by commercial tools, not read from the vehicle** —
air density, pressure altitude, CFM, corrected horsepower. Sweeping for them finds
nothing. Some of those calculations are patent-protected; do not reimplement them.

---

# 6. Validate alarms against recorded data before shipping them

This one caused two wrong implementations in a row.

An oil-pressure low alarm was gated on "engine running," inferred from fuel flow. It
looked right, passed host unit tests, and was **wrong**: fuel rate was on a slow polling
tier and went stale, so shutdown transitions and key-on windows produced **11 false
alarms** when replayed against real drive logs. The replacement — arm only after 20 s of
sustained *fresh* RPM ≥ 400 — produced **zero** false alarms over the same 14,244 rows.

**Build the log replay before you tune thresholds.** Alarm logic that passes unit tests
can still fail on real transitions, and transitions are where alarms actually fire. A
tool that replays your alarm logic over recorded CSV is the cheapest bug-finder in the
project.

Related: thresholds should come from the vehicle's own recorded distribution, not from
forum consensus. Two real nuisance alarms were found this way — a voltage threshold that
tripped on every cold crank, and a DPF pressure threshold that tripped on a hard highway
pull because differential pressure scales with flow squared, not with soot.

---

# 7. Firmware traps that are not vehicle-specific

Relevant if you touch the display firmware rather than only the profile.

- **Cross-core `millis()` comparison underflowed.** One core captured `now`, another
  stamped timestamps a few milliseconds later; unsigned subtraction wrapped to ~4.29 e9
  and marked fresh data stale, producing a flicker that survived two wrong fixes. Use
  **signed** deltas: `(int32_t)(now - then) < limit`. This is also wrap-safe at 49 days.
- **LVGL's `lv_snprintf` is built without `%f`** and silently renders `f`. Use standard
  `snprintf` in chart callbacks.
- **Blocking connect on a core that also polls input** freezes the UI during reconnect.
- **Snapshot every UI state that moves.** A grid-overflow bug shipped because every host
  render happened to have the cursor on a top cell.

---

# 8. Safety — non-negotiable when the car is not yours

The scan tool is **read-only by construction**: only OBD read services (`01`, `03`, `09`,
`22`) plus an AT-command allowlist can be transmitted. Write and control services —
`2E` write-by-identifier, `31` routine control, `2F` I/O control, `11` ECU reset, `14`
clear DTCs, `27` security access, `10` session control — are **rejected in code**, and the
catalog is validated at startup so a malformed range fails before the session begins
rather than mid-sweep.

Keep it that way. A mistyped hex digit must not be able to become a write or start a
routine on somebody else's vehicle.

## 8.1 Validate the exact bytes you send, not a tidied-up copy

An adversarial review caught this in our own whitelist, and it is the subtlest bug in the
project so far.

Both validators normalised their input before checking it — `"".join(s.split()).upper()`
— which strips **all** whitespace, including carriage returns. But the caller transmits
the **original** string, and an ELM327 treats `\r` as a *command terminator*. So:

| Input | Validator saw | Adapter would have received |
| :--- | :--- | :--- |
| `"0100\r2EF190AA"` | one mode-`01` read → **accepted** | `0100` **and** `2EF190AA` — a write |
| `"ATSH 7E0\rATPP 0C SV 01"` | an allowed `ATSH` prefix → **accepted** | also `ATPP 0C SV 01` — a *persistent* parameter write |

The validator and the transmitter were looking at two different strings. Nothing
user-controlled reached it at the time, so it was latent — but a whitelist that depends
on callers being well-behaved is not a whitelist.

**The general rule: whatever you validate must be byte-identical to what you send.**
Reject control characters on the raw input before any normalisation, and anchor
prefix-matched allowlists so an approved prefix cannot carry an arbitrary suffix.

**Two things to tell an owner before you plug in**, both true: the tool only ever reads,
and nothing is installed or modified.

⚠️ **If your vehicle needs a non-default diagnostic session to expose enhanced data**
(some manufacturers do), that requires service `0x10` — which this tool deliberately will
not send. Do not quietly add it to make a read work. Raise it as a design decision,
because it changes the tool from a reader into something that changes ECU state.

---

# 9. Practical session plan

**Parked, ~1 hour:**
1. Census both addressing modes. Record which headers answer and how (positive vs
   negative). If nothing answers on 11-bit, try 29-bit before concluding anything.
2. Walk the Mode-01 supported-PID bitmaps — seven probes buys the exact generic PID list
   per module, free.
3. Sweep candidate blocks. Log everything, including negatives.
4. **Sample with the engine off** for floor detection.

**Driving, one normal trip:**
5. Log all hits plus anchors at ~1 Hz. Vary load — hills, acceleration, steady cruise.
   Constant-speed-only data makes half the parameters look identical.
6. **Include a cold start** if at all possible.

**At the desk:**
7. Correlate, then design a discriminating experiment for each promising candidate.
8. Only then write the vehicle profile — from measured data, with provenance comments
   recording *how* each PID was confirmed.

---

# 10. Expectation setting

- Some parameters are **simply not exposed**. On the GM truck, DPF soot load, regen
  status and turbo vane position were swept for repeatedly and never found. Absence of
  evidence became evidence of absence only after sweeping both addressing modes and every
  plausible module.
- A first profile reaching parity with an existing one is a scan session, a drive, and a
  few evenings of decode work — *if* the data is on the bus your adapter can reach.
- If a whole category is missing, suspect a **second bus** before suspecting the vehicle.
  A single-bus adapter cannot see everything on every platform.
- Write down what you *didn't* find. It saves the next person from re-sweeping it.
