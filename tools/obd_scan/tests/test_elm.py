import pathlib
import socket
import sys
import time

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from fake_elm import FakeElm, WedgedElm
from obd_scan import catalog as cat
from obd_scan.elm import AdapterUnreachable, ElmSession, diagnose_connect_error
from obd_scan.reply import Cls


@pytest.fixture
def server():
    fake = FakeElm({
        "ATZ": "ELM327 v2.3",
        "ATE0": "OK", "ATL0": "OK", "ATS0": "OK",
        "ATSP0": "OK", "ATSP6": "OK", "ATSP7": "OK", "ATCP18": "OK",
        "ATAT2": "OK", "ATST19": "OK",
        "ATSH7E0": "OK", "ATSH7E1": "OK", "ATSHDA10F1": "OK",
        "ATDP": "ISO 15765-4 (CAN 11/500)",
        "0100": "4100BE3EB811",
        "22F446": "62F4461A",
        "22F400": "7F2231",
    })
    host, port = fake.start()
    yield fake, host, port
    fake.stop()


def test_init_and_probe(server):
    fake, host, port = server
    s = ElmSession(host, port)
    s.connect()
    assert "ELM327" in s.init()
    r = s.probe("22F446")
    assert r.cls is Cls.POSITIVE and r.payload == bytes([0x1A])
    s.close()


def test_header_is_set_once_per_change(server):
    fake, host, port = server
    s = ElmSession(host, port)
    s.connect(); s.init()
    h = next(x for x in cat.HEADERS_11BIT if x.name == "7E0")
    s.set_header(h); s.probe("22F446")
    s.set_header(h); s.probe("22F400")      # same header — must not re-send AT SH
    assert fake.requests_seen.count("ATSH7E0") == 1
    s.close()


def test_29bit_header_sends_protocol_and_priority_in_order(server):
    # DEFECT 3: membership alone ("X in requests_seen") would still pass if
    # the commands were duplicated or sent in the wrong order. Order matters:
    # protocol must be selected before priority and header, or the adapter
    # addresses the wrong module and a wrong-module read silently attributes
    # findings to the wrong ECU.
    fake, host, port = server
    s = ElmSession(host, port)
    s.connect(); s.init()
    h = next(x for x in cat.HEADERS_29BIT if x.name == "18DA10F1")
    s.set_header(h)
    seen = fake.requests_seen
    assert seen.count("ATSP7") == 1
    assert seen.count("ATCP18") == 1
    assert seen.count("ATSHDA10F1") == 1
    i_sp, i_cp, i_sh = seen.index("ATSP7"), seen.index("ATCP18"), seen.index("ATSHDA10F1")
    assert i_sp < i_cp < i_sh
    s.close()


def test_set_header_after_detect_protocol_sends_atsp_once(server):
    # DEFECT 1: detect_protocol() used to stash the human-readable ATDP text
    # ("ISO 15765-4 (CAN 11/500)") in the same attribute set_header() compares
    # against the ATSP selector ("6"/"7"), so the two never matched and ATSP
    # was re-sent on every set_header() call. Two headers sharing an at_sp
    # must only cause one ATSP after a detect_protocol() call.
    fake, host, port = server
    s = ElmSession(host, port)
    s.connect(); s.init()
    s.detect_protocol()
    h0 = next(x for x in cat.HEADERS_11BIT if x.name == "7E0")
    h1 = next(x for x in cat.HEADERS_11BIT if x.name == "7E1")
    s.set_header(h0)
    s.set_header(h1)                    # same at_sp ("6") -- must not resend ATSP
    assert fake.requests_seen.count("ATSP6") == 1
    s.close()


def test_stop_without_closing_client_terminates_server_thread():
    # DEFECT 2: FakeElm.stop() used to close only the listening socket. The
    # accepted connection was never closed, so a server thread blocked in
    # conn.recv() (because the test failed before session.close()) leaked
    # forever. Closing the LISTENER has no effect on an already-accepted
    # connection's blocking recv() -- those are independent sockets.
    #
    # One request/reply round trip is exchanged first and deliberately NOT
    # followed by a second command or a close: this guarantees accept() has
    # already completed and the server thread is genuinely parked in its
    # second conn.recv() (waiting for a next line that never comes) --
    # exactly the state a failed test leaves behind. Without this round
    # trip the assertion would race accept() and could pass by accident.
    fake = FakeElm({"ATZ": "ELM327 v2.3"})
    host, port = fake.start()
    client = socket.create_connection((host, port), timeout=2.0)
    try:
        client.sendall(b"ATZ\r")
        client.settimeout(2.0)
        buf = b""
        while b">" not in buf:
            buf += client.recv(256)
        # The server thread loops back to check its stop-flag before each
        # recv() -- give it a moment to land inside that recv() (there is
        # nothing else for it to do) so stop() genuinely races a blocked
        # recv(), not an idle iteration boundary.
        time.sleep(0.05)
        fake.stop()                      # deliberately no session.close(), no further data
        fake._thread.join(timeout=2.0)
        assert not fake._thread.is_alive()
    finally:
        client.close()


