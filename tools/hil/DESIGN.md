# Hardware-in-the-loop test rig — Phase 1 design

**Status:** implemented 2026-07-29 — `tools/hil/` ships the rig, it has been run end to end
against the real board, and CI runs its pure tests on every push.
**Scope:** Phase 1 only — USB-only, no BLE peer. Phases 2 and 3 are out of scope here.

## Why this exists

Two consecutive releases shipped broken to the vehicle on 2026-07-26:

| Release | Failure | Root cause |
| :------ | :------ | :--------- |
| v0.1.0 | could not connect to the adapter at all | NimBLE 2.x changed `setConnectTimeout` from seconds to milliseconds, leaving a 4 ms connect budget (PR #13) |
| v0.1.1 | linked, but every gauge stayed blank | IDF 5 `disableCore0WDT()` leaves the TWDT idle hook installed; the resulting error log on every idle tick saturated the UART and pushed OBD replies past their 400 ms window (PR #17) |

Both passed every gate the project has: host tests, fuzzing, device build, CodeQL, Scorecard.
Neither is visible without real hardware. This rig closes that gap. It is not "more tests" — it
is a different *class* of test.

## What Phase 1 can and cannot catch

`docs/OBD-BACKLOG.md` §12 (private) originally recorded that Phase 1 "would not have caught
either field failure." **That is half wrong, and the correction is the main reason Phase 1 is worth building
now rather than waiting for Phase 2.**

- **v0.1.1 is catchable in Phase 1.** The symptom was `E (nnnn) task_wdt:
  esp_task_wdt_reset(705): task not found` repeating at idle-tick rate. That is an IDF `ESP_LOG`
  write to UART0, and it is emitted whether or not any BLE peer exists. Detecting a regression is
  a grep on the UART bridge. The failure mode is documented in `src/main.cpp:136-146`.
- **v0.1.0 is not catchable in Phase 1.** With no peer to connect to, "connect failed because the
  timeout is 4 ms" is indistinguishable from "connect failed because nothing is there." That
  needs the Phase 2 fake adapter.

So Phase 1 covers one of the two field failures, plus the boot-integrity class (v1.3.0's
`obd_core0` stack-overflow boot loop) that no host test can see.

## Hardware, as measured

Both USB-C ports on the CrowPanel Advance 3.5 are passed through to the VM. Established
empirically on 2026-07-29, not assumed:

| Node | VID:PID | What it is | Behaviour on reset |
| :--- | :------ | :--------- | :----------------- |
| `/dev/ttyACM0` | `303a:1001` | ESP32-S3 native USB (USB-Serial/JTAG) | **re-enumerates** — node disappears and returns |
| `/dev/ttyUSB0` | `1a86:7522` | CH340 bridge on UART0 | **stays enumerated** |

Board identity, from `esptool.py flash_id`: ESP32-S3 (QFN56) rev v0.2, 8 MB embedded PSRAM
(AP_3v3), 16 MB flash, quad-line flash set in eFuse, 3.3 V flash voltage.

### The port split is not what §12 assumed

§12 hypothesised "monitor on the bridge, flash on native, and the monitor never drops mid-test."
The first half holds; the conclusion does not. **Both `crowpanel` and `crowpanel_obd` set
`-D ARDUINO_USB_CDC_ON_BOOT=1`** (`platformio.ini:26,61`), which routes the Arduino `Serial`
object to the native USB CDC. Consequences:

- The **UART bridge** carries the ROM bootloader log, the second-stage loader, IDF `ESP_LOG`
  output, and panic/backtrace output. It does **not** carry the application's `Serial.printf`
  lines.
- The **native USB** port carries the application log — `[BOOT]`, `[BLE]`, `[OTA]`, `[THEME]` and
  the rest — but drops out on every reset.

**Therefore the harness reads both ports.** Neither alone is sufficient. Log routing is
deliberately left alone: mirroring the application log to UART0 as well would double UART0
traffic, and a saturated 115200 UART starving OBD replies is precisely how v0.1.1 failed.

### Two constraints learned the hard way

1. **Never hold the native port open while `esptool` or `pio` is using it.** A first capture
   attempt with a reader contending against `esptool` on the same node returned 26 bytes of
   protocol garbage instead of a log. The runner owns that port exclusively: release, flash or
   reset, then reopen.
2. **Discover ports by VID:PID, never by a hardcoded path or `by-id` string.** `ttyACMn`
   numbering shifts across re-enumeration (`docs/INSTALL.md:57` already warns against globbing
   the port), and this board's `by-id` string embeds its MAC address, which does not belong in a
   public repository. `HIL_PORT_NATIVE` and `HIL_PORT_UART` override discovery for a second board.

## The one firmware change: a boot banner

`setup()` (`src/main.cpp:95-170`) prints **nothing on the success path** — only `[PROFILE]` when
an NVS key fails to resolve and `[WDT]` when the watchdog reconfigure fails. A harness can
therefore only infer success from the absence of a crash, and cannot tell which build is actually
on the board.

Phase 1 adds one `Serial.printf` at the end of `setup()`:

```
[BOOT] crowpanel_obd v0.1.3 (e1626f9) profile=gm_sierra_lz0
[BOOT] psram=8MB flash=16MB reset=0xa heap=241344
```

(The example shows a tagged release. A bench `pio run` prints `local (local)` in those two fields
— see *Version assertion* under Deliberate non-goals.)

Fields: `OTA_ENV`, `FW_VERSION`, `FW_GIT`, active profile key, PSRAM and flash size, reset reason,
free heap. This ships in the release firmware — it is equally useful in the field, where "which
build is this and why did it restart" is the first question. **It must never include the VIN:**
that would be a real PII leak in a field log, and `check_no_pii.py` would reject it.

This landed as its own PR (#26), ahead of the harness, so the harness had something to assert
against.

## Layout

```
tools/hil/
  hil/parse.py       PURE. log lines -> structured verdicts. no serial, no subprocess.
  hil/ports.py       VID:PID discovery -> resolved port paths.
  hil/runner.py      device layer: pio flash, esptool reset, dual capture, key injection
  hil/__main__.py    CLI: sequences a run, prints the verdict table, writes artifacts
  tests/             pytest over parse.py and ports.py using recorded golden logs, plus the
                     CLI's unwind/exit-code guarantees driven with fakes (no board, no pyserial)
  DESIGN.md          this document
  README.md          what it is, what it cannot do, how to run it
```

The split mirrors how the firmware is already organised: pure logic that is tested exhaustively,
over a thin hardware layer that is not. `parse.py` and `ports.py` have no I/O and run in CI
without a board. `runner.py` touches hardware and only runs on a machine with the board attached.

Python, with `pyserial` (3.5 or newer). Invoked as `python3 -m hil --env crowpanel`.

## Run sequence

Per environment — `crowpanel` (mock data) then `crowpanel_obd` (real, BLE):

```
1. open the UART bridge, hold it open for the entire run
2. release the native port -> pio run -e <env> -t upload --upload-port <native>
3. reopen the native port in a retry loop, tolerating ENODEV for ~2 s while it re-enumerates
4. capture both ports for the boot window (default 15 s)
5. inject keys, capturing each response window (5 s each)
6. soak (default 300 s) with an active liveness probe
7. write artifacts, print the verdict table
```

Step 1 precedes step 2 deliberately. If the native reopen in step 3 loses the race, the UART
capture still holds the boot reason and any panic output, so boot evidence is never lost.

## Checks

| # | Check | Port | Catches | Precedent |
| :- | :---- | :--- | :------ | :-------- |
| 1 | `pio upload` exits 0 | — | build or toolchain rot | the pioarduino platform-resolution trap, `platformio.ini:8-11` |
| 2 | ROM reports `SPI_FAST_FLASH_BOOT` | UART | bad flash; stuck in download mode | — |
| 3 | no `Guru Meditation`, no `Backtrace:`, no panic reset code | UART | boot-loop crash | v1.3.0 `obd_core0` stack overflow |
| 4a | **zero** `esp_task_wdt_reset` / `task not found` lines | UART | UART-saturating IDF log spam | **v0.1.1, replayed exactly** |
| 4b | no `[WDT] reconfigure failed`, no `[WDT] OBD task stalled` | native | the watchdog reconfigure silently failing; the 240 s stall restart firing | `main.cpp:157,207` |
| 5 | `[BOOT]` present and its `env` matches the env just flashed | native | flashing the wrong environment | `default_envs = crowpanel_obd` makes this an easy mistake |
| 6 | `[encoder] found\|MISSING` matches `--expect-knob` | native | I²C or knob wiring regression | `encoder_input.cpp:47`; v1.3.1 invisible-menu class |
| 7 | `'n'` is acknowledged by a `[THEME]` line **somewhere in the run's capture** | native | a console that stopped answering; LVGL repaint hang | latency is asserted by check 10, not here |
| 8 | `'s'` is acknowledged by **any** `[MOCK]` line, likewise anywhere in the capture, then no panic | native | full-screen alarm-overlay render crash | mock environment only; the toggle's direction depends on prior state, so either `[MOCK] alarm sweep` or `[MOCK] safe bands` counts |
| 9 | `crowpanel_obd`: `[BLE] scanning` appears **at least once** | native | a state machine that never starts searching at all. It does **not** catch one that scanned once and then wedged — a single occurrence anywhere in the boot capture passes | `ble_obd_source.cpp:309`; the Phase 2 boundary |
| 10 | soak: the `'n'` probe answers every 30 s for 300 s | native | silent late hang | v1.3.0 boot loop; the 240 s heartbeat path, `main.cpp:205-210` |

Check 10 is an **active** probe rather than passive silence, because silence cannot distinguish
"healthy and idle" from "wedged". It requires no firmware change.

**Checks 7 and 8 assert acknowledgement, not latency** — deliberately, and this was learned the
hard way. They were originally scoped to a 2 s reply window, and check 8 then FAILed on 1 run in
3 (measured 2026-07-29) with `[MOCK] alarm sweep` *present in the capture* but a beat past the
window: `'n'` triggers a full-screen repaint of the 480x320 ILI9488 over SPI plus a backlight
change, and the `'s'` key is not even read until `loop()` finishes that work. A gate that fails
one run in three teaches people to re-run until green, at which point it protects nothing. The
windows are now 5 s and both checks are evaluated over the whole capture (boot + probes + soak).
No rigour is lost: each key is sent exactly once per run, so any matching line is that one ack;
and response *latency* is still asserted by check 10, which requires every 30 s probe to answer
inside its own step. An empty capture still FAILs both — a key was written and nothing came
back, which is positive evidence of a wedged console rather than absent evidence.

### CLI surface

```
python3 -m hil --env {crowpanel,crowpanel_obd,both}   default: both
               --expect-knob {yes,no,auto}            default: auto
               --soak SECONDS                         default: 300
               --boot-window SECONDS                  default: 15
               --allow-skips                          default: off
```

`--expect-knob auto` reports whatever `[encoder]` says without failing on it, because whether a
Modulino knob is attached to the bench board is a property of the bench, not of the firmware.
`yes` and `no` turn it into an assertion for a rig whose wiring is known and fixed.

### One parser pitfall, recorded so it is not rediscovered

`ble_obd_source.cpp:309` reads `Serial.println("[BLE] scanning 6s …")` — that is a **UTF-8
ellipsis (U+2026), not three ASCII dots.** Matching on `"scanning 6s ..."` will silently never
fire. Match on the `[BLE] scanning` prefix and treat the remainder as opaque. The same caution
applies to any other log line with typographic punctuation.

### Why UI navigation is not driven

§12 wanted Phase 1 to drive "the whole UI, alarm model, menu, RTC and theme." Phase 1 does not,
for two reasons:

- The knob is a Modulino encoder on the board's I²C bus. The host cannot turn it. Detecting its
  presence is possible; actuating it is not.
- Adding navigation keys to the serial console would reopen F-11. That console is
  unauthenticated, and serial access to the settings menu means serial access to **Forget
  adapter**, which wipes the stored BLE bond and reopens the trust-on-first-use window — the one
  thing on that console with a real security consequence (`src/main.cpp:213-235`).

So Phase 1 asserts against exactly the surface the shipped binary already exposes: `'n'` and, in
the mock build, `'s'`. Navigation *logic* is already covered by pure host suites
(`test_nav_model`, `test_encoder_logic`, `test_layout`). A `-D HIL_INPUT=1` build that injects
encoder events over serial remains available as a follow-up if on-device UI bugs actually prove
to be escaping — but it would test a binary that differs from the shipped one in its input path,
so it needs evidence to justify, not speculation.

## Deliberate non-goals

- **Heap-trend leak detection.** The firmware emits no periodic heap line — heap appears only in
  the OTA path (`ota_update.cpp:115,175,178`). Detecting a slow runtime leak would need more
  firmware surface than the banner. What Phase 1 gets cheaply instead: the banner's boot heap is
  recorded in `verdict.json` for a human to read. There is no baseline comparison and
  no `--update-baseline` flag — a single boot reading could only ever catch static-allocation
  bloat, never a runtime leak, so an automated pass/warn verdict on it would be asserting more
  than the evidence supports. Automated heap-regression detection against a committed baseline is
  recorded as a follow-up, not implemented in Phase 1.
- **Version assertion.** A local `pio run` stamps `FW_VERSION "local"` (`src/fw_git.h:15-16`);
  only `release.yml` sed-stamps a real tag. The harness therefore asserts the *environment* and
  merely reports the *version*. A non-`local` version on a bench build is flagged, not failed.
- **Anything requiring a BLE peer.** That is Phase 2 in `docs/OBD-BACKLOG.md` §12 (private).

## Error handling

Exit codes distinguish a firmware problem from a broken rig:

| Code | Meaning |
| :--- | :------ |
| 0 | every check ran and passed |
| 1 | a check failed — firmware problem |
| 2 | rig or environment error — no board, port busy, permission denied, flash failed |
| 3 | passed, but checks were skipped (unless `--allow-skips`) |

Code 3 exists deliberately. A run in which the native port never returned would otherwise report
"all assertions passed" while silently testing a fraction of what it claims. Skipped checks are
reported loudly and are non-zero by default.

Specific degradations:

- **No board present** → exit 2 immediately. A HIL run that passes with no hardware attached is
  worse than having no rig.
- **`EACCES` on a port** → report the `dialout` group or `chmod` fix rather than a traceback.
  This is the first thing that goes wrong on a fresh machine.
- **Flash failure** → capture `pio` stdout and stderr to artifacts, exit 2, and **do not run the
  assertions**. Asserting against a stale binary produces confident nonsense.
- **Native reopen times out** → degrade rather than abort. Continue with the UART-only checks and
  mark the native-only checks `SKIPPED`, never `PASSED`.
- **The UART tap dies mid-run** (cable out, hub reset, port stolen) → the three UART-derived checks
  are downgraded `PASS` → `SKIP`, so the run cannot exit 0. This is not cosmetic: `UartTap.text`
  retains everything captured before the tap died, and every UART check's pass condition is
  satisfied by that retained text — a cable pulled 50 s into a 300 s soak would otherwise report
  "all checks green" having observed a sixth of its window. An *observed* `FAIL` is kept as a FAIL;
  a panic that was actually seen is real evidence regardless of what happened to the cable next.
- **`--soak 0`** → the soak check reports `SKIP`, never `PASS`. The loop runs zero probes, and a
  check that never ran must not be green. Not clamped to a minimum: the operator asked for no soak
  and is told they got none (exit 3).
- **Any unhandled exception, anywhere** → exit 2 with a traceback, never exit 1. Python's default
  exit code for an uncaught exception is 1, the code reserved for "the firmware is broken", so both
  the `pyserial` import and `main()` itself are wrapped. A missing `pyserial` — a documented
  prerequisite — names its own fix and exits 2.
- Every read is bounded. A `finally` block always closes both ports and always writes artifacts,
  including on `SIGINT`. The UART is drained on every soak step, not only at the window ends, so a
  300 s soak's output never has to survive in the OS tty buffer.
- A rig error in the second environment still renders the first environment's verdict table before
  returning 2. Exit-2 precedence is right; hiding a genuine `FAIL` the run already found is not.

## Artifacts

Every run writes `tools/hil/runs/<iso-timestamp>-<env>/` containing `uart.log`, `native.log`,
`pio.log` and `verdict.json`. Gitignored. *Every* run means the rig-error and `SIGINT` paths too,
with whatever partial evidence exists — a rig error is exactly when someone wants the record. A
`pio` run killed at its 600 s timeout puts its partial output in `pio.log`, which on that path is
the only evidence of what wedged.

This is structural, not incidental. The v0.1.x arc produced the rule *on a field-only failure,
get the serial log before shipping a fix* — three wrong theories cost three trips to the vehicle.
A rig that discarded its logs would reproduce that mistake on the bench.

## Testing the harness

`parse.py` is the part that can regress silently, so it carries the test weight — and the
**negative** fixtures matter most. A parser with no failing fixtures is a parser that always
passes.

| Golden log fixture | Required verdict |
| :----------------- | :--------------- |
| clean healthy boot | PASS |
| `task_wdt: … task not found` spam | **FAIL check 4** — v0.1.1 replayed |
| `Guru Meditation` plus backtrace | **FAIL check 3** |
| real download-mode capture (recorded 2026-07-29) | FAIL check 2 |
| the 26-byte contended-garbage capture (recorded 2026-07-29) | inconclusive / SKIPPED, **not** PASS, and no exception |
| `[BOOT] env=crowpanel` when `crowpanel_obd` was flashed | **FAIL check 5** |
| banner line 1 only (line 2 lost to the reopen race) | SKIPPED, **not** PASS — a missing number is as unusable as a garbled one, and `heap=0` in `verdict.json` would be a fabricated measurement |

The exit-code mapping is a pure function (`parse.exit_code`) with its own tests, for the same
reason: miscounting a SKIP as a PASS would silently convert every exit 3 into an exit 0, which is
the worst thing this rig could do.

`ports.py` is tested against faked `list_ports` data. `ci.yml`'s existing test job gains one step,
`cd tools/hil && python -m pytest tests -q`; no board is required. The CLI's own
unwind guarantees are covered in `tests/test_cli.py`, which stubs `sys.modules["serial"]` with
`unittest.mock` so that step still needs neither a board nor `pyserial`.

Golden logs are swept for the board's MAC address and for VIN-shaped strings before being
committed; the current fixtures needed no edits, so `tests/logs/*.log` are verbatim captures. Any
VIN that ever does appear in a fixture must use the repository's fake-VIN convention
(`3GT0123456789ABCD`). `check_no_pii.py` already gates this in CI.

## Follow-ups, not in Phase 1

- Periodic heap line, enabling real leak detection during soak.
- `-D HIL_INPUT=1` encoder-event injection, if on-device UI bugs prove to be escaping.
- Phase 2: the fake BLE adapter (Ircama ELM327-emulator behind a BlueZ GATT shim). This is where
  v0.1.0-class failures become detectable. See `docs/OBD-BACKLOG.md` §12 (private).
- Wiring the rig into the PR template's manual on-hardware gate, once it has earned that trust.
