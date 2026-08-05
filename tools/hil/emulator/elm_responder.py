#!/usr/bin/env python3
"""A minimal ELM327 responder — enough to drive this firmware, and MIT.

WHY NOT AN OFF-THE-SHELF EMULATOR
Ircama's ELM327-emulator is more capable than this and was used to prove the
approach. It is licensed **CC-BY-NC-SA-4.0** — non-commercial, share-alike, and
classified on PyPI as proprietary. This repository is MIT, which permits
commercial use; shipping a rig that *requires* a non-commercial dependency would
quietly revoke that for anyone commercial. NC restricts the USER, not merely the
redistributor, so "it's only a pip install" does not get around it.

So the rig ships this instead: fewer features, no licence entanglement, and
`ble_elm.py` will happily point at Ircama (or anything else speaking ELM327 over
TCP) if you want its full UDS depth and the NC terms suit you.

WHAT IT COVERS
The exact dialogue this firmware performs, captured from the bench:
  AT layer   ATZ, ATE0, ATL0, ATS0/ATS1, ATAT2, ATST19, ATSH (both spellings)
  Mode 01    standard PIDs from the scenario table
  Mode 09    PID 02 (VIN), with real multi-frame ISO-TP framing
  Mode 22    enhanced DIDs, ROUTED BY THE CURRENT HEADER
Anything unrecognised answers NO DATA, which is what a real adapter does and
what the firmware's timeout path expects.

WHAT IT DELIBERATELY DOES NOT DO
No flow control, no timing emulation, no protocol negotiation, no DTCs, no
stateful diagnostic sessions. If you need those, use a full emulator — this
exists to make the BLE path testable, not to be a car.
"""

from dataclasses import dataclass, field

# A real ELM327 powers up with echo ON. This matters: ble_obd_source.cpp treats
# a missing ATE0 acknowledgement as fatal and abandons the connect, so a
# responder that never echoed would leave that retry path untested.
BANNER = "ELM327 v1.5"


def iso_tp_frames(payload_hex: str, spaces: bool = False) -> str:
    """Format a payload the way an ELM327 prints it.

    Short replies (<= 7 bytes) print bare. Longer ones print a length header —
    the total byte count in HEX — then indexed frames:

        014\\r0:<6 bytes>\\r1:<7 bytes>\\r2:<7 bytes>\\r\\r>

    Frame 0 carries SIX data bytes because the CAN first-frame PCI consumes two
    of the eight; every consecutive frame carries seven. Getting that split
    wrong still produces plausible-looking output whose byte offsets are all
    shifted by one, so it is pinned by a test.

    The frame index is the ISO-TP sequence number and wraps in the low nibble
    (0..F), so a long reply never prints "10:".
    """
    payload_hex = payload_hex.replace(" ", "").upper()
    nbytes = len(payload_hex) // 2

    def spaced(h: str) -> str:
        if not spaces:
            return h
        return " ".join(h[i:i + 2] for i in range(0, len(h), 2))

    if nbytes <= 7:
        return f"{spaced(payload_hex)}\r\r>"

    out = [f"{nbytes:03X}"]
    first, rest = payload_hex[:12], payload_hex[12:]      # 6 bytes, then 7s
    out.append(f"0:{spaced(first)}")
    idx = 1
    for i in range(0, len(rest), 14):
        out.append(f"{idx & 0x0F:X}:{spaced(rest[i:i + 14])}")
        idx += 1
    return "\r".join(out) + "\r\r>"


@dataclass
class Scenario:
    """What the fake vehicle answers.

    `mode22` is keyed by CAN header first, because that is the distinction the
    whole rig exists to exercise: 221940 (transmission temperature) lives on the
    transmission ECU at 7E2, and a firmware bug that forgot to switch headers
    must FAIL rather than quietly read the engine ECU.
    """
    vin: str = ""
    mode01: dict = field(default_factory=dict)            # {"0C": "1AF8"}
    mode22: dict = field(default_factory=dict)            # {"7E0": {"000C": "0BB8"}}
    banner: str = BANNER


class ElmResponder:
    def __init__(self, scenario: Scenario):
        self.s = scenario
        self.reset()

    def reset(self) -> None:
        self.echo = True          # real hardware powers up echoing
        self.spaces = False       # the firmware sends ATS0 immediately anyway
        self.header = "7DF"       # functional broadcast until told otherwise

    # -- helpers ------------------------------------------------------------

    def _reply(self, body: str) -> str:
        return f"{body}\r\r>"

    def _ok(self) -> str:
        return self._reply("OK")

    def _no_data(self) -> str:
        return self._reply("NO DATA")

    # -- entry point --------------------------------------------------------

    def handle(self, line: str) -> str:
        """Answer one command. `line` has no trailing CR."""
        raw = line.strip()
        # Normalise for MATCHING only. The firmware emits both "ATSH7E0" and
        # "AT SH 7E0" from different call sites; a real ELM ignores spaces in
        # commands, and accepting only one spelling would silently break half
        # the queries.
        cmd = raw.replace(" ", "").upper()
        prefix = f"{raw}\r" if self.echo else ""

        if cmd.startswith("AT"):
            return prefix + self._at(cmd)
        return prefix + self._obd(cmd)

    # -- AT layer -----------------------------------------------------------

    def _at(self, cmd: str) -> str:
        if cmd == "ATZ":
            was_echo = self.echo
            self.reset()
            self.echo = was_echo          # the echo of ATZ itself already went out
            return self._reply(f"\r\r{self.s.banner}")
        if cmd == "ATE0":
            self.echo = False
            return self._ok()
        if cmd == "ATE1":
            self.echo = True
            return self._ok()
        if cmd == "ATS0":
            self.spaces = False
            return self._ok()
        if cmd == "ATS1":
            self.spaces = True
            return self._ok()
        if cmd.startswith("ATSH"):
            self.header = cmd[4:] or self.header
            return self._ok()
        if cmd == "ATRV":
            return self._reply("14.2V")
        if cmd.startswith("ATI"):
            return self._reply(self.s.banner)
        # Everything else (ATL0, ATAT2, ATST19, ATSP…) is accepted. Answering OK
        # rather than staying silent matters: silence would hang the firmware's
        # drainToPrompt for its whole timeout on every unknown command.
        return self._ok()

    # -- OBD layer ----------------------------------------------------------

    def _obd(self, cmd: str) -> str:
        if len(cmd) < 4 or not all(c in "0123456789ABCDEF" for c in cmd):
            return self._no_data()
        mode, rest = cmd[:2], cmd[2:]

        if mode == "01":
            data = self.s.mode01.get(rest.upper())
            if data is None:
                return self._no_data()
            # Service 01 echoes ONE request byte; the positive response SID is
            # the request SID | 0x40.
            return iso_tp_frames(f"41{rest}{data}", self.spaces)

        if mode == "09" and rest == "02":
            if not self.s.vin:
                return self._no_data()
            # Mode-09 PID-02 payload: 49 02 <message count> then 17 ASCII bytes.
            body = "4902" + "01" + "".join(f"{ord(c):02X}" for c in self.s.vin)
            return iso_tp_frames(body, self.spaces)

        if mode == "22":
            table = self.s.mode22.get(self.header.upper(), {})
            data = table.get(rest.upper())
            if data is None:
                return self._no_data()
            # Service 22 echoes TWO request bytes (the 16-bit DID). Echoing one,
            # as service 01 does, shifts every payload byte and produces
            # plausible-but-wrong readings rather than an obvious failure.
            return iso_tp_frames(f"62{rest}{data}", self.spaces)

        return self._no_data()
