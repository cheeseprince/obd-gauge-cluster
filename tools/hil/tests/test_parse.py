"""Golden-log tests for the pure verdict logic.

The NEGATIVE fixtures carry the weight here. A parser with no failing fixtures
is a parser that always passes, which is the failure mode this rig exists to
avoid.
"""
import pathlib
import sys
from pathlib import Path

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import pytest
from hil.parse import (
    Status,
    Verdict,
    check_app_wdt_clean,
    check_banner_env,
    check_ble_scanning,
    check_boot_mode,
    check_encoder,
    check_mock_alarm_ack,
    check_no_panic,
    check_no_wdt_spam,
    check_theme_ack,
    downgrade_uart_verdicts,
    exit_code,
    parse_banner,
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

# Real capture, 2026-07-29 (`tools/hil/tests/logs/uart_boot.log`, which is
# verbatim: the scrub sweep for the board's MAC and the VIN pattern found
# nothing to remove on the UART side). runner.py opens the UART
# tap BEFORE flashing, so every genuine capture starts with esptool's own
# reset INTO download mode to write the image, followed by the real post-flash
# boot. Both ROM boot lines land in one capture; check_boot_mode must judge
# the run by the LAST one, not the first.
TWO_BOOT_UART = DOWNLOAD_MODE_UART + """\
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:1
"""

# The reverse order: a healthy flash boot followed LATER by a download-mode
# reset (e.g. the board dropped back into the bootloader after booting —
# a spurious re-flash trigger, a stuck GPIO0, a second `pio upload` racing
# the same port). Order is the entire point of the fix above: a check that
# only verified "both patterns appear somewhere in the capture" would pass
# under the old first-match code AND the new last-match code, and would
# never catch this case going unhealthy.
FLASH_THEN_DOWNLOAD_UART = HEALTHY_UART + """\
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

# The banner is two lines from one Serial.println, and capture_native reopens
# the port ~1 s after reset — so a capture can land line 1 and lose line 2
# entirely. Everything textual is present; every NUMBER is absent.
TRUNCATED_BANNER_NATIVE = """\
[BOOT] env=crowpanel_obd ver=local git=local profile=gm_sierra_lz0
[encoder] found
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


def test_parse_banner_returns_none_on_a_garbled_number():
    text = HEALTHY_NATIVE.replace("heap=241344", "heap=24\x001344")
    assert parse_banner(text) is None


def test_parse_banner_returns_none_on_a_garbled_size():
    text = HEALTHY_NATIVE.replace("psram=8MB", "psram=?MB")
    assert parse_banner(text) is None


def test_parse_banner_returns_none_when_line_two_was_never_captured():
    # A MISSING number must be as unusable as a garbled one. Before this,
    # _num() substituted 0 for anything absent, so a capture holding only
    # banner line 1 produced a VALID Banner claiming psram=0MB flash=0MB
    # heap=0 — and verdict.json recorded heap 0 as a real measurement.
    assert parse_banner(TRUNCATED_BANNER_NATIVE) is None


@pytest.mark.parametrize("field", ["psram=8MB", "flash=16MB", "reset=1", "heap=241344"])
def test_parse_banner_requires_every_numeric_field(field):
    # All four are printed by the same Serial.println, so in practice they go
    # missing together — but each is individually required, so no single one can
    # be quietly defaulted back to a fabricated value later.
    text = HEALTHY_NATIVE.replace(field + " ", "").replace(" " + field, "")
    assert field not in text
    assert parse_banner(text) is None


def test_check_banner_env_skips_on_a_truncated_banner():
    # The verdict-level consequence: an admitted absence (SKIP -> exit 3), not
    # a green check backed by fabricated zeros.
    v = check_banner_env(TRUNCATED_BANNER_NATIVE, "crowpanel_obd")
    assert v.status is Status.SKIP


def test_boot_mode_passes_on_flash_boot():
    assert check_boot_mode(HEALTHY_UART).status is Status.PASS


def test_boot_mode_fails_in_download_mode():
    v = check_boot_mode(DOWNLOAD_MODE_UART)
    assert v.status is Status.FAIL
    # Exact, not a substring check: the real ROM line nests parens
    # ("DOWNLOAD(USB/UART0)"), and a naive [^)]+ capture truncates the value
    # at the FIRST ')', silently dropping the closing paren. A loose "DOWNLOAD
    # in detail" assertion would pass either way and hide that bug.
    assert v.detail == "booted as DOWNLOAD(USB/UART0)"


def test_boot_mode_skips_when_there_is_no_rom_line():
    # No evidence is not the same as evidence of health.
    assert check_boot_mode("").status is Status.SKIP


def test_boot_mode_judges_the_last_boot_not_the_flash_entry():
    # THE real-hardware bug found on the first HIL run (2026-07-29): the UART
    # tap is opened before flashing, so every genuine capture starts with
    # esptool's own reset into download mode to write the image. That is
    # normal and not a failure — the outcome that matters is the LAST boot
    # the ROM reports. Before the fix, check_boot_mode used re.search (first
    # match) and always FAILed a perfectly healthy flash.
    v = check_boot_mode(TWO_BOOT_UART)
    assert v.status is Status.PASS
    assert v.detail == "SPI_FAST_FLASH_BOOT"


def test_boot_mode_fails_when_the_last_boot_is_download_mode():
    # The reverse of the fixture above. Order, not mere presence, is what the
    # fix keys off — this pins that a healthy boot line earlier in the
    # capture cannot paper over an unhealthy one that came later.
    v = check_boot_mode(FLASH_THEN_DOWNLOAD_UART)
    assert v.status is Status.FAIL
    assert v.detail == "booted as DOWNLOAD(USB/UART0)"


def test_no_panic_passes_on_clean_log():
    assert check_no_panic(HEALTHY_UART).status is Status.PASS


def test_no_panic_fails_on_guru_meditation():
    v = check_no_panic(PANIC_UART)
    assert v.status is Status.FAIL
    assert "Guru Meditation" in v.detail


def test_no_panic_skips_on_an_empty_capture():
    # A dead UART port is not a clean bill of health.
    assert check_no_panic("").status is Status.SKIP


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


def test_wdt_spam_skips_on_an_empty_capture():
    assert check_no_wdt_spam("").status is Status.SKIP


def test_uart_checks_skip_on_the_contended_garbage_capture():
    # Real 26-byte capture from a reader contending with esptool: it contains the
    # ROM's first line but no boot: line, so it is not UART evidence.
    for v in (check_no_panic(CONTENDED_NATIVE), check_no_wdt_spam(CONTENDED_NATIVE)):
        assert v.status is Status.SKIP


def test_app_wdt_clean_flags_reconfigure_failure():
    # main.cpp:159 logs this before the banner at line 172-191, but setup()
    # continues past it and the banner still prints — so a real capture of this
    # marker always carries [BOOT] evidence too. The fixture reflects that.
    v = check_app_wdt_clean(
        "[BOOT] env=crowpanel_obd ver=local git=local profile=generic\n"
        "[WDT] reconfigure failed: -1\n"
    )
    assert v.status is Status.FAIL


def test_app_wdt_clean_flags_the_240s_stall_restart():
    # main.cpp:230 fires from loop(), which only runs after setup() already
    # printed the banner — so this marker is likewise always preceded by [BOOT]
    # in a genuine capture.
    v = check_app_wdt_clean(
        "[BOOT] env=crowpanel_obd ver=local git=local profile=generic\n"
        "[WDT] OBD task stalled >240s — restarting\n"
    )
    assert v.status is Status.FAIL


def test_app_wdt_clean_passes_on_healthy_native_log():
    assert check_app_wdt_clean(HEALTHY_NATIVE).status is Status.PASS


def test_app_wdt_clean_skips_on_an_empty_capture():
    assert check_app_wdt_clean("").status is Status.SKIP


def test_app_wdt_clean_skips_when_the_banner_never_arrived():
    # Console output with no [BOOT] means the app never finished setup().
    assert check_app_wdt_clean("[BLE] scanning\n").status is Status.SKIP


def test_banner_env_passes_when_it_matches():
    assert check_banner_env(HEALTHY_NATIVE, "crowpanel_obd").status is Status.PASS


def test_banner_env_fails_on_a_wrong_env_flash():
    # default_envs = crowpanel_obd makes this an easy mistake to make.
    v = check_banner_env(HEALTHY_NATIVE, "crowpanel")
    assert v.status is Status.FAIL
    assert "crowpanel_obd" in v.detail


def test_banner_env_skips_when_the_banner_never_arrived():
    assert check_banner_env(CONTENDED_NATIVE, "crowpanel").status is Status.SKIP


def test_check_banner_env_skips_rather_than_raising_on_a_garbled_banner():
    # The whole point: a corrupted capture yields a verdict, never a traceback.
    text = HEALTHY_NATIVE.replace("reset=1", "reset=x")
    assert check_banner_env(text, "crowpanel_obd").status is Status.SKIP


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


# The real 2026-07-29 flake: `[MOCK] alarm sweep` WAS emitted, ~2 s late, and so
# landed in the soak portion of the capture rather than the 's' probe window.
# Both acks are evaluated over the whole capture now, so a late-but-present ack
# counts. `'n'`/`'s'` are sent once per run, so any such line is that one ack.
LATE_ACK_BOOT = "[BOOT] env=crowpanel ver=local git=local profile=generic\n"
LATE_ACK_SOAK = "[THEME] night (resolved, mode=ON)\n[MOCK] alarm sweep\n"


def test_acks_pass_when_the_reply_arrives_only_in_the_soak_portion():
    combined = LATE_ACK_BOOT + LATE_ACK_SOAK
    assert check_theme_ack(combined).status is Status.PASS
    assert check_mock_alarm_ack(combined).status is Status.PASS


def test_acks_still_fail_when_the_ack_is_nowhere_in_the_capture():
    # The negative case must still bite, or widening the scope would have turned
    # these into checks that can no longer fail. A boot banner with no ack means
    # a key was written and nothing ever came back — positive evidence of a
    # wedged console, not absence of observation, hence FAIL and not SKIP.
    assert check_theme_ack(LATE_ACK_BOOT).status is Status.FAIL
    assert check_mock_alarm_ack(LATE_ACK_BOOT).status is Status.FAIL
    assert check_theme_ack("").status is Status.FAIL
    assert check_mock_alarm_ack("").status is Status.FAIL


def test_a_theme_ack_is_not_accepted_as_a_mock_ack():
    # Widened scope must not blur the two stimuli into one another.
    assert check_mock_alarm_ack(LATE_ACK_BOOT + "[THEME] night\n").status is Status.FAIL
    assert check_theme_ack(LATE_ACK_BOOT + "[MOCK] alarm sweep\n").status is Status.FAIL


def test_ble_scanning_matches_the_utf8_ellipsis_line():
    # ble_obd_source.cpp:309 uses U+2026, NOT three ASCII dots. Matching "..."
    # would silently never fire, so the check must key off the prefix.
    assert check_ble_scanning(HEALTHY_NATIVE).status is Status.PASS


def test_ble_scanning_fails_when_the_state_machine_never_scans():
    v = check_ble_scanning("[BOOT] env=crowpanel_obd ver=local git=local profile=generic\n")
    assert v.status is Status.FAIL


# --- real captures from the board, tests/logs/ -----------------------------
#
# Everything above is a fixture typed by hand. These two files are not: they
# are `runs/20260729T214724-crowpanel/{native,uart}.log` from the rig's first
# real run against hardware (2026-07-29), copied verbatim — the pre-commit
# scrub swept for the board's MAC address and for the repository's fake-VIN
# pattern and found neither, so nothing was edited out and these bytes are
# exactly what the board emitted.
# The uart.log fixture's raw ROM-download bytes are what motivated the
# check_boot_mode fix above (TWO_BOOT_UART is a hand-shortened stand-in for
# this same real shape) — this test locks the parser against the actual
# bytes rather than a fixture someone typed from memory.

LOGS = Path(__file__).parent / "logs"


def test_real_captured_boot_log_parses_and_passes():
    """Locks the parser against bytes the board actually produced, not against
    a fixture someone typed from memory."""
    native = (LOGS / "native_boot_crowpanel.log").read_text()
    uart = (LOGS / "uart_boot.log").read_text()

    b = parse_banner(native)
    assert b is not None, "the real capture must contain a [BOOT] banner"
    assert b.env == "crowpanel"
    assert b.psram_mb == 8 and b.flash_mb == 16

    assert check_boot_mode(uart).status is Status.PASS
    assert check_no_panic(uart).status is Status.PASS
    assert check_no_wdt_spam(uart).status is Status.PASS


# --- the exit-code contract ------------------------------------------------
#
# This mapping is the rig's whole promise: "the rig is broken" must never read
# as "the firmware is good", and neither must "we did not look". It was pure
# logic inline in __main__ with no tests at all, where miscounting a SKIP as a
# PASS would silently convert every exit 3 into an exit 0.

def _v(status, name="c"):
    return Verdict(name, status)


def test_exit_code_all_pass_is_zero():
    assert exit_code([_v(Status.PASS), _v(Status.PASS)]) == 0


def test_exit_code_any_fail_is_one():
    assert exit_code([_v(Status.PASS), _v(Status.FAIL)]) == 1


def test_exit_code_fail_outranks_skip():
    # A run that both failed and skipped is a firmware problem: 1, the louder
    # and more actionable answer, not 3.
    assert exit_code([_v(Status.SKIP), _v(Status.FAIL)]) == 1


def test_exit_code_fail_outranks_skip_even_with_allow_skips():
    # --allow-skips forgives absence, never a failure.
    assert exit_code([_v(Status.SKIP), _v(Status.FAIL)], allow_skips=True) == 1


def test_exit_code_skip_alone_is_three():
    assert exit_code([_v(Status.PASS), _v(Status.SKIP)]) == 3


def test_exit_code_allow_skips_turns_three_into_zero():
    assert exit_code([_v(Status.PASS), _v(Status.SKIP)], allow_skips=True) == 0


def test_exit_code_of_an_empty_table_is_three_even_with_allow_skips():
    # Documented decision: no verdicts at all is the most complete form of "no
    # evidence captured", so it can never be 0. --allow-skips says specific
    # known checks may be absent on this bench; it is not permission to accept a
    # run that asserted nothing whatsoever. Unreachable from __main__ today —
    # pinned so a refactor that can emit an empty table fails loudly.
    assert exit_code([]) == 3
    assert exit_code([], allow_skips=True) == 3


# --- a dead UART tap must not leave green checks behind --------------------

def _uart_verdicts():
    """What the three UART checks return from a capture that STOPPED EARLY.

    This is the dangerous shape: text captured before the tap died still holds
    the post-flash ROM boot line, so check_boot_mode passes on it and it also
    satisfies the evidence gate for the two negative-marker checks. All three
    come back PASS having observed a fraction of the run.
    """
    return [
        Verdict("flash", Status.PASS, "crowpanel"),
        check_boot_mode(TWO_BOOT_UART),
        check_no_panic(TWO_BOOT_UART),
        check_no_wdt_spam(TWO_BOOT_UART),
    ]


def test_a_healthy_capture_really_does_pass_all_three_uart_checks():
    # The premise of the downgrade: without it, these are three green checks.
    assert [v.status for v in _uart_verdicts()[1:]] == [Status.PASS] * 3
    assert exit_code(_uart_verdicts()) == 0


def test_dead_tap_downgrades_uart_passes_to_skip():
    out = downgrade_uart_verdicts(_uart_verdicts(), "UART tap died: [Errno 5] I/O error")
    by_name = {v.check: v for v in out}
    for name in ("boot mode", "no panic", "no task_wdt spam"):
        assert by_name[name].status is Status.SKIP
        assert "UART tap died" in by_name[name].detail
    # ...and the run stops claiming to be green.
    assert exit_code(out) == 3


def test_dead_tap_leaves_non_uart_verdicts_alone():
    out = downgrade_uart_verdicts(_uart_verdicts(), "cable out")
    assert out[0] == Verdict("flash", Status.PASS, "crowpanel")


def test_dead_tap_preserves_an_observed_fail():
    # A panic that was actually SEEN is real evidence no matter what happened to
    # the cable afterwards. Demoting it to SKIP would discard the single most
    # valuable thing the run found.
    fail = check_no_panic(PANIC_UART)
    assert fail.status is Status.FAIL
    out = downgrade_uart_verdicts([fail], "cable out")
    assert out == [fail]
    assert exit_code(out) == 1


def test_dead_tap_leaves_an_existing_skip_untouched():
    # An empty capture already says "no evidence"; rewriting it would only lose
    # the more specific reason.
    skip = check_no_panic("")
    assert skip.status is Status.SKIP
    assert downgrade_uart_verdicts([skip], "cable out") == [skip]
