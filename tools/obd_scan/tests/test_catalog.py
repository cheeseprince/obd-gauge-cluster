import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from obd_scan import catalog as cat


def test_read_modes_allowed():
    for req in ["0100", "010C", "0902", "22F446", "03"]:
        cat.validate_request(req)          # must not raise


@pytest.mark.parametrize("req", [
    "2EF190AA",   # write data by identifier
    "310112",     # routine control
    "2F110303",   # I/O control
    "1101",       # ECU reset
    "14FFFFFF",   # clear DTCs
    "2701",       # security access
    "1003",       # session control
    "2803",       # communication control
    "8502",       # control DTC setting
])
def test_write_modes_rejected(req):
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_request(req)


def test_malformed_request_rejected():
    for bad in ["", "2", "ZZ", "22F4"]:      # odd/short/non-hex
        with pytest.raises(cat.UnsafeRequest):
            cat.validate_request(bad)


def test_at_allowlist():
    cat.validate_at("ATZ")
    cat.validate_at("AT SH 7E0")
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_at("ATPP 2F SV 01")     # programmable-parameter write


def test_preset_validation_rejects_unsafe_block():
    bad = cat.VehiclePreset(
        name="bogus",
        blocks=[cat.Block("evil", prefix=0x2EF1)],   # mode 2E in a sweep block
        probes=["0100"],
    )
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_preset(bad)


def test_shipped_presets_are_safe():
    for preset in cat.PRESETS.values():
        cat.validate_preset(preset)          # must not raise


def test_shipped_headers_are_safe():
    # IMPORTANT 5: validate_preset() covers probes/blocks but not
    # Header.at_sh/at_sp/at_cp -- the other data-driven input to the
    # transmit path. Every catalog header (curated GM list AND any
    # hand-authored future header, e.g. Ford/BMW) must pass the same
    # AT-allowlist gate at startup.
    cat.validate_headers(cat.HEADERS_11BIT + cat.HEADERS_29BIT)   # must not raise


def test_validate_headers_rejects_malformed_header():
    bad = cat.Header("evil", at_sh="7E0\rATPP0CSV01", at_sp="6")
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_headers([bad])


def test_header_render():
    h = next(h for h in cat.HEADERS_11BIT if h.name == "7E0")
    assert h.at_sh == "7E0" and h.at_sp == "6" and h.at_cp is None
    h29 = next(h for h in cat.HEADERS_29BIT if h.name == "18DA10F1")
    assert h29.at_sh == "DA10F1" and h29.at_sp == "7" and h29.at_cp == "18"


def test_block_expands_to_requests():
    b = cat.Block("22F4xx", prefix=0x22F4, lo=0x00, hi=0x02)
    assert list(b.requests()) == ["22F400", "22F401", "22F402"]


def test_block_expands_to_requests_3byte_prefix():
    # A 3-byte prefix (service + 2-byte extended PID) must render at full
    # width — this is the missing coverage the review flagged.
    b = cat.Block("221Exx", prefix=0x221E00, lo=0x00, hi=0x02)
    assert list(b.requests()) == ["221E0000", "221E0001", "221E0002"]


# --- Command-smuggling: embedded CR/LF/NUL must not slip past the validator
# by hiding from the whitespace-stripped string the validator inspects while
# still being transmitted verbatim to an ELM327, which treats CR as a command
# terminator. ---------------------------------------------------------------

def test_embedded_cr_smuggled_write_rejected():
    # "0100" looks like an innocuous mode-01 read to a validator that strips
    # all whitespace before checking — but the adapter sees two commands:
    # "0100" and "2EF190AA" (write data by identifier).
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_request("0100\r2EF190AA")


def test_embedded_lf_smuggled_write_rejected():
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_request("0100\n2EF190AA")


def test_embedded_null_byte_rejected():
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_request("0100\x002EF190AA")


def test_embedded_cr_smuggled_at_write_rejected():
    # An allowed ATSH prefix hiding a persistent ATPP programmable-parameter
    # write behind an embedded CR.
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_at("ATSH 7E0\rATPP 0C SV 01")


def test_at_suffix_smuggling_rejected():
    # No CR needed at all: an allowed ATSH prefix with an unbounded,
    # non-hex suffix that forms a different command entirely.
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_at("ATSHATPP0CSV01")


@pytest.mark.parametrize("cmd", [
    "ATZ", "ATE0", "ATL0", "ATS0", "ATAT2", "ATST19", "ATSP0", "ATSP6",
    "ATSP7", "ATDP", "ATCP18", "ATSH7E0", "ATSHDA10F1", "ATRV", "ATI",
])
def test_legitimate_at_commands_still_allowed(cmd):
    cat.validate_at(cmd)          # must not raise — regression guard


def test_overlong_request_rejected():
    # Comfortably longer than any legitimate request; belt-and-braces
    # against pathological input regardless of hex-well-formedness.
    with pytest.raises(cat.UnsafeRequest):
        cat.validate_request("01" + "00" * 20)


def test_atcea_allowlisted():
    cat.validate_at("ATCEA12")      # BMW extended-addressing target — must pass
    cat.validate_at("ATCEA")        # bare = turn extended addressing OFF


def test_bmw_headers_validate():
    # Every AT command a BMW header can emit must pass the startup gate.
    cat.validate_headers(cat.BMW_HEADERS)