def test_promptless_reply_is_retried_not_accepted(server):
    # DEFECT 4: _read_to_prompt() returned accumulated text indistinguishably
    # whether or not the '>' prompt was actually seen, so a reply truncated
    # by deadline expiry or peer close flowed into classify() as if complete.
    # drop_prompt_once withholds the prompt on the FIRST "22F446" reply --
    # the fixed probe() must retry (treating "no prompt seen" like an
    # ELM_ERROR) rather than silently accepting whatever text arrived.
    fake = FakeElm({
        "ATZ": "ELM327 v2.3",
        "ATE0": "OK", "ATL0": "OK", "ATS0": "OK",
        "ATAT2": "OK", "ATST19": "OK",
        "22F446": "62F4461A",
    }, drop_prompt_once={"22F446"})
    host, port = fake.start()
    s = ElmSession(host, port, probe_timeout=0.2)
    s.connect(); s.init()
    r = s.probe("22F446")
    assert fake.requests_seen.count("22F446") == 2   # first attempt retried
    assert r.cls is Cls.POSITIVE and r.payload == bytes([0x1A])
    s.close()
    fake.stop()


def test_negative_response_is_classified_not_discarded(server):
    fake, host, port = server
    s = ElmSession(host, port); s.connect(); s.init()
    r = s.probe("22F400")
    assert r.cls is Cls.NRC_OUT_OF_RANGE      # proves the module is alive
    s.close()


def test_wedged_link_is_reported_as_error_not_a_clean_no_data():
    # CRITICAL 1: a peer that accepts the connection, reads forever, and
    # NEVER replies (a wedged WiFi ELM327) must not be trusted just because
    # the RETRY's promptless reply also happens to classify() as NO_DATA --
    # classify("") is indistinguishable from a genuine non-answer. Two
    # consecutive reads with no prompt seen must be attributed to the LINK,
    # not reported as a clean silence. Deterministic: bounded by
    # probe_timeout, no sleeps.
    fake = WedgedElm()
    host, port = fake.start()
    s = ElmSession(host, port, probe_timeout=0.05)
    s.connect()
    r = s.probe("22F446")
    assert r.cls is Cls.ELM_ERROR
    s.close()
    fake.stop()


def test_connect_failure_raises_adapter_unreachable_not_a_bare_oserror():
    # The whole point of AdapterUnreachable: an unreachable adapter used to
    # escape main() as a raw ConnectionRefusedError and print a Python
    # traceback, which says nothing about WiFi, SoftAPs or ports. Bind a
    # socket, close it, and connect to that now-dead port.
    probe = socket.socket(); probe.bind(("127.0.0.1", 0))
    host, port = probe.getsockname()
    probe.close()
    s = ElmSession(host, port, timeout=1.0)
    with pytest.raises(AdapterUnreachable) as e:
        s.connect()
    # The message must name the endpoint tried and stay actionable.
    assert f"{host}:{port}" in str(e.value)
    assert "--port" in str(e.value)


def test_silent_adapter_is_diagnosed_at_init_not_scanned_as_a_dead_vehicle():
    # The nastiest failure mode, because it does NOT crash: a peer that
    # accepts TCP and never speaks ELM327 (wedged adapter, or a phone app
    # holding the adapter's single client slot) let init() return "" and the
    # census then ran to completion reporting every header undetermined --
    # a link fault dressed up as a finding about the vehicle. Fail loudly at
    # the point the adapter first fails to answer instead.
    fake = WedgedElm()
    host, port = fake.start()
    s = ElmSession(host, port, probe_timeout=0.05, reset_timeout=0.05)
    s.connect()
    with pytest.raises(AdapterUnreachable) as e:
        s.init()
    assert "never answered" in str(e.value)
    s.close()
    fake.stop()


