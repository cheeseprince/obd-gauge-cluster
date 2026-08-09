import csv
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from fake_elm import FakeElm, WedgedElm
from obd_scan import catalog as cat
from obd_scan.elm import ElmSession
from obd_scan.stages import (
    Hit,
    default_session_gate,
    estimate_samples_per_pid,
    filter_hits_by_pids,
    probe_bmw_capability,
    run_census,
    run_log,
    run_sweep,
)


def test_filter_hits_by_pids():
    hits = [Hit("7E0", "2220A1", "", "", 0), Hit("7E1", "222104", "", "", 0), Hit("7E0", "221135", "", "", 0)]
    kept, missing = filter_hits_by_pids(hits, "2220A1,222104")
    assert {h.request for h in kept} == {"2220A1", "222104"}
    assert missing == []
    # case-insensitive; reports requested-but-absent DIDs
    kept, missing = filter_hits_by_pids(hits, "2220a1,DEADBE")
    assert [h.request for h in kept] == ["2220A1"]
    assert missing == ["DEADBE"]
    # empty pids keeps everything
    kept, missing = filter_hits_by_pids(hits, "")
    assert len(kept) == 3 and missing == []


def test_estimate_samples_per_pid():
    few = estimate_samples_per_pid(20)     # focused drive
    many = estimate_samples_per_pid(400)   # whole sweep
    assert few > many > 0                  # fewer PIDs -> more samples each
    assert 400 < few < 1500                # ~750 for 20 polls @ 60ms over 15 min

BASE = {
    "ATZ": "ELM327 v2.3", "ATE0": "OK", "ATL0": "OK", "ATS0": "OK",
    "ATAT2": "OK", "ATST19": "OK", "ATSP0": "OK", "ATSP6": "OK",
    "ATSP7": "OK", "ATCP18": "OK", "ATDP": "ISO 15765-4 (CAN 11/500)",
}


def _session(extra):
    fake = FakeElm({**BASE, **extra})
    host, port = fake.start()
    s = ElmSession(host, port)
    s.connect(); s.init()
    return fake, s


def test_census_marks_positive_header_alive():
    fake, s = _session({"ATSH7E0": "OK", "0100": "4100BE3EB811", "220005": "620005AA"})
    res = run_census(s, cat.PRESETS["gm"], headers=[h for h in cat.HEADERS_11BIT if h.name == "7E0"])
    row = res["headers"][0]
    assert row.alive is True and row.evidence == "positive"
    s.close(); fake.stop()


def test_census_treats_negative_response_as_alive():
    # Module answers 7F 22 31 to everything: no data, but definitively present.
    fake, s = _session({"ATSH7E1": "OK", "0100": "7F2231", "220005": "7F2231"})
    res = run_census(s, cat.PRESETS["gm"], headers=[h for h in cat.HEADERS_11BIT if h.name == "7E1"])
    row = res["headers"][0]
    assert row.alive is True and row.evidence == "negative"
    s.close(); fake.stop()


def test_census_marks_silent_header_dead():
    fake, s = _session({"ATSH7E5": "OK"})     # everything else -> NO DATA
    res = run_census(s, cat.PRESETS["gm"], headers=[h for h in cat.HEADERS_11BIT if h.name == "7E5"])
    assert res["headers"][0].alive is False
    s.close(); fake.stop()


def test_census_decodes_supported_pid_bitmap():
    fake, s = _session({"ATSH7E0": "OK", "0100": "4100BE3EB811", "220005": "NO DATA"})
    res = run_census(s, cat.PRESETS["gm"], headers=[h for h in cat.HEADERS_11BIT if h.name == "7E0"])
    pids = res["headers"][0].supported_pids
    assert 0x0C in pids and 0x0D in pids       # RPM and speed present in BE3EB811
    s.close(); fake.stop()


