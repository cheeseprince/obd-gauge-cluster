"""Tests for scripts/check_notices.py -- the guard over THIRD-PARTY-NOTICES.md.

A guard is only worth what its failures are worth, and a licence guard that
silently passes is indistinguishable from no guard at all. THIRD-PARTY-NOTICES.md
ships beside the binary to satisfy LGPL-2.1 and Apache-2.0 notice terms, so every
drift the guard claims to catch is driven here against the REAL repository files
with one thing mutated -- not against a hand-built fixture that could agree with
the guard while both disagree with the tree.

Lives beside test_pii_guard.py for the same reason it does: tools/obd_scan is the
pytest suite CI runs on both Linux and Windows, so a guard tested here is tested
on both.
"""
import importlib.util
import pathlib
import sys

import pytest

_REPO = pathlib.Path(__file__).resolve().parents[3]
_GUARD = _REPO / "scripts" / "check_notices.py"
assert _GUARD.is_file(), f"guard not found at {_GUARD}"
_spec = importlib.util.spec_from_file_location("check_notices", _GUARD)
guard = importlib.util.module_from_spec(_spec)
sys.modules["check_notices"] = guard
_spec.loader.exec_module(guard)

PIO_TEXT = (_REPO / "platformio.ini").read_text(encoding="utf-8")
NOTICES_TEXT = (_REPO / "THIRD-PARTY-NOTICES.md").read_text(encoding="utf-8")


@pytest.fixture
def shipped():
    return guard.load_shipped()


@pytest.fixture
def pins():
    return guard.parse_platformio(PIO_TEXT)


@pytest.fixture
def notices():
    return guard.parse_notices(NOTICES_TEXT)


# --- the parsers see the real files -----------------------------------------

def test_platformio_pins_are_read(pins):
    # The shipped env's four libraries plus the platform. Exact values, because
    # a parser that returned the right KEYS with empty versions would make every
    # version comparison vacuously... loud, but for the wrong reason.
    assert pins["LovyanGFX"] == "1.2.26"
    assert pins["lvgl"] == "8.4.0"
    assert pins["NimBLE-Arduino"] == "2.5.1"
    assert pins["platform-espressif32"] == "53.03.13"
    assert pins["Modulino"].startswith("1f40e45")


def test_only_the_shipped_env_is_read():
    # env:crowpanel is the MOCK_OBD display build and carries no NimBLE. If the
    # section handling leaked across headers, NimBLE would appear here.
    only_mock = guard.parse_platformio(PIO_TEXT, env="crowpanel")
    assert "NimBLE-Arduino" not in only_mock
    assert only_mock["lvgl"] == "8.4.0"


def test_notices_table_is_read_and_stops_at_the_table(notices):
    assert notices["Arduino core for ESP32"] == ("3.1.3", "LGPL-2.1-or-later")
    assert notices["Modulino"][0] == "1f40e45"          # backticks stripped
    # The reproduced GPL/MPL texts below the table contain pipes of their own.
    # Nothing from them may be mistaken for a component.
    assert len(notices) == len(guard.load_shipped())
    assert not any("GNU" in name or "TERMS" in name for name in notices)


# --- the tree is currently in step ------------------------------------------

def test_repo_is_currently_in_step(shipped, pins, notices):
    errors, _ = guard.check(shipped, pins, notices)
    assert errors == [], "\n".join(errors)


def test_the_lgpl_row_is_reported_unverifiable_not_skipped(shipped, pins, notices):
    """The Arduino core version cannot be checked offline. Say so, every run.

    This is the assertion that keeps the guard honest: if someone later makes
    those versions checkable, or drops them from SHIPPED, this fails and forces
    a decision rather than letting the LGPL-2.1 row quietly stop being mentioned.
    """
    _, unverifiable = guard.check(shipped, pins, notices)
    assert any("Arduino core for ESP32" in u for u in unverifiable)
    assert any("ESP-IDF" in u for u in unverifiable)


# --- every drift the docstring promises to catch, driven red ----------------

def test_new_dependency_with_no_notice_fails(shipped, notices):
    """The case that started all this: a dep added, the notices file forgotten."""
    added = PIO_TEXT.replace(
        "    lovyan03/LovyanGFX@1.2.26\n",
        "    lovyan03/LovyanGFX@1.2.26\n    somebody/Telemetry@2.0.1\n")
    assert added != PIO_TEXT
    errors, _ = guard.check(shipped, guard.parse_platformio(added), notices)
    assert any("Telemetry" in e for e in errors), errors


def test_version_drift_fails(shipped, pins, notices):
    drifted = dict(notices)
    drifted["LVGL"] = ("8.3.0", "MIT")
    errors, _ = guard.check(shipped, pins, drifted)
    assert any("LVGL" in e and "8.3.0" in e for e in errors), errors


def test_missing_notices_row_fails(shipped, pins, notices):
    dropped = {k: v for k, v in notices.items() if k != "NimBLE-Arduino"}
    errors, _ = guard.check(shipped, pins, dropped)
    assert any("NimBLE-Arduino" in e and "NO row" in e for e in errors), errors


def test_stale_notices_row_fails(shipped, pins, notices):
    stale = dict(notices)
    stale["RetiredThing"] = ("1.0.0", "MIT")
    errors, _ = guard.check(shipped, pins, stale)
    assert any("RetiredThing" in e for e in errors), errors


def test_licence_id_drift_fails(shipped, pins, notices):
    wrong = dict(notices)
    wrong["Arduino core for ESP32"] = ("3.1.3", "MIT")
    errors, _ = guard.check(shipped, pins, wrong)
    assert any("Arduino core" in e and "MIT" in e for e in errors), errors


def test_dependency_removed_from_platformio_fails(shipped, notices):
    without = PIO_TEXT.replace("    lvgl/lvgl@8.4.0\n", "")
    assert without != PIO_TEXT
    errors, _ = guard.check(shipped, guard.parse_platformio(without), notices)
    assert any("LVGL" in e and "no longer pinned" in e for e in errors), errors


# --- version matching ---------------------------------------------------------

def test_sha_prefix_counts_but_a_release_prefix_does_not():
    full = "1f40e45a4d11232f790039e681de6d9518b79385"
    assert guard._version_matches("1f40e45", full)
    assert not guard._version_matches("1f40", full)          # too short to be a pin
    # The prefix rule must NOT leak into release versions, or 1.2.2 would pass
    # against a 1.2.26 pin -- a real version, silently one release stale.
    assert guard._version_matches("1.2.26", "1.2.26")
    assert not guard._version_matches("1.2.2", "1.2.26")


# --- a guard that parses nothing must not report success ---------------------

def test_empty_parse_is_not_a_pass():
    """A reformat that defeats a parser must fail loudly, not pass vacuously.

    Every check here is 'for each thing I parsed'. Parse nothing and every one of
    them is trivially satisfied -- the exact shape of a green check that means
    nothing, which is why main() refuses before reaching check().
    """
    assert guard.parse_platformio(PIO_TEXT, env="env-that-does-not-exist") == {}
    assert guard.parse_notices("no table here at all") == {}

    errors, _ = guard.check(guard.load_shipped(), {}, {})
    assert errors, "check() on empty inputs must not look clean"