def test_bmw_preset_registered_and_valid():
    p = cat.PRESETS["bmw"]
    cat.validate_preset(p)
    cat.validate_headers(p.headers)
    assert "22DA25" in p.probes                     # the go/no-go DID
    names = {h.name for h in p.headers}
    assert {"BMW-612", "BMW-618"} <= names


def test_read_only_guard_rejects_writes():
    # Re-proof: no addressing change ever lets a write service through.
    for w in ("10", "1003", "2EF190AA", "31010203", "2FAB", "27"):
        try:
            cat.validate_request(w)
            assert False, f"write {w!r} was not rejected"
        except cat.UnsafeRequest:
            pass


def test_allowed_modes_is_exactly_the_read_services():
    # SAFETY INVARIANT: the scanner runs on other people's vehicles and must never
    # transmit a write. This pins the allowlist to the four OBD READ services
    # (01 current data, 03 stored DTCs, 09 vehicle info, 22 ReadDataByIdentifier).
    # If this fails, someone widened the allowlist — do NOT "fix" it by editing this
    # test; a write service in ALLOWED_MODES breaks the read-only-by-construction guarantee.
    from obd_scan import catalog as cat
    assert cat.ALLOWED_MODES == frozenset({0x01, 0x03, 0x09, 0x22})


def test_presets_never_sweep_chassis_modules():
    # SAFETY: a powertrain scan must never probe 7E3-7E7. On a 2018 Audi Q5, 7E4
    # is a driver-assist (ADAS) module and reading it tripped pre-sense warnings.
    for name, preset in cat.PRESETS.items():
        names = {h.name for h in preset.headers}
        bad = names & {"7E3", "7E4", "7E5", "7E6", "7E7"}
        assert not bad, f"preset {name} sweeps chassis/ADAS module(s) {bad}"


def test_preset_for_vin_maps_wmi():
    assert cat.preset_for_vin("WAU0123456789ABCD") == "audi"
    assert cat.preset_for_vin("1FT0123456789ABCD") == "ford"
    assert cat.preset_for_vin("1GT0123456789ABCD") == "gm"
    assert cat.preset_for_vin("WBA0123456789ABCD") == "bmw"
    assert cat.preset_for_vin("wau0123456789abcd") == "audi"   # case-insensitive


def test_preset_for_vin_unknown_is_none():
    assert cat.preset_for_vin("JHM0123456789ABCD") is None     # Honda — not mapped
    assert cat.preset_for_vin("") is None
    assert cat.preset_for_vin("WA") is None
    assert cat.preset_for_vin(None) is None


def test_all_known_headers_includes_preset_headers():
    # run_log reconstructs a hit's header by name from this set; it must include
    # the base pools AND preset-specific headers (BMW's extended 612/618), or
    # logging a BMW drive crashes.
    names = {h.name for h in cat.all_known_headers()}
    for n in ("7DF", "7E0", "7E1", "BMW-612", "BMW-618"):
        assert n in names, f"{n} missing from all_known_headers()"


def test_parse_vin_from_payload():
    vin = "WAU0123456789ABCD"
    # classify() strips '49 02'; payload = count byte (0x01) + 17 ASCII VIN bytes
    assert cat.parse_vin_from_payload(b"\x01" + vin.encode("ascii")) == vin
    # bare 17 bytes (no count) also parse
    assert cat.parse_vin_from_payload(vin.encode("ascii")) == vin
    # too short / empty / non-ASCII -> None
    assert cat.parse_vin_from_payload(b"") is None
    assert cat.parse_vin_from_payload(b"\x01\x02\x03") is None
    assert cat.parse_vin_from_payload(b"\xff" * 17) is None


def test_jeep_preset_targets_the_29bit_tcm_not_11bit():
    """The Wagoneer's transmission answers on 29-bit DA18F1, not 11-bit 7E1.

    A captured 2024 Wagoneer lists 7E1.7E9.2204FE as UNSUPPORTED, so the Grand
    Cherokee's 11-bit ATF path does NOT transfer. Getting this backwards would
    make the whole scan silently return nothing from the transmission, which is
    the one module the scan exists to reach.
    """
    preset = cat.PRESETS["jeep"]
    names = {h.name for h in preset.headers}
    assert "18DA18F1" in names, "TCM must be reachable on the 29-bit path"
    assert "7E1" not in names, "11-bit 7E1 is confirmed dead on the Wagoneer"
    tcm = next(h for h in preset.headers if h.name == "18DA18F1")
    assert tcm.at_sp == "7" and tcm.at_cp == "18"      # 29-bit/500k needs both


def test_jeep_preset_excludes_non_powertrain_modules():
    """DAC7 (tire pressure) and DA30 (wheel speeds / lateral G) are alive on the
    vehicle but are not powertrain. Same rule that keeps 7E3-7E7 out of the
    11-bit pool after the Audi 7E4 pre-sense incident."""
    names = {h.name for h in cat.PRESETS["jeep"].headers}
    assert "18DAC7F1" not in names
    assert "18DA30F1" not in names


def test_jeep_sweeps_the_block_holding_the_atf_candidate():
    """2204FE is the reason this preset exists — the sweep must actually cover it."""
    reqs = {r for b in cat.PRESETS["jeep"].blocks for r in b.requests()}
    assert "2204FE" in reqs        # ATF temp candidate (verified on Grand Cherokee)
    assert "22051A" in reqs        # gear, verified on a 2024 Wagoneer


def test_jeep_wmi_resolves_for_auto_detect():
    assert cat.preset_for_vin("1C40123456789ABCD") == "jeep"