class _DyingSocket:
    """Wraps a real, already-connected socket so the first `alive_calls`
    sendall() calls behave normally and every call after that raises OSError
    -- simulates a link that drops mid-census, deterministically and without
    any thread racing or sleeps (recv/settimeout/close pass straight through
    to the real socket so a still-open connection keeps working right up
    until the injected failure)."""

    def __init__(self, real_sock, alive_calls: int):
        self._real = real_sock
        self._alive_calls = alive_calls
        self._calls = 0

    def sendall(self, data):
        self._calls += 1
        if self._calls > self._alive_calls:
            raise OSError("simulated link failure")
        return self._real.sendall(data)

    def recv(self, n):
        return self._real.recv(n)

    def settimeout(self, t):
        return self._real.settimeout(t)

    def close(self):
        return self._real.close()


def test_census_aborts_when_set_header_fails_partway():
    # First header's full exchange (ATSP6, ATSH7E0, 0100, 220005 = 4 sends)
    # succeeds; the very next send -- ATSH7E1, i.e. set_header() for the
    # SECOND header -- hits the dead link. This is symptom #1: a socket
    # failure in set_header() must not be swallowed and must not let
    # subsequent headers be probed at all.
    #
    # 7E0 answers NEGATIVE (7F 22 31) rather than positive so the count of
    # sendall() calls stays exactly 4 and deterministic -- a positive reply
    # would trigger the extra supported-PID-bitmap walk and consume the
    # "budget" meant to carry us to the second header's ATSH.
    fake, s = _session({
        "ATSH7E0": "OK", "0100": "7F2231", "220005": "7F2231",
        "ATSH7E1": "OK",
    })
    headers = [h for h in cat.HEADERS_11BIT if h.name in ("7E0", "7E1")]
    s.sock = _DyingSocket(s.sock, alive_calls=4)
    res = run_census(s, cat.PRESETS["gm"], headers=headers)

    assert res["aborted"] is True
    assert res["error"]                                   # non-empty reason

    names = [r.header.name for r in res["headers"]]
    assert "7E0" in names                                  # probed before the link died
    row0 = next(r for r in res["headers"] if r.header.name == "7E0")
    assert row0.alive is True and row0.evidence == "negative"

    assert "7E1" not in res["alive"]
    # 7E1 was where the failure happened, and run_census() always appends the
    # in-progress row before breaking -- so it must be present, and must read
    # "error", never "silent" (that would misreport it as a probed-and-absent
    # module rather than an unreached one). ALSO (final review): this used
    # to be gated behind `if "7E1" in names:`, which made the assertion
    # vacuously pass if the row were ever dropped entirely instead of
    # correctly marked -- assert unconditionally.
    assert "7E1" in names
    row1 = next(r for r in res["headers"] if r.header.name == "7E1")
    assert row1.evidence == "error" and row1.alive is False

    s.close(); fake.stop()


def test_census_aborts_when_probe_fails_not_just_set_header():
    # Same underlying fault (OSError on sendall), but this time it happens
    # during probe() rather than set_header() -- symptom #2: identical fault,
    # previously opposite outcome (this one used to crash run_census outright
    # instead of being swallowed). ATSP6 + ATSH7E0 succeed (2 sends); the
    # first probe ("0100") is where the link dies.
    fake, s = _session({"ATSH7E0": "OK", "0100": "4100BE3EB811", "220005": "620005AA"})
    headers = [h for h in cat.HEADERS_11BIT if h.name == "7E0"]
    s.sock = _DyingSocket(s.sock, alive_calls=2)
    res = run_census(s, cat.PRESETS["gm"], headers=headers)

    assert res["aborted"] is True
    assert res["error"]
    row0 = res["headers"][0]
    assert row0.evidence == "error"
    assert row0.alive is False
    assert row0.header.name not in res["alive"]

    s.close(); fake.stop()


def test_census_marks_persistent_elm_error_as_error_not_silent():
    # The adapter answers a transport-fault string to every probe on this
    # header. ElmSession.probe() retries once internally, so this is a
    # SECOND consecutive ELM_ERROR after the retry already happened --
    # symptom #3: this must not fall through to "silent", which would be
    # indistinguishable from a genuinely absent module.
    fake, s = _session({"ATSH7E2": "OK", "0100": "CAN ERROR", "220005": "CAN ERROR"})
    res = run_census(s, cat.PRESETS["gm"], headers=[h for h in cat.HEADERS_11BIT if h.name == "7E2"])
    row = res["headers"][0]
    assert row.evidence == "error"
    assert row.alive is False
    assert row.header.name not in res["alive"]
    s.close(); fake.stop()


