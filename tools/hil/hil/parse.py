"""Pure verdict logic for the HIL rig.

No serial, no subprocess, no filesystem. Everything here takes captured text and
returns Verdicts, which is what makes it testable in CI with no board attached.

Port routing matters when reading this module. On this board
ARDUINO_USB_CDC_ON_BOOT=1, so:
  * UART bridge (CH340)  -> ROM bootloader log, IDF ESP_LOG, panic/backtrace
  * native USB (CDC)     -> the application's own Serial.printf lines
Each check below documents which stream it expects.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from enum import Enum


class Status(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass(frozen=True)
class Verdict:
    check: str
    status: Status
    detail: str = ""


@dataclass(frozen=True)
class Banner:
    env: str
    version: str
    git: str
    profile: str
    psram_mb: int
    flash_mb: int
    reset: int
    heap: int


_KV = re.compile(r"(\w+)=([^\s]+)")


def _num(raw: str | None, default: int | None, unit: str = "") -> int | None:
    """Parse a numeric banner field. None means UNUSABLE — missing or garbled.

    A corrupted field returns None so the caller can treat the whole banner as
    unusable. A missing field falls back to `default`, and every caller in
    `parse_banner` passes `default=None` on purpose: a field that never arrived
    is exactly as unusable as one that arrived garbled, and substituting a
    plausible-looking number (heap=0, psram=0MB) would record a fabricated
    measurement in `verdict.json` as if it had been observed. Absent evidence
    is not evidence.
    """
    if raw is None:
        return default
    if unit:
        raw = raw.removesuffix(unit)
    try:
        return int(raw)
    except ValueError:
        return None


def parse_banner(native_log: str) -> Banner | None:
    """Merge every `[BOOT]` line's key=value pairs into one Banner.

    Returns None when no banner was captured, which is a genuinely different
    outcome from a banner that disagrees with expectations — callers must
    distinguish "no evidence" (SKIP) from "wrong" (FAIL). A banner whose numeric
    fields are garbled (a reader contending with esptool truncates mid-line) is
    treated the same way: unusable, so callers SKIP rather than trust a
    plausible-looking but wrong number. So is a banner whose numeric fields are
    *missing*: the banner is two lines from one Serial.println and
    `capture_native` reopens the port ~1 s after reset, so a capture can land
    line 1 and lose line 2 entirely. That yields env/ver/git but no
    psram/flash/reset/heap, and a Banner reporting heap=0 would be a fabricated
    measurement, not a captured one.
    """
    fields: dict[str, str] = {}
    for line in native_log.splitlines():
        if line.startswith("[BOOT]"):
            fields.update(_KV.findall(line))
    if not fields:
        return None

    # `default=None` on all four: missing is as unusable as garbled. `reset` is
    # required alongside the other three deliberately, and requiring it costs
    # nothing — all four are printed by the SAME Serial.println (banner line 2),
    # so a capture missing `reset` is already missing psram/flash/heap and would
    # be rejected anyway. Requiring it buys one rule instead of two, and keeps
    # `Banner.reset` from ever holding a sentinel that a future reset-reason
    # check would read back as real data.
    psram = _num(fields.get("psram"), None, "MB")
    flash = _num(fields.get("flash"), None, "MB")
    reset = _num(fields.get("reset"), None)
    heap = _num(fields.get("heap"), None)

    # Any missing or garbled numeric field makes the whole banner unusable.
    # Returning None routes the caller to SKIP ("no usable banner") rather than
    # handing back a Banner with a silently wrong number in it — a
    # plausible-looking lie is worse than an admitted absence.
    if None in (psram, flash, reset, heap):
        return None

    return Banner(
        env=fields.get("env", ""),
        version=fields.get("ver", ""),
        git=fields.get("git", ""),
        profile=fields.get("profile", ""),
        psram_mb=psram,
        flash_mb=flash,
        reset=reset,
        heap=heap,
    )


# --- UART bridge checks ----------------------------------------------------

# Greedy `.+` (not `[^)]+`), because the real ROM line nests parens:
# "boot:0x2 (DOWNLOAD(USB/UART0))" — measured on hardware 2026-07-29. A
# non-greedy/negated-class capture stops at the FIRST ')', truncating the
# value to "DOWNLOAD(USB/UART0" and losing the closing paren. `.` does not
# cross the line's trailing \r\n, so greedy still lands on the last ')' of
# THIS line, not a later line's.
_ROM_BOOT = re.compile(r"boot:0x[0-9a-f]+ \((.+)\)")


# --- evidence gates ---------------------------------------------------------
#
# A check that scans for negative markers ("no panic", "no spam") is only
# meaningful if we were actually listening. On an empty capture — dead port,
# lost re-enumeration race, cable out — "I found no panic" is not good news, it
# is no news, and reporting it as PASS is how a rig silently stops testing while
# looking green. Every negative-marker check gates on one of these.


def _has_uart_evidence(uart_log: str) -> bool:
    """True when the UART capture proves the port was live and the board booted.

    The ROM bootloader line is emitted on every boot before any application code
    runs, so its absence means we captured nothing at all.
    """
    return _ROM_BOOT.search(uart_log) is not None


def _has_native_evidence(native_log: str) -> bool:
    """True when the native capture proves the application actually ran.

    setup() prints the [BOOT] banner unconditionally at its end, so no banner
    means no usable application log — the CDC port never came back, or the
    firmware died before reaching the end of setup().
    """
    return "[BOOT]" in native_log


def check_boot_mode(uart_log: str) -> Verdict:
    """The ROM must report a flash boot, not download mode.

    Absent evidence is SKIP: an empty capture proves nothing, and reporting it
    as a pass is how a rig silently stops testing.

    Takes the LAST ROM boot line, not the first — measured on hardware
    2026-07-29. `runner.py` opens the UART tap BEFORE flashing and holds it
    for the whole run, so every real capture starts with `pio`/`esptool`
    resetting the board INTO download mode to write the new image. That line
    is always present and is not a failure; the boot state that actually
    matters is whatever the board most recently reset into (the post-flash
    boot, or a later in-run reset if one happened during the soak).

    Note what this does NOT detect. The ROM `boot:0x…` field reports the SPI
    strapping boot mode, not the reset cause, so a board that reset from a
    watchdog or a panic still comes back up as SPI_FAST_FLASH_BOOT. A later
    reset therefore changes nothing here; panic and watchdog detection live in
    `check_no_panic` and `check_no_wdt_spam`, which read the reset's own
    output.
    """
    matches = list(_ROM_BOOT.finditer(uart_log))
    if not matches:
        return Verdict("boot mode", Status.SKIP, "no ROM boot line captured")
    mode = matches[-1].group(1)
    if "SPI_FAST_FLASH_BOOT" in mode or "SPI_FLASH_BOOT" in mode:
        return Verdict("boot mode", Status.PASS, mode)
    return Verdict("boot mode", Status.FAIL, f"booted as {mode}")


_PANIC_MARKERS = ("Guru Meditation", "Backtrace:", "StoreProhibited", "LoadProhibited")


def check_no_panic(uart_log: str) -> Verdict:
    if not _has_uart_evidence(uart_log):
        return Verdict("no panic", Status.SKIP, "no UART evidence captured")
    hits = [m for m in _PANIC_MARKERS if m in uart_log]
    if hits:
        return Verdict("no panic", Status.FAIL, f"found {', '.join(hits)}")
    return Verdict("no panic", Status.PASS)


# The v0.1.1 signature. disableCore0WDT() on IDF 5 unsubscribes IDLE0 but leaves
# the TWDT idle hook installed; the hook then logs this on every idle tick,
# saturating the 115200 UART and pushing OBD replies past their 400 ms window.
_WDT_SPAM = re.compile(r"task_wdt:.*(?:task not found|esp_task_wdt_reset)")


def check_no_wdt_spam(uart_log: str) -> Verdict:
    if not _has_uart_evidence(uart_log):
        return Verdict("no task_wdt spam", Status.SKIP, "no UART evidence captured")
    n = len(_WDT_SPAM.findall(uart_log))
    if n:
        return Verdict("no task_wdt spam", Status.FAIL,
                       f"{n} task_wdt error lines — the v0.1.1 regression")
    return Verdict("no task_wdt spam", Status.PASS)


# --- native USB checks -----------------------------------------------------

def check_app_wdt_clean(native_log: str) -> Verdict:
    """The firmware's own [WDT] lines both mean something went wrong.

    main.cpp:157 logs a failed TWDT reconfigure; main.cpp:207 logs the 240 s
    OBD-stall restart. Neither should appear in a healthy bench run.
    """
    if not _has_native_evidence(native_log):
        return Verdict("app watchdog clean", Status.SKIP, "no native evidence captured")
    for marker in ("[WDT] reconfigure failed", "[WDT] OBD task stalled"):
        if marker in native_log:
            return Verdict("app watchdog clean", Status.FAIL, marker)
    return Verdict("app watchdog clean", Status.PASS)


def check_banner_env(native_log: str, expected_env: str) -> Verdict:
    b = parse_banner(native_log)
    if b is None:
        return Verdict("banner env", Status.SKIP, "no [BOOT] banner captured")
    if b.env == expected_env:
        return Verdict("banner env", Status.PASS, b.env)
    return Verdict("banner env", Status.FAIL,
                   f"flashed {expected_env} but the board reports {b.env}")


def check_encoder(native_log: str, expect: str) -> Verdict:
    """`expect` is "yes", "no" or "auto".

    "auto" reports without asserting, because whether a Modulino knob is plugged
    into the bench board is a property of the bench, not of the firmware.
    """
    if "[encoder] found" in native_log:
        found = True
    elif "[encoder] MISSING" in native_log:
        found = False
    else:
        return Verdict("encoder", Status.SKIP, "no [encoder] line captured")

    state = "found" if found else "MISSING"
    if expect == "auto":
        return Verdict("encoder", Status.PASS, f"{state} (not asserted)")
    want = expect == "yes"
    if found == want:
        return Verdict("encoder", Status.PASS, state)
    return Verdict("encoder", Status.FAIL, f"expected {expect}, got {state}")


# The two ack checks below FAIL rather than SKIP on empty input, which looks
# inconsistent with `_has_native_evidence` above — it is deliberate, so please
# do not "fix" it. The negative-marker checks gate on evidence because "I saw no
# panic" is meaningless if nobody was listening. These two are the opposite
# shape: `__main__` only calls them after a key was actually written to a live
# port, so a capture with no ack in it is not absence of observation, it is a
# positive observation that the firmware did not answer — exactly the
# LVGL-repaint-hang class they exist to catch. (The "was the port live at all"
# question is already answered upstream: `_run_env` skips both checks and emits
# an explicit "console probes" SKIP when the boot capture came back empty.)
#
# Both take the WHOLE native capture (boot + probes + soak), not one probe
# window. They assert that the console answered and the render did not kill the
# firmware — not that it answered inside a timer. See the call site in
# `__main__._run_env` for the detail, but in short: check_mock_alarm_ack ('s')
# costs no rigour from the wider scope because 's' is sent exactly once per
# run, so any [MOCK] line is provably that one ack. check_theme_ack ('n') is
# different — the soak re-sends 'n' every step — so check 7 is a floor that
# the soak's own liveness check (check 10) subsumes, not an independently
# timed assertion. It is kept because it still catches a console that never
# answers at all, including `--soak 0` runs where check 10 does not run.
def check_theme_ack(native_log: str) -> Verdict:
    """Did the `'n'` key ever get acknowledged? Scope: the whole capture.

    Note: [THEME] has a second emitter in the firmware — main.cpp's per-tick
    theme resolver (~line 432), not just the 'n' key handler (~line 201) — so
    a PASS here is not strictly proof the key handler ran, only that the
    firmware printed [THEME] somewhere after being asked. Pre-existing, not a
    bug; recorded so a PASS is not read as more key-derived than it is.
    """
    if "[THEME]" in native_log:
        return Verdict("theme ack ('n')", Status.PASS)
    return Verdict("theme ack ('n')", Status.FAIL, "no [THEME] line anywhere in the capture")


def check_mock_alarm_ack(native_log: str) -> Verdict:
    """Did the `'s'` key ever get acknowledged? Scope: the whole capture.

    Matches any `[MOCK]` line: the key toggles, so which of `alarm sweep` /
    `safe bands` comes back depends on the state the firmware booted with.
    """
    if "[MOCK]" in native_log:
        return Verdict("mock alarm ack ('s')", Status.PASS)
    return Verdict("mock alarm ack ('s')", Status.FAIL, "no [MOCK] line anywhere in the capture")


def check_ble_link(native_log: str, expect: str) -> Verdict:
    """Did the BLE state machine get anywhere? `expect` is "yes", "no" or "auto".

    This used to assert only on "[BLE] scanning", which INVERTED the logic for
    the better outcome. A board that can actually reach a peer does not scan at
    all: it reconnects straight to the cached address and prints
    "[BLE] connecting cached …" then "[BLE] connected + ELM ready". So once HIL
    Phase 2 gave the rig an adapter to talk to, the check began failing runs in
    which the firmware performed *better* than in the passing case. Scanning is
    the weaker evidence; a completed link is the stronger one.

    ble_obd_source.cpp:309 prints the scan line with a UTF-8 ellipsis (U+2026),
    not three ASCII dots — match the prefix and treat the rest as opaque, or the
    check silently never fires.

    "yes" means a peer (a real adapter, or the Phase 2 emulator) is present, and
    then scanning alone is a genuine failure: it means the board never FOUND the
    peer, which is exactly the regression class the rig exists to catch. "no" and
    "auto" accept either outcome, because whether an adapter is plugged into the
    bench is a property of the bench, not of the firmware.
    """
    scanned = "[BLE] scanning" in native_log
    linked = "[BLE] connected + ELM ready" in native_log

    if expect == "yes":
        if linked:
            return Verdict("BLE link", Status.PASS, "connected + ELM ready")
        if scanned:
            return Verdict("BLE link", Status.FAIL,
                           "scanned but never linked — a peer was expected")
        return Verdict("BLE link", Status.FAIL,
                       "no scan and no connection — peer expected")

    if linked:
        return Verdict("BLE link", Status.PASS, "connected + ELM ready")
    if scanned:
        return Verdict("BLE link", Status.PASS, "scanning; no peer linked")
    return Verdict("BLE link", Status.FAIL,
                   "state machine never scanned or connected")


# --- cross-cutting verdict logic -------------------------------------------

# The checks that read nothing but the UART capture. If the tap dies mid-run,
# their evidence stops at that moment while their PASS conditions stay satisfied
# by the text captured earlier, so they must be downgraded by name.
UART_DERIVED_CHECKS = ("boot mode", "no panic", "no task_wdt spam")


def downgrade_uart_verdicts(verdicts: list[Verdict], reason: str) -> list[Verdict]:
    """Rewrite PASSing UART-derived verdicts to SKIP when the tap died mid-run.

    Why this is necessary: every UART check's PASS condition is satisfied by
    *retained* text. `UartTap.text` keeps everything captured before the tap
    died, so a cable pulled at t+50 s of a 300 s soak still leaves the
    post-flash ROM boot line in the buffer. `check_boot_mode` then PASSes on it,
    and it also satisfies `_has_uart_evidence`, so `check_no_panic` and
    `check_no_wdt_spam` PASS for having seen no markers in the 50 s they
    observed — reporting a green run that watched 17% of its window.

    A FAIL is left alone on purpose. A panic that was actually observed is real
    evidence no matter what happened to the cable afterwards, and demoting it to
    SKIP would discard the single most valuable thing the run found. An existing
    SKIP is left alone too: it already says "no evidence".
    """
    out: list[Verdict] = []
    for v in verdicts:
        if v.check in UART_DERIVED_CHECKS and v.status is Status.PASS:
            detail = f"UART tap died mid-run ({reason}); PASS not trustworthy"
            out.append(Verdict(v.check, Status.SKIP, detail))
        else:
            out.append(v)
    return out


def exit_code(verdicts: list[Verdict], allow_skips: bool = False) -> int:
    """Map a verdict table to a process exit code. Pure, so it is testable.

        1  any FAIL                       -> firmware problem
        3  any SKIP and not allow_skips   -> passed, but tested less than claimed
        0  otherwise

    FAIL outranks SKIP: a run that both failed and skipped is a firmware
    problem, and 1 is the louder, more actionable answer.

    Rig errors (code 2) are deliberately NOT decidable here — they are the
    caller's concern, raised as exceptions before any verdict table exists, and
    they take precedence over everything this function can return.

    An EMPTY verdict list returns 3, and does so even under `allow_skips`. No
    verdicts at all is the most complete form of "no evidence captured", so it
    must never be 0; and `--allow-skips` is a statement that *particular known
    checks* may be absent on this bench, not a licence to accept a run that
    asserted nothing whatsoever. This case is not reachable from `__main__`
    today (every path appends at least one verdict) — it is pinned so that a
    future refactor which can produce an empty table fails loudly instead of
    exiting 0.
    """
    if not verdicts:
        return 3
    if any(v.status is Status.FAIL for v in verdicts):
        return 1
    if not allow_skips and any(v.status is Status.SKIP for v in verdicts):
        return 3
    return 0
