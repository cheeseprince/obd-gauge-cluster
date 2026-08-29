"""Host tests for the BLE->TCP bridge.

The GATT discovery itself needs hardware, so the module is split to keep the
untestable part as small as possible: ranking, profile binding and chunking are
pure, and serve() takes a duck-typed client so the whole byte path can run
against a stub. Nothing here imports bleak.
"""
import asyncio
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from obd_scan.ble_bridge import PROFILES, bind_profile, chunk, looks_like_obd, rank_devices, serve

VLINKER_SVC, VLINKER_NTF, VLINKER_WR = PROFILES[0][1], PROFILES[0][2], PROFILES[0][3]


def test_name_hints_match_case_insensitively():
    assert looks_like_obd("vLinker MS-B")
    assert looks_like_obd("OBDLink CX")
    assert not looks_like_obd("Aria-Scale-7F")
    assert not looks_like_obd("")
    assert not looks_like_obd(None)


def test_service_match_outranks_a_stronger_named_device():
    # The weak device advertises our service; the strong one only has the name.
    devices = [
        ("AA:00", "Some Gadget", [VLINKER_SVC], -90),
        ("BB:00", "vlinker clone", [], -40),
    ]
    ranked = rank_devices(devices)
    assert ranked[0][3] == "AA:00", "a service-UUID match must beat a name match"


def test_name_match_outranks_a_stronger_silent_device():
    devices = [
        ("AA:00", "", [], -30),
        ("BB:00", "vLinker MS-B", [], -80),
    ]
    assert rank_devices(devices)[0][3] == "BB:00"


def test_rssi_breaks_ties_within_a_tier():
    devices = [
        ("AA:00", "elm327", [], -80),
        ("BB:00", "elm327", [], -50),
    ]
    assert rank_devices(devices)[0][3] == "BB:00"


def test_bind_profile_prefers_write_without_response():
    chars = {VLINKER_NTF: ["notify"], VLINKER_WR: ["write", "write-without-response"]}
    label, ntf, wr, needs_response = bind_profile(chars)
    assert label == "vlinker 18f0"
    assert (ntf, wr) == (VLINKER_NTF, VLINKER_WR)
    assert needs_response is False


def test_bind_profile_falls_back_to_write_with_response():
    chars = {VLINKER_NTF: ["notify"], VLINKER_WR: ["write"]}
    assert bind_profile(chars)[3] is True


def test_bind_profile_handles_ffe0_single_characteristic():
    # ffe0 clones use ONE characteristic for both directions.
    ffe0 = [p for p in PROFILES if p[0] == "clone ffe0"][0]
    chars = {ffe0[2]: ["notify", "write-without-response"]}
    label, ntf, wr, _ = bind_profile(chars)
    assert label == "clone ffe0" and ntf == wr


def test_bind_profile_returns_none_for_a_stranger():
    assert bind_profile({"0000feed-0000-1000-8000-00805f9b34fb": ["read"]}) is None


def test_chunk_splits_at_the_mtu_and_never_yields_empty():
    assert chunk(b"abcdef", 2) == [b"ab", b"cd", b"ef"]
    assert chunk(b"", 20) == [b""]
    assert chunk(b"abc", 0) == [b"a", b"b", b"c"]      # size is clamped to >= 1


class StubClient:
    """Minimal duck-typed BleakClient: records writes, answers on a terminator."""

    def __init__(self):
        self.is_connected = True
        self.writes = []
        self._cb = None

    async def start_notify(self, _uuid, cb):
        self._cb = cb

    async def write_gatt_char(self, _uuid, data, response=False):
        self.writes.append(bytes(data))
        if b"\r" in bytes(data):
            self._cb(0, bytearray(b"41 00 BE 3E B8 11\r\r>"))


def _free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_bytes_flow_both_ways_and_long_writes_are_chunked():
    port = _free_port()
    stub = StubClient()

    async def scenario():
        task = asyncio.create_task(
            serve(stub, VLINKER_NTF, VLINKER_WR, False, "127.0.0.1", port,
                  mtu=23, log=lambda *_: None))
        await asyncio.sleep(0.2)
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        cmd = b"AT SH 6F1 " + b"X" * 40 + b"\r"
        writer.write(cmd)
        await writer.drain()
        reply = await asyncio.wait_for(reader.read(64), timeout=3)
        writer.close()
        task.cancel()
        return cmd, reply

    cmd, reply = asyncio.run(scenario())
    assert reply.startswith(b"41 00"), "notify never reached the TCP client"
    assert b"".join(stub.writes) == cmd, "chunking must not alter the byte stream"
    assert max(len(w) for w in stub.writes) <= 20, "a write exceeded the MTU payload"