def test_census_silent_header_regression_still_evidence_silent():
    # Regression guard: a clean link where the adapter genuinely answers
    # "NO DATA" for every probe must still be reported as plain "silent" --
    # not collateral damage of adding the "error" class.
    fake, s = _session({"ATSH7E5": "OK"})     # everything else -> NO DATA
    res = run_census(s, cat.PRESETS["gm"], headers=[h for h in cat.HEADERS_11BIT if h.name == "7E5"])
    row = res["headers"][0]
    assert row.evidence == "silent"
    assert row.alive is False
    assert res["aborted"] is False
    assert res["error"] is None
    s.close(); fake.stop()


def test_census_wedged_link_never_reads_as_clean_silence():
    # CRITICAL 1, demonstrated end-to-end through run_census: a link that
    # accepts, reads, and never replies must produce evidence="error" on
    # the header being probed when the wedge is hit, never "silent" (which
    # census/report define as "a clean link that got a clean non-answer").
    # "alive" must stay empty AND the caller must be able to tell this
    # apart from a genuinely absent module -- that is exactly what
    # evidence="error" is for. Deterministic: bounded by probe_timeout, no
    # sleeps, no OSError needed (the wedge never raises -- sendall()
    # succeeds against the kernel buffer, only the read side is silent).
    fake = WedgedElm()
    host, port = fake.start()
    s = ElmSession(host, port, probe_timeout=0.05)
    s.connect()
    headers = [h for h in cat.HEADERS_11BIT if h.name == "7E0"]
    res = run_census(s, cat.PRESETS["gm"], headers=headers)

    row = res["headers"][0]
    assert row.evidence == "error"
    assert row.alive is False
    assert res["alive"] == []

    s.close(); fake.stop()


def test_sweep_records_only_real_hits():
    responses = {**BASE, "ATSH7E0": "OK", "0100": "4100BE3EB811", "220005": "620005AA"}
    responses["220041"] = "620041DEAD"          # a genuine hit
    responses["220042"] = "7F2231"              # module alive, PID absent
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host, port); s.connect(); s.init()

    census = run_census(s, cat.PRESETS["gm"],
                        headers=[h for h in cat.HEADERS_11BIT if h.name == "7E0"])
    blocks = [cat.Block("2200xx", 0x2200, lo=0x40, hi=0x43)]
    res = run_sweep(s, census, cat.PRESETS["gm"], blocks=blocks)

    hits = {h.request for h in res["hits"]}
    assert "220041" in hits
    assert "220042" not in hits                 # negative response is not a hit
    assert res["negatives"] >= 1                # but it IS counted
    s.close(); fake.stop()


def test_sweep_sets_header_once_per_block():
    # Guards the tool's main performance property: with ~256 probes per
    # block, calling set_header() once per PROBE instead of once per
    # (header, block) pair doubles the command count.
    #
    # Counting ATSH7E0 commands actually seen on the wire cannot catch that
    # regression: ElmSession.set_header() short-circuits with
    # `if self.cur_header == h.name: return` before sending anything, and
    # the preceding census already leaves cur_header == "7E0". So whether
    # run_sweep() calls set_header() once per block or once per probe, ZERO
    # further "ATSH7E0" strings reach the fake adapter either way -- the old
    # assertion (`after - before <= 1`) passed under both the correct
    # placement and the bug.
    #
    # Instead, spy on the set_header() METHOD CALL itself -- not the wire
    # command it may or may not emit -- and assert it is invoked exactly
    # once per (header, block) pair. That count differs (1 vs 16 here)
    # regardless of the callee's internal caching.
    responses = {**BASE, "ATSH7E0": "OK", "0100": "4100BE3EB811", "220005": "620005AA"}
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host, port); s.connect(); s.init()
    census = run_census(s, cat.PRESETS["gm"],
                        headers=[h for h in cat.HEADERS_11BIT if h.name == "7E0"])

    calls: list[str] = []
    real_set_header = s.set_header
    def _spy(h):
        calls.append(h.name)
        return real_set_header(h)
    s.set_header = _spy

    blocks = [cat.Block("2200xx", 0x2200, lo=0x40, hi=0x4F)]     # 16 probes
    run_sweep(s, census, cat.PRESETS["gm"], blocks=blocks)

    # 1 alive header x 1 block == exactly 1 call. If set_header() were
    # called per probe instead, this would be 16.
    assert len(calls) == 1
    s.close(); fake.stop()


