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
import errno
import socket
import time

from . import catalog as cat
from .reply import Cls, Reply, classify

PROMPT = ">"


class AdapterUnreachable(Exception):
    """The ELM327 adapter could not be reached, or was reached but is silent.

    Raised instead of letting a bare OSError escape to a Python traceback.
    `str(e)` is a complete, parking-lot-readable diagnosis: what was tried,
    what happened, and a numbered checklist of what to do about it. The CLI
    prints it verbatim, so keep the text self-contained.
    """


def diagnose_connect_error(err: BaseException, host: str, port: int, timeout: float) -> str:
    """Turn a socket-level connect failure into a field diagnosis.

    Pure and socket-free so it can be tested without a network. The three
    common failures each have a DIFFERENT fix, which is the entire reason
    this exists -- a generic "cannot connect to adapter" would send you
    checking the wrong thing:

      timed out  -> the laptop is on the wrong WiFi (by far the most common)
      refused    -> right host, wrong port (or the adapter is still booting)
      gaierror   -> --host was given a name, not an IP

    Ordering matters: gaierror and ConnectionRefusedError are both OSError
    subclasses, and TimeoutError IS socket.timeout on Python 3.10+, so the
    specific types must be tested before the errno fallback.
    """
    where = f"{host}:{port}"
    if isinstance(err, socket.gaierror):
        return (
            f"Cannot reach the OBD adapter: the host name '{host}' could not be resolved.\n"
            f"\n"
            f"  1. --host takes an IP address, not a name.\n"
            f"     The Vgate iCar Pro WiFi is 192.168.0.10 (the default).\n"
        )
    if isinstance(err, ConnectionRefusedError):
        return (
            f"Cannot reach the OBD adapter at {where}: the host answered but REFUSED the\n"
            f"connection -- something is at {host}, but nothing is listening on port {port}.\n"
            f"\n"
            f"  1. Check --port. The iCar Pro WiFi serves ELM327 on 35000 (the default).\n"
            f"  2. The adapter may still be booting. Give it ~10 s after plugging it into\n"
            f"     the OBD-II port, then re-run.\n"
            f"  3. If {host} is your own machine or a router, --host is pointing at the\n"
            f"     wrong device.\n"
        )
    if isinstance(err, (TimeoutError, socket.timeout)):
        return (
            f"Cannot reach the OBD adapter at {where}: timed out after {timeout:.0f} s with no\n"
            f"reply to the TCP handshake.\n"
            f"\n"
            f"This almost always means the laptop is not joined to the adapter's WiFi.\n"
            f"\n"
            f"  1. Plug the adapter into the OBD-II port and wait for its LED.\n"
            f"  2. Turn the ignition on -- the port is unpowered on some vehicles otherwise.\n"
            f"  3. Join the adapter's SoftAP in WiFi settings (normally named V-LINK).\n"
            f"     It has no internet, so macOS may auto-switch back to a known network --\n"
            f"     re-check that you are still on it right before re-running.\n"
            f"  4. If your adapter uses a different address, pass --host / --port.\n"
        )
    if getattr(err, "errno", None) in (errno.ENETUNREACH, errno.EHOSTUNREACH, errno.ENETDOWN):
        return (
            f"Cannot reach the OBD adapter at {where}: no route to that address from this\n"
            f"machine ({err}).\n"
            f"\n"
            f"  1. Join the adapter's SoftAP in WiFi settings (normally named V-LINK).\n"
            f"  2. Confirm WiFi is on and the adapter is powered.\n"
        )
    return (
        f"Cannot reach the OBD adapter at {where}: {type(err).__name__}: {err}\n"
        f"\n"
        f"  1. Confirm you are joined to the adapter's WiFi (normally V-LINK).\n"
        f"  2. Confirm --host / --port match the adapter (iCar Pro: 192.168.0.10:35000).\n"
    )


class ElmSession:
    def __init__(self, host: str = "192.168.0.10", port: int = 35000,
                 timeout: float = 5.0, probe_timeout: float = 1.0,
                 reset_timeout: float = 3.0):
        self.host, self.port = host, port
        self.timeout = timeout
        self.probe_timeout = probe_timeout
        # Ceiling for the ATZ reset in init(). Broken out of the hard-coded
        # 3.0 so the silent-adapter test can run fast; the default is unchanged.
        self.reset_timeout = reset_timeout
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
        """Open the TCP link, or raise AdapterUnreachable with a diagnosis.

        Every socket failure here (refused / timed out / unresolvable / no
        route) is an OSError subclass, and letting any of them escape prints
        a traceback whose top frame is `sock.connect(sa)` -- true, and
        useless to someone sitting in a truck wondering which WiFi they are
        on. Translate once, at the boundary."""
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as e:
            raise AdapterUnreachable(
                diagnose_connect_error(e, self.host, self.port, self.timeout)) from e
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
        timing on with a short ceiling. Returns the adapter identity string.

        Raises AdapterUnreachable if ATZ draws no ELM327 prompt. This is the
        failure mode that does NOT crash and is therefore the most dangerous:
        an open socket that never speaks ELM327 let init() return "" and the
        scan run to completion with every header `evidence="error"` -- a link
        fault presented as a finding about the vehicle. Fail at the first
        unanswered command instead of producing an empty census."""
        ident = self.cmd("ATZ", timeout=self.reset_timeout)
        if not self.last_saw_prompt:
            # Retry once before condemning the link, mirroring probe()'s
            # retry-then-report: a clone that is merely slow to come out of
            # reset used to proceed with a blank identity, and turning that
            # into a hard abort would be a regression on working hardware.
            # Two consecutive unanswered ATZs is the link, not slowness.
            ident = self.cmd("ATZ", timeout=self.reset_timeout)
        if not self.last_saw_prompt:
            raise AdapterUnreachable(
                f"Connected to {self.host}:{self.port}, but the adapter never answered ATZ\n"
                f"(no ELM327 '>' prompt within {self.reset_timeout:.0f} s).\n"
                f"\n"
                f"The TCP socket opened, so something is listening -- it just is not\n"
                f"speaking ELM327. Nothing was learned about the vehicle.\n"
                f"\n"
                f"  1. Power-cycle the adapter: unplug it from the OBD-II port, replug,\n"
                f"     wait ~10 s.\n"
                f"  2. Close any other app holding the adapter. Most WiFi ELM327s accept\n"
                f"     ONE client at a time, so a phone still connected in Car Scanner or\n"
                f"     Torque will keep this session silent.\n"
                f"  3. Confirm --port points at the ELM327 socket (iCar Pro: 35000) and\n"
                f"     not at the adapter's web/config port.\n")
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
