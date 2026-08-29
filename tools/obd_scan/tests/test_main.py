import json
import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from obd_scan import catalog as cat
from obd_scan.__main__ import _blocks_from_discover, _rehydrate_alive_headers


def test_rehydrate_includes_preset_specific_bmw_header():
    # Regression: a BMW census with a live 6F1 header (612/618) must NOT KeyError.
    # cmd_sweep used to rehydrate from the global pools (which lack 612/618); it
    # now resolves from the bmw preset's own declared headers.
    preset = cat.PRESETS["bmw"]
    census = {"headers": [
        {"alive": True,  "header": {"name": "7DF"}},
        {"alive": True,  "header": {"name": "BMW-618"}},
        {"alive": False, "header": {"name": "BMW-612"}},
    ]}
    rows, skipped = _rehydrate_alive_headers(census, preset)
    assert sorted(r.header.name for r in rows) == ["7DF", "BMW-618"]  # dead 612 dropped
    assert skipped == []


def test_rehydrate_rejects_undeclared_chassis_header():
    # Safety: a tampered/shared census that injects an ADAS module (7E4) the preset
    # never declared must be REFUSED, not swept -- the read-only MODE gate checks
    # the service byte, not the target ECU, so this is the target-side guard.
    preset = cat.PRESETS["audi"]           # 7DF/7E0/7E1 only
    census = {"headers": [
        {"alive": True, "header": {"name": "7E0"}},
        {"alive": True, "header": {"name": "7E4"}},   # injected ADAS module
    ]}
    rows, skipped = _rehydrate_alive_headers(census, preset)
    assert [r.header.name for r in rows] == ["7E0"]   # legit powertrain header kept
    assert skipped == ["7E4"]                          # injected module refused


def test_blocks_from_discover_rebuilds_sweepable_blocks(tmp_path):
    p = tmp_path / "discover.json"
    p.write_text(json.dumps({"blocks": [
        {"name": "2258xx", "prefix": 0x2258, "note": "discovered: answered at 7DF"},
    ]}))
    blocks = _blocks_from_discover(str(p))

    assert [b.name for b in blocks] == ["2258xx"]
    assert next(iter(blocks[0].requests())) == "225800"     # sweepable as-is


def test_blocks_from_discover_rejects_a_write_service(tmp_path):
    # discover.json is ordinary JSON on disk -- hand-editable, copyable between
    # machines, shareable -- so a prefix in it is exactly as untrusted as the
    # preset name in a shared census.json, which cmd_sweep already refuses to
    # take on faith. 0x2E58 would make the sweep emit 2E58xx: WriteDataByIdentifier,
    # the write twin of Mode 22. It must be refused before the link opens.
    p = tmp_path / "evil.json"
    p.write_text(json.dumps({"blocks": [{"name": "2E58xx", "prefix": 0x2E58}]}))

    with pytest.raises(cat.UnsafeRequest):
        _blocks_from_discover(str(p))


def test_blocks_from_discover_rejects_a_malformed_entry(tmp_path):
    p = tmp_path / "bad.json"
    p.write_text(json.dumps({"blocks": [{"name": "2258xx"}]}))    # no prefix

    with pytest.raises(SystemExit):
        _blocks_from_discover(str(p))