class _FailOnCommand:
    """Wraps a real, already-connected socket so sendall() raises OSError the
    moment a specific command STRING is sent -- simulates a link that dies
    mid-block, deterministically. Unlike keying on a hand-derived sendall()
    call count (brittle: any change to the probe sequence shifts the count
    and silently breaks the test's intent), this triggers on the exact
    command text, so it stays correct and self-documenting no matter how
    many commands precede it. Every send before the trigger, and
    recv/settimeout/close always, pass straight through to the real socket."""

    def __init__(self, real_sock, trigger: str):
        self._real = real_sock
        self._trigger = (trigger.upper().replace(" ", "") + "\r").encode()

    def sendall(self, data):
        if data == self._trigger:
            raise OSError("simulated link failure")
        return self._real.sendall(data)

    def recv(self, n):
        return self._real.recv(n)

    def settimeout(self, t):
        return self._real.settimeout(t)

    def close(self):
        return self._real.close()


def test_sweep_aborts_but_keeps_hits_found_before_link_died():
    # A link that dies partway through a block must not silently look like
    # "the rest of the block is unsupported" -- it must report aborted=True
    # and keep whatever hits were genuinely collected before the failure.
    responses = {**BASE, "ATSH7E0": "OK", "0100": "4100BE3EB811", "220005": "620005AA"}
    responses["220040"] = "620040AAAA"           # hit, collected before failure
    responses["220041"] = "7F2231"               # negative, collected before failure
    fake, s = _session(responses)
    census = run_census(s, cat.PRESETS["gm"],
                        headers=[h for h in cat.HEADERS_11BIT if h.name == "7E0"])

    s.sock = _FailOnCommand(s.sock, trigger="220042")   # link dies on the 3rd probe
    blocks = [cat.Block("2200xx", 0x2200, lo=0x40, hi=0x45)]
    res = run_sweep(s, census, cat.PRESETS["gm"], blocks=blocks)

    assert res["aborted"] is True
    assert res["error"]                                  # non-empty reason

    hits = {h.request for h in res["hits"]}
    assert "220040" in hits                              # survives the truncation
    assert "220042" not in hits
    assert "220043" not in hits                          # never reached
    assert res["negatives"] >= 1                         # 220041 still counted
    assert res["probes"] == 2                            # 220040, 220041 completed

    s.close(); fake.stop()


def test_sweep_counts_elm_errors_separately_from_negatives_and_hits():
    # IMPORTANT 1: sweep has only two outcomes today (hit / negative), so an
    # ELM_ERROR (transport fault the link recovered from, no OSError raised)
    # vanishes into "probes" -- a sweep over a faulting link would report
    # "0 hits / N probes (0 negative responses), aborted: False", which reads
    # exactly like a clean negative sweep. It must be counted and surfaced
    # separately so it cannot be mistaken for "the vehicle doesn't support
    # this PID".
    responses = {**BASE, "ATSH7E0": "OK", "0100": "4100BE3EB811", "220005": "620005AA"}
    responses["220040"] = "620040AAAA"           # genuine hit
    responses["220041"] = "7F2231"               # genuine negative
    responses["220042"] = "CAN ERROR"            # transport fault, both attempts
    fake, s = _session(responses)
    census = run_census(s, cat.PRESETS["gm"],
                        headers=[h for h in cat.HEADERS_11BIT if h.name == "7E0"])
    blocks = [cat.Block("2200xx", 0x2200, lo=0x40, hi=0x42)]
    res = run_sweep(s, census, cat.PRESETS["gm"], blocks=blocks)

    assert res["errors"] == 1
    assert res["negatives"] == 1
    assert len(res["hits"]) == 1
    assert res["probes"] == 3
    s.close(); fake.stop()