def test_second_client_is_refused():
    port = _free_port()
    stub = StubClient()

    async def scenario():
        task = asyncio.create_task(
            serve(stub, VLINKER_NTF, VLINKER_WR, False, "127.0.0.1", port,
                  mtu=23, log=lambda *_: None))
        await asyncio.sleep(0.2)
        r1, w1 = await asyncio.open_connection("127.0.0.1", port)
        await asyncio.sleep(0.1)
        r2, _w2 = await asyncio.open_connection("127.0.0.1", port)
        await asyncio.sleep(0.2)
        second = await r2.read(16)
        w1.close()
        task.cancel()
        return second

    assert asyncio.run(scenario()) == b"", "a second client must be refused"


# --- in-process bridge: the path `obd_scan --ble` uses -----------------------
# These cover the seam, not the radio. GATT discovery still needs hardware; what
# is testable is that a caller gets a usable port, gets the real failure when
# the adapter does not come up, and never gets a port it cannot connect to.

def _join_bridge_threads(timeout=3.0):
    """Wait for any bridge thread this test started to actually exit.

    Not tidiness: a live daemon thread makes the whole pytest process
    multi-threaded, and a later test that forks (test_correlate uses
    multiprocessing) then trips a DeprecationWarning that is not its fault and
    that varies with collection order. Joining here keeps the leak inside the
    test that caused it.
    """
    import threading
    for t in threading.enumerate():
        if t.name == "ble-bridge":
            t.join(timeout)


def test_serve_reports_the_port_it_actually_bound(monkeypatch):
    """port=0 asks the OS for a free port, so the caller must be told which one.

    Echoing the requested port back would hand out ':0' -- a port nothing can
    connect to -- which is the whole reason `ready` exists.
    """

    got = {}
    client = StubClient()

    async def drive():
        task = asyncio.ensure_future(
            serve(client, VLINKER_NTF, VLINKER_WR, False, "127.0.0.1", 0,
                  mtu=23, log=lambda *a: None,
                  ready=lambda h, p: got.update(host=h, port=p)))
        for _ in range(200):                      # yield until the server binds
            if got:
                break
            await asyncio.sleep(0.01)
        task.cancel()

    asyncio.run(drive())
    assert got["host"] == "127.0.0.1"
    assert got["port"] != 0                       # a REAL port, not the request
    assert 1024 < got["port"] < 65536


def test_start_returns_the_address_once_the_bridge_is_listening(monkeypatch):
    from obd_scan import ble_bridge as bb

    async def fake_run(args, ready=None):
        ready(args.host, 45999)                   # stand in for a bound server
        # Short, not "forever": a lingering daemon thread makes the whole test
        # process multi-threaded, and a later test that forks (correlate uses
        # multiprocessing) then trips a DeprecationWarning that is not its fault.
        await asyncio.sleep(0.2)

    monkeypatch.setattr(bb, "_run", fake_run)
    assert bb.start(name="vlinker", log=lambda *a: None) == ("127.0.0.1", 45999)
    _join_bridge_threads()


def test_start_reraises_the_bring_up_failure_rather_than_timing_out():
    """A dead adapter must surface as its own diagnosis, immediately.

    The failure mode this guards against is the bridge thread dying while the
    caller sits out the full connect timeout and then reports something vague.
    """
    import pytest
    from obd_scan import ble_bridge as bb

    async def fake_run(args, ready=None):
        raise bb.NoAdapterFound("nothing answered")

    bb_run = bb._run
    bb._run = fake_run
    try:
        with pytest.raises(bb.NoAdapterFound):
            bb.start(connect_timeout=5.0, log=lambda *a: None)
    finally:
        bb._run = bb_run
        _join_bridge_threads()


def test_start_times_out_with_an_actionable_message():
    import pytest
    from obd_scan import ble_bridge as bb

    async def never_ready(args, ready=None):
        await asyncio.sleep(0.5)          # outlives the 0.2s timeout, then exits

    bb_run = bb._run
    bb._run = never_ready
    try:
        with pytest.raises(bb.BleBridgeError) as e:
            bb.start(connect_timeout=0.2, log=lambda *a: None)
        assert "powered and in range" in str(e.value)
    finally:
        bb._run = bb_run
        _join_bridge_threads()
