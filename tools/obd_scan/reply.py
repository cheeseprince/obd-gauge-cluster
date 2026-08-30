"""
Classify and parse raw ELM327 replies.

Pure functions only — no I/O — so the whole parsing surface is testable
without an adapter or a vehicle.

The key correctness property here is ECHO MATCHING. A positive UDS response
to `22F446` is `62 F4 46 <data...>`: service byte + the echoed PID. The
firmware's scanner instead searched for the substring "62" anywhere in the
reply, which false-positives on any data byte that happens to render as 62.
On an unknown platform the whole point is trusting the hit list, so we parse.
"""
from dataclasses import dataclass, field
from enum import Enum

# Error strings the ELM327 emits in place of a vehicle response.
_ELM_ERRORS = ("?", "BUFFER FULL", "CAN ERROR", "STOPPED", "UNABLE TO CONNECT",
               "BUS INIT", "BUS ERROR", "DATA ERROR", "FB ERROR", "ACT ALERT",
               "LP ALERT", "RX ERROR", "ERR")


class Cls(Enum):
    POSITIVE          = "positive"            # real data
    NRC_OUT_OF_RANGE  = "nrc_out_of_range"    # 7F xx 31 — module alive, PID absent
    NRC_PENDING       = "nrc_pending"         # 7F xx 78 — wait and re-read
    NRC_OTHER         = "nrc_other"           # any other negative response
    NO_DATA           = "no_data"             # nothing answered
    ELM_ERROR         = "elm_error"           # transport fault — retry, don't record


@dataclass
class Reply:
    cls: Cls
    raw: str
    payload: bytes = b""          # data bytes AFTER service + echoed PID
    nrc: int | None = None        # negative response code, when cls is an NRC_*
    frames: list[str] = field(default_factory=list)


def _hex_bytes(text: str) -> bytes:
    """Parse a run of hex digits (spaces optional) into bytes. Odd digit counts
    and non-hex characters yield b'' rather than raising — a garbled frame is a
    classification problem, not an exception."""
    compact = "".join(c for c in text if not c.isspace())
    if not compact or len(compact) % 2 or any(c not in "0123456789ABCDEFabcdef" for c in compact):
        return b""
    return bytes.fromhex(compact)


def _request_parts(request: str) -> tuple[int, bytes]:
    """('22F446') -> (0x22, b'\\xF4\\x46'). Service byte + echoed PID bytes."""
    raw = _hex_bytes(request)
    return (raw[0], raw[1:]) if raw else (0, b"")


def classify(raw: str, request: str) -> Reply:
    """Classify one raw ELM reply against the request that produced it."""
    text = raw.replace("\\r", "\r").replace("\\n", "\n")
    upper = text.upper()

    if not text.strip():
        return Reply(Cls.NO_DATA, raw)
    if "NO DATA" in upper:
        return Reply(Cls.NO_DATA, raw)
    for err in _ELM_ERRORS:
        if err in upper:
            return Reply(Cls.ELM_ERROR, raw)

    lines = [ln.strip() for ln in upper.replace("\n", "\r").split("\r") if ln.strip()]
    if not lines:
        return Reply(Cls.NO_DATA, raw)

    data = _hex_bytes(lines[0])
    if len(lines) > 1:                     # possible multi-frame reply
        assembled = assemble_multiframe(lines)
        if assembled:
            data = assembled

    # Negative response: 7F <service> <nrc>
    if len(data) >= 3 and data[0] == 0x7F:
        nrc = data[2]
        cls = (Cls.NRC_OUT_OF_RANGE if nrc == 0x31 else
               Cls.NRC_PENDING      if nrc == 0x78 else
               Cls.NRC_OTHER)
        return Reply(cls, raw, nrc=nrc, frames=lines)

    svc, pid = _request_parts(request)
    want_svc = (svc + 0x40) & 0xFF          # 0x22 -> 0x62, 0x01 -> 0x41
    if len(data) >= 1 + len(pid) and data[0] == want_svc and bytes(data[1:1 + len(pid)]) == pid:
        return Reply(Cls.POSITIVE, raw, payload=bytes(data[1 + len(pid):]), frames=lines)

    return Reply(Cls.NO_DATA, raw, frames=lines)


def assemble_multiframe(lines: list[str]) -> bytes:
    """Assemble an ELM327 multi-frame reply.

    Format: a bare length line (3 hex digits = total byte count) followed by
    `N:<hex>` fragment lines in order. Returns b'' if the fragments do not
    supply at least the declared length (a dropped fragment must not silently
    decode as short-but-valid data), or if the fragment indices are not the
    ISO-TP sequence 0,1,..,F,0,1,.. in arrival order (a duplicate index masking
    a missing one must not silently decode as valid data either).
    """
    if not lines:
        return b""
    head = "".join(lines[0].split())
    if ":" in head or not all(c in "0123456789ABCDEF" for c in head):
        return b""
    try:
        total = int(head, 16)
    except ValueError:
        return b""

    frags: list[tuple[int, bytes]] = []
    for ln in lines[1:]:
        if ":" not in ln:
            continue
        idx_txt, _, hex_txt = ln.partition(":")
        try:
            idx = int(idx_txt.strip(), 16)
        except ValueError:
            continue
        frags.append((idx, _hex_bytes(hex_txt)))

    if not frags:
        return b""
    # The fragment index is the ISO-TP sequence number, a FOUR-BIT field: the
    # ELM prints its low nibble, so frame 16 prints "0:" again and a reply of
    # 17+ frames counts 0..F,0,1,... Validate against a running counter in
    # ARRIVAL order -- no gaps, no duplicates, no reordering, since a duplicate
    # index can otherwise mask a missing one and still coincidentally reach the
    # declared length below.
    #
    # Sorting cannot do this job once the index wraps: 0..F,0 sorts to 0,0,1,...
    # Mirrors assembleVinFrames() in src/vin.cpp, which masks the nibble the
    # same way against the same counter.
    for expect, (idx, _frag) in enumerate(frags):
        if idx != (expect & 0x0F):
            return b""
    body = b"".join(b for _, b in frags)
    # Do NOT rstrip 0x55 here: the declared length is the true data length,
    # so ELM padding always sits beyond `total`. Truncating to `total` below
    # already discards padding -- stripping first would also strip a
    # genuine final data byte that happens to be 0x55.
    if len(body) < total:
        return b""
    return body[:total]


def decode_supported(payload: bytes, base: int) -> list[int]:
    """Decode a Mode-01 'PIDs supported' 32-bit bitmap.

    The MSB of the first byte is PID base+1; the LSB of the last is base+0x20.
    """
    if len(payload) < 4:
        return []
    bits = int.from_bytes(payload[:4], "big")
    return [base + 1 + i for i in range(32) if bits & (1 << (31 - i))]
