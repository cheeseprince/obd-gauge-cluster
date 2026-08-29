import argparse
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


# --- transport selection: WiFi stays the default, --ble opts in --------------

def test_open_transport_is_a_passthrough_without_ble():
    """The WiFi path must be byte-identical to what it was before --ble existed."""
    from obd_scan.__main__ import _open_transport
    args = argparse.Namespace(host="192.168.4.1", port=35000, ble=None, ble_addr=None)
    assert _open_transport(args) == ("192.168.4.1", 35000)


def test_open_transport_refuses_ble_and_an_explicit_host_together():
    """Two flags naming two different adapters is a contradiction, not a priority
    question. Silently picking one is how you scan the wrong thing and trust it."""
    from obd_scan.__main__ import _DEFAULT_HOST, _open_transport
    args = argparse.Namespace(host="192.168.4.1", port=35000, ble=True,
                              ble_addr=None, ble_scan_timeout=10.0)
    with pytest.raises(SystemExit) as e:
        _open_transport(args)
    assert "only one" in str(e.value)
    # ...but the UNTOUCHED default is not an explicit host, so this is allowed.
    args.host = _DEFAULT_HOST
    args.ble = None
    args.ble_addr = None
    assert _open_transport(args) == (_DEFAULT_HOST, 35000)


def test_open_transport_starts_the_bridge_and_returns_its_port(monkeypatch):
    from obd_scan import ble_bridge
    from obd_scan.__main__ import _DEFAULT_HOST, _open_transport

    seen = {}

    def fake_start(name=None, addr=None, scan_timeout=10.0, **kw):
        seen.update(name=name, addr=addr, scan_timeout=scan_timeout)
        return ("127.0.0.1", 46001)

    monkeypatch.setattr(ble_bridge, "start", fake_start)
    args = argparse.Namespace(host=_DEFAULT_HOST, port=35000, ble="vlinker",
                              ble_addr=None, ble_scan_timeout=7.5)
    assert _open_transport(args) == ("127.0.0.1", 46001)
    assert seen == {"name": "vlinker", "addr": None, "scan_timeout": 7.5}


def test_bare_ble_flag_means_best_ranked_not_a_name_match(monkeypatch):
    """`--ble` with no value must not be passed on as the literal string 'True'."""
    from obd_scan import ble_bridge
    from obd_scan.__main__ import _DEFAULT_HOST, _open_transport

    seen = {}
    monkeypatch.setattr(ble_bridge, "start",
                        lambda name=None, **kw: seen.update(name=name) or ("127.0.0.1", 1))
    args = argparse.Namespace(host=_DEFAULT_HOST, port=35000, ble=True,
                              ble_addr=None, ble_scan_timeout=10.0)
    _open_transport(args)
    assert seen["name"] is None


def test_a_bring_up_failure_becomes_a_sentence_not_a_traceback(monkeypatch):
    from obd_scan import ble_bridge
    from obd_scan.__main__ import _DEFAULT_HOST, _open_transport

    def boom(**kw):
        raise ble_bridge.NoAdapterFound("nothing answered the scan")

    monkeypatch.setattr(ble_bridge, "start", boom)
    args = argparse.Namespace(host=_DEFAULT_HOST, port=35000, ble=True,
                              ble_addr=None, ble_scan_timeout=10.0)
    with pytest.raises(SystemExit) as e:
        _open_transport(args)
    assert "nothing answered the scan" in str(e.value)
def test_every_text_file_open_names_its_encoding():
    """No text I/O may inherit the locale's default encoding.

    Python picks the LOCALE default for text mode when `encoding` is omitted:
    UTF-8 on Linux and macOS, but cp1252 on a stock Windows install. The scan
    report contains '→' and '°', neither of which cp1252 can encode, so
    `Path.write_text()` there raised UnicodeEncodeError -- at the very last step
    of a real scan, after the drive was already done.

    This is a whole class of bug, not one line, so it is guarded as a class:
    every text-mode open and write_text in the package must say what encoding it
    means. Binary mode is exempt -- it has no encoding to name.
    """
    import re

    pkg = pathlib.Path(__file__).resolve().parents[1]
    offenders = []
    for py in sorted(pkg.glob("*.py")):
        for n, line in enumerate(py.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("#", 1)[0]
            if "encoding=" in code:
                continue
            if re.search(r"\bopen\(", code) and not re.search(r'"[rwax]b"|\'[rwax]b\'', code):
                offenders.append(f"{py.name}:{n}: {line.strip()}")
            elif re.search(r"\.write_text\(|\.read_text\(", code):
                offenders.append(f"{py.name}:{n}: {line.strip()}")
    assert not offenders, "text I/O without an explicit encoding:\n" + "\n".join(offenders)