def test_log_counts_elm_errors_separately_from_blank_cells(tmp_path):
    # IMPORTANT 1: log has only two outcomes today (payload / blank cell),
    # so an ELM_ERROR writes the exact same empty cell as a genuinely
    # unsupported PID -- a log over a faulting link produces a full-length
    # CSV of blanks that `correlate` then calls "no-signal" throughout, with
    # no way to tell "the vehicle doesn't have this" from "the link kept
    # faulting". error_polls must count it.
    responses = {**BASE, "ATSH7E0": "OK",
                 "010C": "410C1AF8", "010D": "410D3C", "0104": "410478",
                 "0105": "4105A0", "0110": "41100123", "0133": "413365",
                 "0146": "414628", "220041": "CAN ERROR"}
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host, port); s.connect(); s.init()
    h11 = next(x for x in cat.HEADERS_11BIT if x.name == "7E0")
    s.set_header(h11)

    out = tmp_path / "drive.csv"
    hits = [Hit("7E0", "220041", "", "", 0)]
    res = run_log(s, hits, str(out), hz=50.0, duration_s=0.1)

    assert res["error_polls"] > 0
    rows = list(csv.DictReader(out.open()))
    assert rows[0]["220041@7E0"] == ""            # cell is blank either way
    s.close(); fake.stop()


def test_log_writes_raw_hex_and_anchors(tmp_path):
    responses = {**BASE, "ATSH7E0": "OK",
                 "010C": "410C1AF8", "010D": "410D3C", "0104": "410478",
                 "0105": "4105A0", "0110": "41100123", "0133": "413365",
                 "0146": "414628", "220041": "620041DEAD"}
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host, port); s.connect(); s.init()
    h11 = next(x for x in cat.HEADERS_11BIT if x.name == "7E0")
    s.set_header(h11)

    out = tmp_path / "drive.csv"
    hits = [Hit("7E0", "220041", "620041DEAD", "DEAD", 2)]
    res = run_log(s, hits, str(out), hz=50.0, duration_s=0.1)

    rows = list(csv.DictReader(out.open()))
    assert rows, "expected at least one logged row"
    assert rows[0]["220041@7E0"] == "DEAD"        # raw hex preserved
    assert float(rows[0]["rpm"]) > 0              # anchor decoded
    assert res["rows"] == len(rows)
    s.close(); fake.stop()


def test_log_scopes_headers_to_allowed_set(tmp_path):
    # Security (audit High #2): a tampered/shared sweep.json can name an unsafe
    # module (e.g. an ADAS 7E4). run_log with allowed_headers scoped to the
    # preset must DROP any hit whose header isn't in that set — it never polls
    # the injected module.
    responses = {**BASE, "ATSH7DF": "OK",
                 "010C": "410C1AF8", "010D": "410D3C", "0104": "410478",
                 "0105": "4105A0", "0110": "41100123", "0133": "413365",
                 "0146": "414628", "225801": "625801C4"}
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host, port); s.connect(); s.init()

    out = tmp_path / "drive.csv"
    hits = [Hit("7DF", "225801", "625801C4", "C4", 1),     # legit powertrain header
            Hit("7E4", "22DEAD", "62DEADBE", "BE", 1)]      # injected ADAS module
    allowed = [h for h in cat.PRESETS["bmw"].headers if h.name == "7DF"]
    res = run_log(s, hits, str(out), hz=50.0, duration_s=0.1, allowed_headers=allowed)

    cols = csv.DictReader(out.open()).fieldnames
    assert "225801@7DF" in cols        # legit header kept
    assert "22DEAD@7E4" not in cols     # injected 7E4 dropped — never polled
    assert res["rows"] > 0
    s.close(); fake.stop()


