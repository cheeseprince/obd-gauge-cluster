"""Tests for scripts/check_no_pii.py — the guard that keeps real VINs out.

It is a REQUIRED CI check and a pre-commit hook, it protects the leak class that
forced two repo recreates, and until 2026-08-16 nothing verified it. A guard
whose patterns are only checked by reading is exactly the shape of thing this
repo has been bitten by before.

The VIN used below is SYNTHETIC and already in the allowlist, so nothing here
stores anything derived from a real vehicle — the same rule the guard's own
docstring sets for itself.
"""
import importlib.util
import pathlib
import sys

_GUARD = pathlib.Path(__file__).resolve().parents[3] / "scripts" / "check_no_pii.py"
assert _GUARD.is_file(), f"guard not found at {_GUARD}"
_spec = importlib.util.spec_from_file_location("check_no_pii", _GUARD)
guard = importlib.util.module_from_spec(_spec)
sys.modules["check_no_pii"] = guard
_spec.loader.exec_module(guard)

# Allowlisted (synthetic) and a not-allowlisted synthetic of the same shape.
#
# FOREIGN IS ASSEMBLED AT RUNTIME, NOT WRITTEN AS A LITERAL. It has to be a token
# the guard REJECTS -- that is the whole point of it -- so a 17-char literal here
# would make this very file fail the guard it tests. Allowlisting it instead
# would defeat the tests that need a rejected token. Splitting it means the
# file's text contains no VIN-shaped run, while the tests still see one.
#
# (CI caught exactly this on the first push of these tests: the guard flagged
# the literal the moment the file became tracked. It was right to.)
ALLOWED = "1FT8W3BT0N2345678"          # in ALLOWED_VINS, so a literal is fine
FOREIGN = "1ZZ9W3BT0N" + "7654321"


def _hex(v):
    return v.encode().hex().upper()


# --- the allowlist still works ---------------------------------------------

def test_allowlisted_vin_is_permitted():
    assert guard.vin_tokens(f"vin = {ALLOWED}") == []


def test_unknown_vin_is_caught():
    assert any(FOREIGN in t for t in guard.vin_tokens(f"vin = {FOREIGN}"))


# --- separator-wrapped: the \b hole ----------------------------------------
# `_` is a word character, so a \b-anchored pattern never fires inside a
# filename-shaped string. This was a real miss.

def test_snake_case_wrapped_vin_is_caught():
    assert guard.vin_tokens(f"log_{FOREIGN}_drive.csv"), "underscore-wrapped VIN must be caught"


def test_dash_and_dot_wrapped_vins_are_caught():
    assert guard.vin_tokens(f"log-{FOREIGN}-drive.csv"), "dash-wrapped"
    assert guard.vin_tokens(f"{FOREIGN}.csv"), "dot-suffixed"


# --- hex: the encoding the scanner itself produces --------------------------
# tools/obd_scan stores replies as payload_hex, and the 22F1xx block covers
# 22F190 whose value IS the VIN. So hex is the natural leak encoding here.

def test_hex_encoded_vin_is_caught():
    hits = guard.vin_tokens(f'"payload_hex": "{_hex(FOREIGN)}"')
    assert hits and "hex-encoded" in hits[0]


def test_hex_encoded_vin_is_caught_when_not_byte_zero_of_the_run():
    # A real F190 reply carries a 62F190 header before the VIN bytes.
    run = "62F190" + _hex(FOREIGN) + "00"
    assert guard.vin_tokens(f'"payload_hex": "{run}"'), "VIN must be found mid-run"


def test_hex_of_an_allowlisted_vin_is_permitted():
    assert guard.vin_tokens(f'"payload_hex": "{_hex(ALLOWED)}"') == []


def test_lowercase_hex_is_caught():
    assert guard.vin_tokens(_hex(FOREIGN).lower()), "hex case must not matter"


# --- no false positives on things the tree is full of -----------------------

def test_sha256_digests_do_not_false_positive():
    # requirements/*.txt carries hundreds of these; a digest decodes to random
    # bytes, not to VIN-charset ASCII.
    digests = [
        "a" * 64,
        "0123456789abcdef" * 4,
        "9f069f41e3c0c4fa2e56c392ab471af7adfc95d908081f32c72b3b49491e996f",
        "974a75ecaf957fcf2b03a9b1204d7eed516f46b5b4681124a849e1e9e026d955",
    ]
    for d in digests:
        assert guard.vin_tokens(f"--hash=sha256:{d}") == [], d


def test_long_alphanumeric_runs_do_not_false_positive():
    # The original \b anchor existed to stop a 17-char window inside the base64
    # OTA public key from matching. That protection must survive.
    b64 = "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEei2TMeyABCDEFGHJKLMNPRSTUVWXYZ0123456789"
    assert guard.vin_tokens(b64) == []


def test_short_and_empty_inputs():
    assert guard.vin_tokens("") == []
    assert guard.vin_tokens("too short 1FT8W3BT0N") == []


# --- a stated limitation, pinned so it is not mistaken for coverage ---------

def test_whitespace_split_vin_is_a_known_miss():
    """Documented in the guard's docstring as NOT covered.

    Joining across whitespace would match unrelated adjacent tokens, so the
    trade is deliberate. This test exists so the limitation is visible in the
    suite rather than only in prose — if someone later closes the gap, this
    fails and tells them to update the docstring.
    """
    assert guard.vin_tokens(f"{FOREIGN[:8]} {FOREIGN[8:]}") == []


# --- the guard must scan ITSELF -------------------------------------------
# It used to exempt itself. That exemption meant the one file the guard could
# not inspect was the one most likely to accumulate VIN-shaped examples -- and
# on 2026-08-16 a docstring added a REAL VIN as an illustration, the guard
# reported OK, CI agreed, and it reached the public repo. These tests exist so
# the exemption cannot come back silently.

def test_guard_does_not_exempt_itself():
    src = _GUARD.read_text()
    assert "never scan the guard itself" not in src, \
        "the self-exemption is back; it is what let a real VIN reach the public repo"


def test_guards_own_source_is_clean_under_its_own_rules():
    """The guard's own file must pass the guard. Belt and braces: even if the
    file-walk regressed, this asserts the CONTENT directly."""
    assert guard.vin_tokens(_GUARD.read_text()) == [], \
        "the guard's own source contains a non-allowlisted VIN-shaped token"


def test_docstring_examples_are_allowlisted():
    """Every VIN-shaped example in the docstring must be a permitted synthetic."""
    doc = guard.__doc__ or ""
    for tok in guard.VIN_RE.findall(doc):
        assert tok.upper() in guard.ALLOWED_VINS, f"docstring example {tok!r} is not allowlisted"
