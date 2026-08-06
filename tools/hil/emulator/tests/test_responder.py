"""Tests for the ELM327 responder.

Byte-exactness is the whole point. The dash parses these strings with the same
code that talks to a real vLinker, so a reply that is *nearly* right teaches the
rig nothing — it just moves the bug from the firmware into the fixture.

Every expectation below was taken from a real exchange captured on the bench,
not invented.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from elm_responder import ElmResponder, Scenario, iso_tp_frames


def r(**kw):
    return ElmResponder(Scenario(**kw))


# --- AT command layer ------------------------------------------------------

def test_atz_returns_the_identity_banner():
    out = r().handle("ATZ")
    assert "ELM327 v1.5" in out
    assert out.endswith(">")


def test_echo_is_on_after_reset_and_ate0_turns_it_off():
    # A real ELM327 powers up with echo ENABLED, so the firmware's ATE0 is not
    # ceremonial: ble_obd_source.cpp treats a missing ATE0 ack as fatal and
    # abandons the connect. If the responder never echoed, that retry path
    # would never be exercised.
    e = r()
    assert e.handle("ATZ").startswith("ATZ")      # echoed
    assert "OK" in e.handle("ATE0")
    assert not e.handle("ATL0").startswith("ATL0")  # no longer echoed


def test_header_command_is_accepted_with_or_without_spaces():
    # The firmware emits BOTH forms: "ATSH7E0" from one path and "AT SH 7E0"
    # from another. Accepting only one silently breaks half the queries.
    e = r(); e.handle("ATE0")
    assert "OK" in e.handle("ATSH7E0")
    assert e.header == "7E0"
    assert "OK" in e.handle("AT SH 7E2")
    assert e.header == "7E2"


def test_unknown_at_command_is_answered_ok_not_ignored():
    # Silence would hang the firmware's drainToPrompt for its full timeout.
    e = r(); e.handle("ATE0")
    assert e.handle("ATXYZ").endswith(">")


# --- Mode 01 ---------------------------------------------------------------

def test_mode01_known_pid_returns_a_positive_answer():
    e = r(mode01={"0C": "1AF8"}); e.handle("ATE0")
    assert e.handle("010C") == "410C1AF8\r\r>"


def test_mode01_unknown_pid_returns_no_data():
    e = r(mode01={}); e.handle("ATE0")
    assert e.handle("0199") == "NO DATA\r\r>"


# --- Mode 22, per-header -----------------------------------------------------

def test_mode22_is_routed_by_the_current_header():
    # THE point of the whole rig. 221940 lives on the TRANSMISSION ecu (7E2);
    # asking the engine ecu (7E0) for it must NOT answer, or a firmware bug that
    # forgets to switch headers would pass.
    e = r(mode22={"7E2": {"1940": "008C"}}); e.handle("ATE0")
    e.handle("ATSH7E0")
    assert e.handle("221940") == "NO DATA\r\r>"
    e.handle("ATSH7E2")
    assert e.handle("221940") == "621940008C\r\r>"


def test_mode22_positive_answer_echoes_two_request_bytes():
    # Service 22 echoes TWO bytes of the request (the 16-bit DID); service 01
    # echoes one. Getting this wrong shifts every payload by a byte.
    e = r(mode22={"7E0": {"000C": "0BB8"}}); e.handle("ATE0"); e.handle("ATSH7E0")
    assert e.handle("22000C") == "62000C0BB8\r\r>"


# --- ISO-TP framing --------------------------------------------------------

def test_short_payload_is_a_single_frame_with_no_length_header():
    assert iso_tp_frames("410C1AF8") == "410C1AF8\r\r>"


def test_long_payload_is_split_into_indexed_frames_with_a_length_header():
    # Captured shape: "014\r0:<6 bytes>\r1:<7 bytes>\r2:<7 bytes>\r\r>".
    # Frame 0 carries SIX bytes (the CAN first-frame PCI eats two); every
    # consecutive frame carries seven. The header is the total payload length
    # in HEX, and 0x014 == 20 == 6+7+7.
    vin_payload = "490201" + "".join(f"{ord(c):02X}" for c in "3GTUUEE80S2345678")
    out = iso_tp_frames(vin_payload)
    assert out.startswith("014\r")
    assert "\r0:490201334754\r" in out
    assert "\r1:55554545383053\r" in out
    assert "\r2:32333435363738\r" in out
    assert out.endswith("\r\r>")


def test_frame_index_wraps_in_the_low_nibble():
    # ELM cycles the sequence index 0..F. A 200-byte reply must not print "10:".
    out = iso_tp_frames("AA" * 200)
    assert "\rF:" in out
    assert "\r10:" not in out


def test_vin_reply_round_trips_to_the_expected_vin():
    # End-to-end on the fixture itself: the bytes the dash will reassemble must
    # spell the VIN we pinned, or the profile-selection test proves nothing.
    e = r(vin="3GTUUEE80S2345678"); e.handle("ATE0")
    out = e.handle("0902")
    hexes = "".join(part.split(":", 1)[1] for part in out.split("\r") if ":" in part)
    payload = bytes.fromhex(hexes)
    assert payload[:3] == b"\x49\x02\x01"
    assert payload[3:20].decode() == "3GTUUEE80S2345678"


# --- spaces ----------------------------------------------------------------

def test_ats1_enables_spaces_and_ats0_disables_them():
    # The firmware sends ATS0 and its parser strips spaces anyway, but a rig
    # that ignored the command would never catch a regression in that stripping.
    e = r(mode01={"0C": "1AF8"}); e.handle("ATE0")
    e.handle("ATS1")
    assert e.handle("010C") == "41 0C 1A F8\r\r>"
    e.handle("ATS0")
    assert e.handle("010C") == "410C1AF8\r\r>"
