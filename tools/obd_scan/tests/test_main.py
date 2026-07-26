import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from obd_scan import catalog as cat
from obd_scan.__main__ import _rehydrate_alive_headers


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
