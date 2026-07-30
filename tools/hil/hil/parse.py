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


def parse_banner(native_log: str) -> Banner | None:
    """Merge every `[BOOT]` line's key=value pairs into one Banner.

    Returns None when no banner was captured, which is a genuinely different
    outcome from a banner that disagrees with expectations — callers must
    distinguish "no evidence" (SKIP) from "wrong" (FAIL).
    """
    fields: dict[str, str] = {}
    for line in native_log.splitlines():
        if line.startswith("[BOOT]"):
            fields.update(_KV.findall(line))
    if not fields:
        return None

    def mb(key: str) -> int:
        # Values arrive as "8MB"; strip the unit rather than assuming a width.
        return int(fields.get(key, "0").removesuffix("MB") or 0)

    return Banner(
        env=fields.get("env", ""),
        version=fields.get("ver", ""),
        git=fields.get("git", ""),
        profile=fields.get("profile", ""),
        psram_mb=mb("psram"),
        flash_mb=mb("flash"),
        reset=int(fields.get("reset", "-1")),
        heap=int(fields.get("heap", "0")),
    )


# --- UART bridge checks ----------------------------------------------------

_ROM_BOOT = re.compile(r"boot:0x[0-9a-f]+ \(([^)]+)\)")


def check_boot_mode(uart_log: str) -> Verdict:
    """The ROM must report a flash boot, not download mode.

    Absent evidence is SKIP: an empty capture proves nothing, and reporting it
    as a pass is how a rig silently stops testing.
    """
    m = _ROM_BOOT.search(uart_log)
    if not m:
        return Verdict("boot mode", Status.SKIP, "no ROM boot line captured")
    mode = m.group(1)
    if "SPI_FAST_FLASH_BOOT" in mode or "SPI_FLASH_BOOT" in mode:
        return Verdict("boot mode", Status.PASS, mode)
    return Verdict("boot mode", Status.FAIL, f"booted as {mode}")


_PANIC_MARKERS = ("Guru Meditation", "Backtrace:", "StoreProhibited", "LoadProhibited")


def check_no_panic(uart_log: str) -> Verdict:
    hits = [m for m in _PANIC_MARKERS if m in uart_log]
    if hits:
        return Verdict("no panic", Status.FAIL, f"found {', '.join(hits)}")
    return Verdict("no panic", Status.PASS)


# The v0.1.1 signature. disableCore0WDT() on IDF 5 unsubscribes IDLE0 but leaves
# the TWDT idle hook installed; the hook then logs this on every idle tick,
# saturating the 115200 UART and pushing OBD replies past their 400 ms window.
_WDT_SPAM = re.compile(r"task_wdt:.*(?:task not found|esp_task_wdt_reset)")


def check_no_wdt_spam(uart_log: str) -> Verdict:
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


def check_theme_ack(native_log: str) -> Verdict:
    if "[THEME]" in native_log:
        return Verdict("theme ack ('n')", Status.PASS)
    return Verdict("theme ack ('n')", Status.FAIL, "no [THEME] line within the window")


def check_mock_alarm_ack(native_log: str) -> Verdict:
    if "[MOCK]" in native_log:
        return Verdict("mock alarm ack ('s')", Status.PASS)
    return Verdict("mock alarm ack ('s')", Status.FAIL, "no [MOCK] line within the window")


def check_ble_scanning(native_log: str) -> Verdict:
    """ble_obd_source.cpp:309 prints "[BLE] scanning 6s …" with a UTF-8 ellipsis
    (U+2026), not three ASCII dots. Match the prefix and treat the rest as
    opaque, or this check silently never fires.
    """
    if "[BLE] scanning" in native_log:
        return Verdict("BLE scanning", Status.PASS)
    return Verdict("BLE scanning", Status.FAIL,
                   "state machine never reported a scan")
