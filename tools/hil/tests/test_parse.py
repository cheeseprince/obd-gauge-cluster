"""Golden-log tests for the pure verdict logic.

The NEGATIVE fixtures carry the weight here. A parser with no failing fixtures
is a parser that always passes, which is the failure mode this rig exists to
avoid.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import pytest

from hil.parse import (
    Status,
    parse_banner,
    check_boot_mode,
    check_no_panic,
    check_no_wdt_spam,
    check_app_wdt_clean,
    check_banner_env,
    check_encoder,
    check_theme_ack,
    check_mock_alarm_ack,
    check_ble_scanning,
)

# --- real captures, recorded from the board on 2026-07-29 -------------------

HEALTHY_UART = """\
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fce3818,len:0x508
entry 0x403c9880
"""

DOWNLOAD_MODE_UART = """\
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x15 (USB_UART_CHIP_RESET),boot:0x2 (DOWNLOAD(USB/UART0))
waiting for download
"""

# The 26-byte truncated capture produced when a reader contends with esptool
# on the native port. Must be inconclusive, never a pass.
CONTENDED_NATIVE = "ESP-ROM:esp32s3-20210327\r\n"

HEALTHY_NATIVE = """\
[BOOT] env=crowpanel_obd ver=local git=local profile=gm_sierra_lz0
[BOOT] psram=8MB flash=16MB reset=1 heap=241344
[encoder] found
[BLE] scanning 6s …
"""

# --- v0.1.1, replayed ------------------------------------------------------

WDT_SPAM_UART = HEALTHY_UART + "".join(
    f"E ({1000 + i}) task_wdt: esp_task_wdt_reset(705): task not found\n"
    for i in range(40)
)

PANIC_UART = HEALTHY_UART + """\
Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.
Backtrace: 0x40081b2a:0x3ffb1f30 0x40087d61:0x3ffb1f50
"""


def test_parse_banner_extracts_every_field():
    b = parse_banner(HEALTHY_NATIVE)
    assert b is not None
    assert b.env == "crowpanel_obd"
    assert b.version == "local"
    assert b.git == "local"
    assert b.profile == "gm_sierra_lz0"
    assert b.psram_mb == 8
    assert b.flash_mb == 16
    assert b.reset == 1
    assert b.heap == 241344


def test_parse_banner_absent_returns_none():
    assert parse_banner(CONTENDED_NATIVE) is None


def test_parse_banner_tolerates_an_unknown_future_field():
    # key=value exists so adding a field cannot break the parser.
    text = HEALTHY_NATIVE.replace("heap=241344", "heap=241344 newthing=7")
    b = parse_banner(text)
    assert b is not None and b.heap == 241344


def test_boot_mode_passes_on_flash_boot():
    assert check_boot_mode(HEALTHY_UART).status is Status.PASS


def test_boot_mode_fails_in_download_mode():
    v = check_boot_mode(DOWNLOAD_MODE_UART)
    assert v.status is Status.FAIL
    assert "DOWNLOAD" in v.detail


def test_boot_mode_skips_when_there_is_no_rom_line():
    # No evidence is not the same as evidence of health.
    assert check_boot_mode("").status is Status.SKIP


def test_no_panic_passes_on_clean_log():
    assert check_no_panic(HEALTHY_UART).status is Status.PASS


def test_no_panic_fails_on_guru_meditation():
    v = check_no_panic(PANIC_UART)
    assert v.status is Status.FAIL
    assert "Guru Meditation" in v.detail


def test_wdt_spam_passes_on_clean_log():
    assert check_no_wdt_spam(HEALTHY_UART).status is Status.PASS


def test_wdt_spam_fails_on_v011_regression():
    # THE v0.1.1 detector. If this ever stops failing, the rig is worthless.
    v = check_no_wdt_spam(WDT_SPAM_UART)
    assert v.status is Status.FAIL
    assert "40" in v.detail  # reports the count, not just "found some"


def test_wdt_spam_fails_on_even_a_single_line():
    # The bug is a leak, not a threshold. One line means the hook is installed.
    v = check_no_wdt_spam(
        HEALTHY_UART + "E (1000) task_wdt: esp_task_wdt_reset(705): task not found\n"
    )
    assert v.status is Status.FAIL


def test_app_wdt_clean_flags_reconfigure_failure():
    v = check_app_wdt_clean("[WDT] reconfigure failed: -1\n")
    assert v.status is Status.FAIL


def test_app_wdt_clean_flags_the_240s_stall_restart():
    v = check_app_wdt_clean("[WDT] OBD task stalled >240s — restarting\n")
    assert v.status is Status.FAIL


def test_app_wdt_clean_passes_on_healthy_native_log():
    assert check_app_wdt_clean(HEALTHY_NATIVE).status is Status.PASS


def test_banner_env_passes_when_it_matches():
    assert check_banner_env(HEALTHY_NATIVE, "crowpanel_obd").status is Status.PASS


def test_banner_env_fails_on_a_wrong_env_flash():
    # default_envs = crowpanel_obd makes this an easy mistake to make.
    v = check_banner_env(HEALTHY_NATIVE, "crowpanel")
    assert v.status is Status.FAIL
    assert "crowpanel_obd" in v.detail


def test_banner_env_skips_when_the_banner_never_arrived():
    assert check_banner_env(CONTENDED_NATIVE, "crowpanel").status is Status.SKIP


@pytest.mark.parametrize(
    "log,expect,status",
    [
        ("[encoder] found\n",   "yes",  Status.PASS),
        ("[encoder] MISSING\n", "yes",  Status.FAIL),
        ("[encoder] MISSING\n", "no",   Status.PASS),
        ("[encoder] found\n",   "no",   Status.FAIL),
        ("[encoder] found\n",   "auto", Status.PASS),
        ("[encoder] MISSING\n", "auto", Status.PASS),
        ("",                    "auto", Status.SKIP),
    ],
)
def test_encoder_expectations(log, expect, status):
    assert check_encoder(log, expect).status is status


def test_theme_ack_requires_the_theme_line():
    assert check_theme_ack("[THEME] night\n").status is Status.PASS
    assert check_theme_ack("").status is Status.FAIL


def test_mock_alarm_ack_matches_either_mock_state():
    assert check_mock_alarm_ack("[MOCK] alarm sweep\n").status is Status.PASS
    assert check_mock_alarm_ack("[MOCK] safe bands (no alarms)\n").status is Status.PASS
    assert check_mock_alarm_ack("").status is Status.FAIL


def test_ble_scanning_matches_the_utf8_ellipsis_line():
    # ble_obd_source.cpp:309 uses U+2026, NOT three ASCII dots. Matching "..."
    # would silently never fire, so the check must key off the prefix.
    assert check_ble_scanning(HEALTHY_NATIVE).status is Status.PASS


def test_ble_scanning_fails_when_the_state_machine_never_scans():
    v = check_ble_scanning("[BOOT] env=crowpanel_obd ver=local git=local profile=generic\n")
    assert v.status is Status.FAIL