def test_diagnosis_distinguishes_the_three_connect_failure_modes():
    # Each failure has a DIFFERENT field fix, so a generic "cannot connect"
    # would be useless: timeout => wrong WiFi, refused => wrong port,
    # gaierror => --host isn't an IP. Pure function, no socket needed.
    timed_out = diagnose_connect_error(TimeoutError("timed out"), "192.168.0.10", 35000, 5.0)
    refused = diagnose_connect_error(ConnectionRefusedError(111, "refused"), "192.168.0.10", 35000, 5.0)
    dns = diagnose_connect_error(socket.gaierror(-2, "Name or service not known"), "v-link", 35000, 5.0)

    assert "WiFi" in timed_out and "V-LINK" in timed_out
    assert "refused" in refused.lower() and "--port" in refused
    assert "resolve" in dns and "IP address" in dns
    # No two diagnoses may collapse into the same text.
    assert len({timed_out, refused, dns}) == 3


def test_cmd_itself_validates_before_transmitting(server):
    # IMPORTANT 3: cmd() validates before sendall(), but the only prior
    # safety test called probe(), which validates FIRST and then calls
    # cmd() -- deleting BOTH validation calls inside cmd() left all tests
    # green while set_header(), init(), and detect_protocol() call cmd()
    # directly and would transmit unvalidated. This test calls cmd() itself.
    fake, host, port = server
    s = ElmSession(host, port); s.connect(); s.init()
    before = list(fake.requests_seen)
    for bad in ("2EF190AA", "0100\r2EF190AA"):
        with pytest.raises(cat.UnsafeRequest):
            s.cmd(bad)                       # cmd(), NOT probe()
    with pytest.raises(cat.UnsafeRequest):
        s.cmd("ATPP0CSV01")
    assert fake.requests_seen == before
    s.close()


def test_unsafe_request_never_reaches_the_socket(server):
    fake, host, port = server
    s = ElmSession(host, port); s.connect(); s.init()
    before = list(fake.requests_seen)
    with pytest.raises(cat.UnsafeRequest):
        s.probe("2EF190AA")                    # write data by identifier
    assert fake.requests_seen == before        # nothing transmitted
    s.close()


def test_set_header_bmw_emits_cea_and_cra():
    fake = FakeElm({"ATSP6": "OK", "ATSH6F1": "OK", "ATCEA18": "OK", "ATCRA618": "OK"})
    host, port = fake.start()
    try:
        s = ElmSession(host=host, port=port)
        s.connect()
        s.set_header(next(h for h in cat.BMW_HEADERS if h.name == "BMW-618"))
        seen = fake.requests_seen
        # extended-address target and RX filter are both set, after the header.
        assert "ATSH6F1" in seen and "ATCEA18" in seen and "ATCRA618" in seen
        assert seen.index("ATCEA18") > seen.index("ATSH6F1")
    finally:
        s.close(); fake.stop()


def test_set_header_standard_clears_extended_addressing():
    # A standard (Mode-01) header AFTER a BMW-enhanced one must clear the stale
    # extended addressing AND the RX filter, or Mode-01 replies (from 7E8) get
    # blocked by the 618 filter. Clearing happens ONLY on this transition.
    fake = FakeElm({"ATSP6": "OK", "ATSH6F1": "OK", "ATCEA18": "OK", "ATCRA618": "OK",
                    "ATSH7DF": "OK", "ATCEA": "OK", "ATCRA": "OK"})
    host, port = fake.start()
    try:
        s = ElmSession(host=host, port=port)
        s.connect()
        s.set_header(next(h for h in cat.BMW_HEADERS if h.name == "BMW-618"))  # ext ON
        s.set_header(cat.BMW_STD)          # standard after BMW → clears ext + filter
        seen = fake.requests_seen
        assert "ATCEA" in seen and "ATCRA" in seen               # the bare (off) forms
        assert seen.index("ATCEA") > seen.index("ATSH7DF")       # cleared after the std select
    finally:
        s.close(); fake.stop()


def test_set_header_gm_never_emits_extended_addressing():
    # GM/Ford headers never turn extended addressing on, so set_header must send
    # NO ATCEA/ATCRA for them (regression guard for the census sendall budget).
    fake = FakeElm({"ATSP6": "OK", "ATSH7E0": "OK"})
    host, port = fake.start()
    try:
        s = ElmSession(host=host, port=port)
        s.connect()
        s.set_header(next(h for h in cat.HEADERS_11BIT if h.name == "7E0"))
        seen = fake.requests_seen
        assert not any(r.startswith("ATCEA") or r.startswith("ATCRA") for r in seen)
    finally:
        s.close(); fake.stop()
