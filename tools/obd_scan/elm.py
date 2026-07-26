"""
ELM327 session over a TCP socket.

Target adapter: Vgate iCar Pro WiFi — SoftAP `V-LINK`, transparent ELM327 at
192.168.0.10:35000, no pairing or bonding.

Two hard-won behaviours are baked in here:

1. PROTOCOL SEARCH IS FRAGILE. Any host character aborts the ELM's auto-search,
   so the first 0100 after ATSP0 gets a multi-second window. A short timeout
   latches a wrong protocol and every subsequent PID returns NO DATA — this
   cost a debug cycle on the GM truck.
2. `AT SH` RETURNS ITS OWN `OK>`. It must be consumed before the PID is sent or
   it gets mistaken for the data reply.

Speed matters: the header is set once per block rather than per probe, and
ATAT2 adaptive timing lets a NO DATA return in ~60-100 ms instead of a fixed
700 ms. That is the difference between a practical multi-block sweep and one
that does not fit in a stationary session.
"""
import socket
import time

from . import catalog as cat
from .reply import Cls, Reply, classify

PROMPT = ">"


class ElmSession:
    def __init__(self, host: str = "192.168.0.10", port: int = 35000,
                 timeout: float = 5.0, probe_timeout: float = 1.0):
        self.host, self.port = host, port
        self.timeout = timeout
        self.probe_timeout = probe_timeout
        self.sock: socket.socket | None = None
        self.cur_header: str | None = None
        # True once a header has turned BMW extended addressing on (at_cea or
        # at_cra set). Drives set_header()'s stateful clear: GM/Ford headers
        # never set this, so they never trigger the bare ATCEA/ATCRA off-forms.
        self._ext_active: bool = False
        # The ATSP selector actually in force ("6"/"7"), or None when unknown
        # (e.g. right after an ATSP0 auto-detect). Compared against
        # Header.at_sp in set_header() -- must never hold anything else.
        self.cur_protocol: str | None = None
        # Human-readable ATDP description ("ISO 15765-4 (CAN 11/500)"), for
        # display only. Kept separate from cur_protocol: the two used to
        # share one field, so the ATSP-selector comparison in set_header()
        # never matched and ATSP was re-sent needlessly.
        self.protocol_desc: str | None = None
        self.last_latency_ms: float = 0.0
        # Set on every _read_to_prompt() call: whether the '>' prompt was
        # actually seen before the read returned. False means the text is
        # possibly truncated (deadline expiry or peer close) and must not be
        # trusted as a complete reply.
        self.last_saw_prompt: bool = False

    # --- transport ---------------------------------------------------------
    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)

    def close(self) -> None:
        if self.sock:
            self.sock.close()
            self.sock = None

    def _read_to_prompt(self, deadline_s: float) -> str:
        """Accumulate until the ELM prompt or the deadline. Returns raw text.

        Sets `self.last_saw_prompt` on every call: True only if the '>' was
        actually seen before returning. On deadline expiry or peer close
        (recv() returning b"") the accumulated text may be partial -- callers
        must consult the flag rather than assume completeness."""
        assert self.sock is not None
        self.last_saw_prompt = False
        out, end = "", time.monotonic() + deadline_s
        while time.monotonic() < end:
            self.sock.settimeout(max(0.01, end - time.monotonic()))
            try:
                chunk = self.sock.recv(512)
            except socket.timeout:
                break
            except OSError:
                break
            if not chunk:
                break
            out += chunk.decode(errors="ignore")
            if PROMPT in out:
                self.last_saw_prompt = True
                return out.split(PROMPT)[0]
        return out

    def cmd(self, text: str, timeout: float | None = None) -> str:
        """Send one command and read to the prompt. AT commands are allowlisted;
        anything else must be a read-service request."""
        assert self.sock is not None
        if text.upper().replace(" ", "").startswith("AT"):
            cat.validate_at(text)
        else:
            cat.validate_request(text)
        self.sock.sendall((text + "\r").encode())
        t0 = time.monotonic()
        raw = self._read_to_prompt(timeout if timeout is not None else self.probe_timeout)
        self.last_latency_ms = (time.monotonic() - t0) * 1000.0
        return raw

    # --- session -----------------------------------------------------------
    def init(self) -> str:
        """Reset and configure: echo off, linefeeds off, spaces off, adaptive
        timing on with a short ceiling. Returns the adapter identity string."""
        ident = self.cmd("ATZ", timeout=3.0)
        for c in ("ATE0", "ATL0", "ATS0"):
            self.cmd(c)
        for c in ("ATAT2", "ATST19"):      # adaptive timing, ~100ms ceiling
            self.cmd(c)                    # best-effort: clones may answer '?'
        self.cur_header = None
        return ident.strip()

    def detect_protocol(self) -> str:
        """Auto-detect with a WIDE window (see module docstring) and report the
        protocol the adapter settled on.

        The description is for display only and is never compared against a
        Header.at_sp selector. The in-force selector is not one of our known
        values after an auto-detect, so cur_protocol resets to None -- the
        next set_header() call will legitimately (re-)assert it once."""
        self.cmd("ATSP0")
        self.cmd("0100", timeout=10.0)     # must not be interrupted
        self.protocol_desc = self.cmd("ATDP", timeout=3.0).strip()
        self.cur_protocol = None
        self.cur_header = None
        return self.protocol_desc

    def set_header(self, h: cat.Header) -> None:
        """Select a request header. Cached — re-selecting the same header is a
        no-op, which is what keeps the sweep fast."""
        if self.cur_header == h.name:
            return
        if self.cur_protocol != h.at_sp:
            self.cmd(f"ATSP{h.at_sp}")
            self.cur_protocol = h.at_sp
        if h.at_cp:
            self.cmd(f"ATCP{h.at_cp}")
        self.cmd(f"ATSH{h.at_sh}")         # consumes its own OK> here
        # BMW extended addressing. Set the target byte + RX filter for a BMW
        # header; clear BOTH (bare ATCEA/ATCRA) only when leaving one — so
        # GM/Ford headers (at_cea/at_cra None, never active) send nothing extra
        # and Mode-01 after a BMW read isn't blocked by a stale 618 filter.
        if h.at_cea:
            self.cmd(f"ATCEA{h.at_cea}")
        elif self._ext_active:
            self.cmd("ATCEA")              # turn extended addressing OFF
        if h.at_cra:
            self.cmd(f"ATCRA{h.at_cra}")
        elif self._ext_active:
            self.cmd("ATCRA")              # reset the RX-address filter
        self._ext_active = bool(h.at_cea or h.at_cra)
        self.cur_header = h.name

    def probe(self, request: str) -> Reply:
        """Send one read request and classify the reply. Retries once on a
        transport-level ELM error, or when the '>' prompt was never seen
        (deadline expiry / peer close) -- that reply may be truncated and
        must not be trusted just because it happened to parse. Timeouts are
        normal and expected here (an unsupported PID legitimately times
        out), so this degrades to a retry-then-report, never an exception.

        CRITICAL: if the RETRY's reply *also* arrives with no prompt seen,
        the reply must not be classified and returned as-is. classify("")
        is Cls.NO_DATA -- indistinguishable from a genuine non-answer -- so
        a wedged link (accepts, reads forever, never replies: sendall()
        succeeds against the kernel buffer, only the read side is silent)
        would otherwise be recorded as a clean "no answer" from the
        vehicle. Two consecutive promptless reads mean the LINK is why we
        got nothing, not the vehicle -- report Cls.ELM_ERROR instead."""
        cat.validate_request(request)
        raw = self.cmd(request)
        r = classify(raw, request)
        if r.cls is Cls.ELM_ERROR or not self.last_saw_prompt:
            raw = self.cmd(request)
            r = classify(raw, request)
            if not self.last_saw_prompt:
                return Reply(Cls.ELM_ERROR, raw)
        return r