class _FailOnNthCommand:
    """Like _FailOnCommand (see above), but only raises OSError starting on
    the Nth time a MATCHING command is sent, not the first.

    run_log() round-robin polls the same commands every cycle -- unlike a
    one-pass sweep, a plain _FailOnCommand trigger would fire on cycle 1 and
    no row would ever complete, which cannot prove "rows already flushed
    survive a later failure." Counting occurrences of the trigger command
    lets earlier cycles complete normally and the fault land on a later,
    chosen cycle -- still keyed on the command STRING, per the task's
    guidance to prefer that over a raw sendall() call count."""

    def __init__(self, real_sock, trigger: str, occurrence: int):
        self._real = real_sock
        self._trigger = (trigger.upper().replace(" ", "") + "\r").encode()
        self._occurrence = occurrence
        self._seen = 0

    def sendall(self, data):
        if data == self._trigger:
            self._seen += 1
            if self._seen >= self._occurrence:
                raise OSError("simulated link failure")
        return self._real.sendall(data)

    def recv(self, n):
        return self._real.recv(n)

    def settimeout(self, t):
        return self._real.settimeout(t)

    def close(self):
        return self._real.close()


def test_log_aborts_but_keeps_rows_written_before_link_died(tmp_path):
    # A link that dies partway through a drive log must not lose the rows
    # already flushed to disk, and must report aborted=True rather than let
    # a truncated log silently look like a completed one.
    responses = {**BASE, "ATSH7E0": "OK",
                 "010C": "410C1AF8", "010D": "410D3C", "0104": "410478",
                 "0105": "4105A0", "0110": "41100123", "0133": "413365",
                 "0146": "414628", "220041": "620041DEAD"}
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host, port); s.connect(); s.init()
    h11 = next(x for x in cat.HEADERS_11BIT if x.name == "7E0")
    s.set_header(h11)

    # "0110" (maf) is the 5th column probed each cycle. Fail on its 2nd
    # occurrence -- cycle 1 completes and is flushed in full; cycle 2 dies
    # partway through, so its row must never appear.
    s.sock = _FailOnNthCommand(s.sock, trigger="0110", occurrence=2)

    out = tmp_path / "drive.csv"
    hits = [Hit("7E0", "220041", "620041DEAD", "DEAD", 2)]
    # duration_s is a safety bound only -- the OSError on cycle 2's "0110"
    # is expected to end the loop almost immediately at hz=1000; this just
    # keeps a broken implementation from hanging the test suite instead of
    # failing it.
    res = run_log(s, hits, str(out), hz=1000.0, duration_s=5.0)

    assert res["aborted"] is True
    assert res["error"]                                  # non-empty reason

    rows = list(csv.DictReader(out.open()))
    assert res["rows"] == 1                              # only cycle 1 completed
    assert len(rows) == 1                                # CSV matches: readable, intact
    assert rows[0]["220041@7E0"] == "DEAD"
    assert float(rows[0]["rpm"]) > 0

    s.close(); fake.stop()


def _bmw_session(responses):
    # NOTE: named distinctly from the `_session(extra) -> (fake, s)` helper
    # above (used throughout the GM census/sweep/log tests in this file) --
    # this one returns (s, fake), the opposite order. Reusing the name
    # `_session` here would silently rebind the module-level name and break
    # every earlier test that unpacks `fake, s = _session(...)`.
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host=host, port=port, probe_timeout=0.3)
    s.connect()
    return s, fake


def test_capability_probe_detects_unsupported_adapter():
    s, fake = _bmw_session({"ATCEA12": "?", "ATCRA612": "?"})
    try:
        assert probe_bmw_capability(s) is False      # '?' => can't do extended addressing
    finally:
        s.close(); fake.stop()


def test_capability_probe_passes_capable_adapter():
    s, fake = _bmw_session({"ATCEA12": "OK", "ATCRA612": "OK"})
    try:
        assert probe_bmw_capability(s) is True
    finally:
        s.close(); fake.stop()


def test_go_no_go_positive():
    # A real DA25 reply: BMW enhanced 62 DA25 <hi> <lo>. classify() -> POSITIVE.
    hdr = next(h for h in cat.BMW_HEADERS if h.name == "BMW-618")
    s, fake = _bmw_session({"ATSP6": "OK", "ATSH6F1": "OK", "ATCEA18": "OK",
                            "ATCRA618": "OK", "22DA25": "62 DA 25 20 30"})
    try:
        assert default_session_gate(s, hdr) == "positive"
    finally:
        s.close(); fake.stop()


def test_go_no_go_negative_is_session_gated():
    hdr = next(h for h in cat.BMW_HEADERS if h.name == "BMW-618")
    s, fake = _bmw_session({"ATSP6": "OK", "ATSH6F1": "OK", "ATCEA18": "OK",
                            "ATCRA618": "OK", "22DA25": "7F 22 31"})
    try:
        assert default_session_gate(s, hdr) == "negative"
    finally:
        s.close(); fake.stop()


# --- anchor Mode-22 mirror fallback -----------------------------------------
# Real case, 2021 F-350: 0110 (maf) and 0146 (ambient) were silent for the
# whole drive while 22F410 and 22F446 answered fine. `correlate` reported both
# anchors UNUSABLE and scored every candidate with 5 of 7 anchors.

# Every generic anchor answering EXCEPT maf/ambient, whose mirrors answer.
_ANCHORS_OK = {"010C": "410C1AF8", "010D": "410D3C", "0104": "410478",
               "0105": "4105A0", "0110": "41100123", "0133": "413365",
               "0146": "414628"}


def _log_once(responses, tmp_path, name="drive.csv", duration_s=0.1):
    fake = FakeElm(responses)
    host, port = fake.start()
    s = ElmSession(host, port); s.connect(); s.init()
    s.set_header(next(x for x in cat.HEADERS_11BIT if x.name == "7E0"))
    out = tmp_path / name
    hits = [Hit("7E0", "220041", "620041DEAD", "DEAD", 2)]
    res = run_log(s, hits, str(out), hz=50.0, duration_s=duration_s)
    rows = list(csv.DictReader(out.open()))
    s.close(); fake.stop()
    return res, rows, fake.requests_seen


def test_anchor_falls_back_to_mode22_mirror(tmp_path):
    responses = {**BASE, "ATSH7E0": "OK", "220041": "620041DEAD",
                 **{k: v for k, v in _ANCHORS_OK.items() if k not in ("0110", "0146")},
                 # J1979 F4xx mirror returns the SAME data bytes as PID xx.
                 "22F410": "62F4100123", "22F446": "62F44628"}
    res, rows, _ = _log_once(responses, tmp_path)

    assert res["anchor_requests"]["maf"] == "22F410"
    assert res["anchor_requests"]["ambient"] == "22F446"
    # untouched anchors keep their generic request
    assert res["anchor_requests"]["coolant"] == "0105"
    assert len(res["anchor_fallbacks"]) == 2

    # The whole point: the columns carry DATA instead of being UNUSABLE, and
    # they decode identically to the generic path (0x0123/100, 0x28-40).
    assert float(rows[0]["maf"]) == 0x0123 / 100.0
    assert float(rows[0]["ambient"]) == 0x28 - 40
    assert all(r["maf"] and r["ambient"] for r in rows)


def test_no_mirror_probe_when_generic_anchor_answers(tmp_path):
    responses = {**BASE, "ATSH7E0": "OK", "220041": "620041DEAD", **_ANCHORS_OK}
    res, rows, seen = _log_once(responses, tmp_path)

    assert res["anchor_fallbacks"] == []
    assert res["anchor_requests"] == dict(cat.ANCHORS)
    # No mirror is ever sent -- a working generic anchor must cost nothing.
    assert not [q for q in seen if q.startswith("22F4")]


def test_dead_anchor_probes_its_mirror_only_once(tmp_path):
    # Both forms dead. Retrying the mirror every cycle would spend sample
    # density on every column for the whole drive to chase an anchor that is
    # not coming back, so resolution is one-shot.
    responses = {**BASE, "ATSH7E0": "OK", "220041": "620041DEAD",
                 **{k: v for k, v in _ANCHORS_OK.items() if k != "0146"}}
    res, rows, seen = _log_once(responses, tmp_path, duration_s=0.3)

    assert len(rows) > 3, "need several cycles for this to mean anything"
    assert res["anchor_fallbacks"] == []
    assert res["anchor_requests"]["ambient"] == "0146"   # unchanged
    assert seen.count("22F446") == 1
    assert all(r["ambient"] == "" for r in rows)
