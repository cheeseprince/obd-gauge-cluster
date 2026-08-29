"""
Session stages: census, sweep, log.

CENSUS answers "who is out there and how do I address them" — the question no
public source could answer for Ford. A header counts as ALIVE if it produced
either a positive or a NEGATIVE response: `7F 22 31` (request out of range)
proves a real module received the request and rejected the PID, which confirms
the addressing even though no data came back.
"""
import csv as _csv
import time as _time
from dataclasses import dataclass, field
from datetime import datetime, timezone

from . import catalog as cat
from .elm import ElmSession
from .reply import Cls, decode_supported

_NEG = {Cls.NRC_OUT_OF_RANGE, Cls.NRC_OTHER, Cls.NRC_PENDING}
_SUPPORT_PIDS = [0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0]


@dataclass
class HeaderResult:
    header: cat.Header
    bits: int
    alive: bool = False
    # "positive" | "negative" | "silent" | "error". "error" means the
    # transport failed while this header was being probed -- we could not
    # determine anything about it, so it must be reported as neither alive
    # nor silent (silent implies a clean link that got a clean non-answer).
    evidence: str = "silent"
    supported_pids: list[int] = field(default_factory=list)
    replies: dict[str, str] = field(default_factory=dict)


def _walk_support_bitmaps(sess: ElmSession) -> list[int]:
    """Walk 0100/0120/... collecting the module's supported generic PIDs.
    Seven probes buys the exact generic-PID list for free."""
    found: list[int] = []
    for base in _SUPPORT_PIDS:
        r = sess.probe(f"01{base:02X}")
        if r.cls is not Cls.POSITIVE:
            break
        pids = decode_supported(r.payload, base)
        found.extend(pids)
        if (base + 0x20) not in pids:      # continuation bit clear -> stop
            break
    return found


def read_vin(sess: ElmSession) -> "str | None":
    """Read the VIN via Mode-09 PID 02 from the engine ECU at 7E0 — standard
    powertrain addressing, so it never touches a chassis/ADAS module. Returns
    the 17-char VIN, or None if it can't be read or parsed. Used by
    `--vehicle auto` to pick the preset from the VIN's WMI."""
    h = next((x for x in cat.HEADERS_11BIT if x.name == "7E0"), None)
    if h is None:
        return None
    sess.set_header(h)
    r = sess.probe("0902")
    if r.cls is not Cls.POSITIVE:
        return None
    return cat.parse_vin_from_payload(r.payload)


def run_census(sess: ElmSession, preset: cat.VehiclePreset,
               headers: list[cat.Header] | None = None) -> dict:
    """Probe every candidate header in both addressing modes.

    A socket failure (OSError) is usually persistent -- the adapter fell off
    BLE/WiFi, the process lost the link, whatever -- so every header probed
    afterward would hit the identical fault. Rather than let that read as
    "every remaining header is silent" (indistinguishable from "this vehicle
    has no modules") or let it crash the whole census outright depending on
    exactly which call happened to fail, set_header() and every probe() call
    are guarded identically: on OSError, the header being worked on when the
    link died is marked evidence="error" (never alive, never silent) and the
    census stops immediately. Headers never reached are simply absent from
    "headers" -- that is what distinguishes "probed and silent" from "never
    probed" -- and the return value flags the truncation explicitly via
    "aborted"/"error" so a caller can say "the link died" instead of
    "nothing is there."
    """
    cat.validate_preset(preset)
    candidates = headers if headers is not None else preset.headers
    results: list[HeaderResult] = []
    aborted = False
    error: str | None = None

    for h in candidates:
        row = HeaderResult(header=h, bits=h.bits)
        try:
            sess.set_header(h)

            for req in preset.probes:
                r = sess.probe(req)
                row.replies[req] = r.raw
                if r.cls is Cls.POSITIVE:
                    row.alive, row.evidence = True, "positive"
                elif r.cls in _NEG and row.evidence != "positive":
                    row.alive, row.evidence = True, "negative"
                elif r.cls is Cls.ELM_ERROR and row.evidence not in ("positive", "negative"):
                    # A second consecutive transport fault (probe() already
                    # retried once internally) -- transport trouble, not
                    # silence. Do not let it fall through to "silent".
                    row.alive, row.evidence = False, "error"

            if row.evidence == "positive":
                row.supported_pids = _walk_support_bitmaps(sess)
        except OSError as exc:
            row.alive, row.evidence = False, "error"
            results.append(row)
            aborted, error = True, (str(exc) or exc.__class__.__name__)
            break
        else:
            results.append(row)

    return {
        "protocol": sess.cur_protocol,
        "preset": preset.name,
        "headers": results,
        "alive": [r.header.name for r in results if r.alive],
        "aborted": aborted,
        "error": error,
    }


def probe_bmw_capability(sess: ElmSession) -> bool:
    """Return True only if the adapter accepts BMW extended addressing.

    Sends ATCEA12 / ATCRA612 and checks each is acknowledged (not '?'/error).
    A clone that cannot do CAN extended addressing answers '?' -- BMW enhanced
    is then unreachable and the caller must abort BMW mode. Read-only: these
    are adapter-config commands, never a vehicle write."""
    for c in ("ATCEA12", "ATCRA612"):
        raw = sess.cmd(c).upper()
        if "?" in raw or "ERROR" in raw:
            return False
    # Clear the probe state so it does not bleed into the first real header.
    sess.cmd("ATCEA")
    sess.cmd("ATCRA")
    sess.cur_header = None
    return True


def default_session_gate(sess: ElmSession, header: cat.Header,
                         did: str = "22DA25") -> str:
    """THE CRUX test. Read one confirmed default-session DID and classify.

    'positive'  -> the module answered; the read path is proven, proceed.
    'negative'  -> module alive but the DID is gated/unsupported (7F 22 xx);
                   reaching it would need service 0x10, a DESIGN DECISION for
                   the user -- NOT auto-handled here.
    'silent'    -> no answer / link problem: addressing wrong or adapter weak."""
    sess.set_header(header)
    r = sess.probe(did)
    if r.cls is Cls.POSITIVE:
        return "positive"
    if r.cls in _NEG:
        return "negative"
    return "silent"


@dataclass
class Hit:
    header: str
    request: str
    raw: str
    payload_hex: str
    length: int


def run_sweep(sess: ElmSession, census: dict, preset: cat.VehiclePreset,
              blocks: list[cat.Block] | None = None,
              progress=None) -> dict:
    """Sweep each configured block at every header the census found alive.

    The header is set ONCE per block, not per probe — with ~256 probes per
    block that halves the command count and is most of the speed advantage
    over the on-device scanner it replaces.

    Same OSError discipline as run_census, and for the same reason: a link
    that dies 40 probes into a 256-probe block must not silently read as
    "the rest of this block is unsupported" (indistinguishable from a real
    negative sweep) or crash the whole sweep outright depending on exactly
    which call happened to fail. set_header() and every probe() call inside
    the sweep are guarded by a single try/except around the full
    header/block/request nest: on OSError, the sweep stops immediately and
    returns whatever hits/negatives/probes were genuinely collected before
    the fault, with "aborted"/"error" set — the same keys, same meaning, as
    run_census, so a caller handles either stage's truncation identically.
    Everything collected before the failure is real and is kept; nothing
    after it is inferred.
    """
    cat.validate_preset(preset)
    use_blocks = blocks if blocks is not None else preset.blocks
    alive = [r for r in census["headers"] if r.alive]
    hits: list[Hit] = []
    negatives = 0
    errors = 0
    probes = 0
    aborted = False
    error: str | None = None

    try:
        for row in alive:
            for block in use_blocks:
                sess.set_header(row.header)        # once per (header, block)
                for req in block.requests():
                    r = sess.probe(req)
                    probes += 1
                    if r.cls is Cls.POSITIVE:
                        hits.append(Hit(row.header.name, req, r.raw,
                                        r.payload.hex().upper(), len(r.payload)))
                    elif r.cls in _NEG:
                        negatives += 1
                    elif r.cls is Cls.ELM_ERROR:
                        # A transport fault the link recovered from (no
                        # OSError raised). Without this, an ELM_ERROR
                        # silently inflates neither hits nor negatives --
                        # it just adds to "probes" -- so a sweep over a
                        # faulting link reads exactly like a clean negative
                        # sweep ("0 hits / N probes, 0 negatives"). Count it
                        # so that misreading is impossible.
                        errors += 1
                    if progress:
                        progress(row.header.name, block.name, req, r.cls.name, len(hits))
    except OSError as exc:
        aborted = True
        error = str(exc) or exc.__class__.__name__

    return {"hits": hits, "probes": probes, "negatives": negatives, "errors": errors,
            # NOTE: "_targeted" (not "_reached"): unlike census's "headers"
            # (objects for headers actually REACHED before any truncation),
            # these are the full set sweep MEANT to cover regardless of how
            # far it got -- same key name as census would have meant a
            # different meaning AND a different type (strings, not
            # HeaderResult objects). Detect truncation via "aborted", never
            # by these lists' length.
            "headers_targeted": [r.header.name for r in alive],
            "blocks_targeted": [b.name for b in use_blocks],
            "aborted": aborted, "error": error}


def discover_headers(census: dict, headers: "list[str] | None" = None):
    """Choose which census headers `run_discover` will probe, and say why.

    Split out of run_discover so the CLI can price a run before starting it:
    the estimate and the walk must never disagree about the header count, and
    they will if each derives it separately.

    The default is EVERY alive header, and that is deliberate. The tempting
    optimisation -- probe only the functional broadcast, since a broadcast is
    answered by every module that implements the DID -- is not safe, because
    whether enhanced Mode 22 answers a broadcast at all is vehicle-specific:

      * BMW F10: 462 Mode-22 DIDs answered on the 7DF broadcast (sweep of
        2026-08-24). Broadcast-only would have found everything.
      * Jeep WS: the confirmed DIDs are physical -- gear 22051A and ATF 2204FE
        both at 29-bit 18DA18F1, with the entire 11-bit path dead. The HIL
        emulator models the same shape on purpose (gm_sierra answers 221940 at
        the transmission's 7E2, not at 7E0 and not on a broadcast).

    So a broadcast-only default finds everything on one of those cars and
    NOTHING on the other -- and "0 blocks" on an unlisted vehicle reads as "this
    car has no enhanced data", which is the exact false negative this tool
    refuses to manufacture elsewhere. Cost is handled by pricing the run up
    front and letting the caller narrow with `headers`, not by guessing.

    Returns (rows, scope) where scope is "explicit" | "all-alive".
    """
    alive = [r for r in census["headers"] if r.alive]
    if headers is not None:
        want = set(headers)
        return [r for r in alive if r.header.name in want], "explicit"
    return alive, "all-alive"


def run_discover(sess: ElmSession, census: dict,
                 offsets: "tuple[int, ...]" = cat.DISCOVER_OFFSETS,
                 service: int = 0x22, lo: int = 0x00, hi: int = 0xFF,
                 headers: "list[str] | None" = None,
                 progress=None) -> dict:
    """Find which Mode-22 blocks a vehicle implements, with no preset to go on.

    This is the stage for a car that is not in the catalog. `run_sweep` can only
    sweep blocks a preset already lists, so on an unlisted make there is nothing
    to sweep; discovery is what produces that list. It probes a handful of
    offsets in each of the 256 candidate blocks (`service`<<8 | high byte) and
    reports every block where at least one probe drew a response. The caller
    then runs a normal sweep over those blocks only.

    A block counts as PRESENT only on a POSITIVE reply. This is deliberately NOT
    the rule run_census uses for header liveness, and the difference matters.

    For a HEADER, a `7F 22 31` is decisive: something received the request, so
    the addressing is right. For a BLOCK it proves nothing, because a module
    that implements Mode 22 answers `7F 22 31` to every unsupported DID across
    the whole 16-bit space -- the NAK says "this module speaks Mode 22", not
    "this block is populated".

    An earlier version of this function scored negatives as block evidence, by
    analogy to the census rule. A Subaru run on 2026-08-25 falsified it flatly:
    256 of 256 candidate blocks were reported present, while only 4 held any
    data. The full sweep then chased 252 phantom blocks -- 65536 probes rather
    than 1024 -- and was killed before it reached the real ones.

    The cost of requiring a positive is recall: a populated block whose probed
    offsets are all unimplemented is missed. DISCOVER_OFFSETS is what keeps that
    small, and the same Subaru run supports it -- all four real blocks had a hit
    at 0x00, and 0x00-0x03 alone would have found every one.

    Negatives are still counted and reported, but as a per-header fact
    ("speaks_mode22"), which is what they actually establish.

    By DEFAULT this probes every alive header, because whether enhanced Mode 22
    answers a functional broadcast is vehicle-specific -- see discover_headers()
    for the BMW/Jeep split that rules out a broadcast-only shortcut. Pass
    `headers` to narrow it; "header_scope" in the result says which rule applied.

    The header is set ONCE per header, not per block: unlike run_sweep, which
    walks 256 consecutive requests inside one block, discovery walks one offset
    across every block, so there is no block boundary to re-key on. That is 1
    set_header() per alive header instead of 256.

    Same OSError discipline as run_census and run_sweep, for the same reason: a
    link that dies partway must not read as "the remaining blocks are
    unimplemented", which is indistinguishable from a real negative result.
    On OSError the walk stops and returns what was genuinely collected with
    "aborted"/"error" set -- the same keys, same meaning, as the other stages.
    Blocks found before the fault are real and are kept; nothing after it is
    inferred.

    NOTE: `service` is a parameter but only Mode 22 is a legal value here, and
    validate_request enforces that -- it is spelled out rather than hardcoded so
    the request construction below reads honestly, not to invite Mode 2E.
    """
    width = 2 if service <= 0xFF else 3
    reqs_by_block: dict[int, list[str]] = {}
    for high in range(lo, hi + 1):
        prefix = (service << 8) | high
        reqs_by_block[prefix] = [f"{prefix:0{width * 2}X}{off:02X}" for off in offsets]

    # Validate EVERY request before a single one goes out, not lazily as each
    # is sent: a bad `service` must fail before the link is touched, the same
    # way validate_preset runs at startup rather than mid-sweep.
    for reqs in reqs_by_block.values():
        for r in reqs:
            cat.validate_request(r)

    alive, scope = discover_headers(census, headers)
    hits: list[Hit] = []
    found: dict[str, set[str]] = {}     # block name -> header names that saw it
    speaks: set[str] = set()            # headers that NAKed, i.e. implement Mode 22
    negatives = 0
    errors = 0
    probes = 0
    aborted = False
    error: str | None = None

    try:
        for row in alive:
            sess.set_header(row.header)              # once per header
            for prefix, reqs in reqs_by_block.items():
                name = f"{prefix:0{width * 2}X}xx"
                for req in reqs:
                    r = sess.probe(req)
                    probes += 1
                    present = False
                    if r.cls is Cls.POSITIVE:
                        hits.append(Hit(row.header.name, req, r.raw,
                                        r.payload.hex().upper(), len(r.payload)))
                        present = True
                    elif r.cls in _NEG:
                        negatives += 1
                        # Evidence about the MODULE, not this block -- see the
                        # docstring. Recorded per header, never as a block hit.
                        speaks.add(row.header.name)
                    elif r.cls is Cls.ELM_ERROR:
                        # Counted, never inferred from -- same reasoning as
                        # run_sweep: without this an ELM_ERROR would inflate
                        # neither hits nor negatives and a faulting link would
                        # read exactly like a clean "nothing here" result.
                        errors += 1
                    if present:
                        found.setdefault(name, set()).add(row.header.name)
                    if progress:
                        progress(row.header.name, name, req, r.cls.name, len(found))
    except OSError as exc:
        aborted = True
        error = str(exc) or exc.__class__.__name__

    blocks = [cat.Block(name, int(name[:-2], 16),
                        note=f"discovered: answered at {', '.join(sorted(found[name]))}")
              for name in sorted(found)]
    return {"blocks": blocks, "hits": hits, "probes": probes,
            "negatives": negatives, "errors": errors,
            "block_headers": {k: sorted(v) for k, v in found.items()},
            # Headers that answered a Mode-22 request with a negative response.
            # They implement the service; it says nothing about which blocks are
            # populated. An empty list next to 0 blocks means something quite
            # different from a full one: the former is "nothing spoke Mode 22",
            # the latter "Mode 22 works here but these offsets found nothing".
            "speaks_mode22": sorted(speaks),
            # "_targeted", not "_reached" -- the full set discovery MEANT to
            # cover, regardless of how far it got. Same convention as run_sweep;
            # detect truncation via "aborted", never via these lengths.
            "headers_targeted": [r.header.name for r in alive],
            "blocks_targeted": len(reqs_by_block),
            "offsets_probed": list(offsets),
            "header_scope": scope,
            "aborted": aborted, "error": error}


# Minimal decoders for the anchor PIDs -- enough to make correlation
# meaningful. Everything else (the candidate hits) is stored as raw hex and
# decoded offline: a wrong decode guess made in the field must not destroy
# data that a better guess at home could still use.
def _decode_anchor(name: str, payload: bytes) -> float | str:
    if not payload:
        return ""
    a = payload[0]
    b = payload[1] if len(payload) > 1 else 0
    if name == "rpm":     return ((a << 8) + b) / 4.0
    if name == "speed":   return float(a)
    if name == "load":    return a * 100.0 / 255.0
    if name == "coolant": return float(a - 40)
    if name == "maf":     return ((a << 8) + b) / 100.0
    if name == "baro":    return float(a)
    if name == "ambient": return float(a - 40)
    return ""


def filter_hits_by_pids(hits: "list[Hit]", pids: str) -> "tuple[list[Hit], list[str]]":
    """Keep only hits whose request is in the comma-separated `pids` list
    (case-insensitive). Returns (kept_hits, missing_requests) — missing are
    requested PIDs not present in the sweep. Empty/None `pids` keeps all hits.

    Focused logging: a drive logs every kept PID each cycle, so logging ~20-30
    candidate DIDs instead of a 400+ sweep gives hundreds of samples per PID in
    15 min instead of a few dozen — which is what correlation needs."""
    if not pids:
        return list(hits), []
    wanted = {p.strip().upper() for p in pids.split(",") if p.strip()}
    kept = [h for h in hits if h.request.upper() in wanted]
    missing = sorted(wanted - {h.request.upper() for h in kept})
    return kept, missing


def estimate_samples_per_pid(n_polls: int, minutes: float = 15.0,
                             per_poll_ms: float = 60.0) -> int:
    """Rough projection of samples-per-column for a `minutes`-long drive: one
    cycle polls all n_polls columns at ~per_poll_ms each (ELM round-trip), so
    samples ~= (minutes*60) / (n_polls * per_poll_ms / 1000). Guidance only."""
    cycle_s = max(n_polls * per_poll_ms / 1000.0, 1e-3)
    return int(minutes * 60.0 / cycle_s)


def run_triage(sess: ElmSession, hits: "list[Hit]", allowed_headers=None,
               progress=None) -> dict:
    """Re-probe every sweep hit once and sort the list by what it is worth logging.

    THE PROBLEM THIS SOLVES. A sweep on an unlisted vehicle returns hundreds of
    DIDs -- 462 on the F10. A drive log cannot carry them: every value is a
    round trip, so polling 462 DIDs gives each one a sample every several
    minutes, which is worse than useless because `correlate` then ranks noise.
    Somebody has to choose maybe 30. Until now that somebody was a human reading
    hex, and the choice decided whether a drive was worth taking.

    THE ONE THING A SECOND PROBE ESTABLISHES. A DID whose bytes CHANGED between
    two reads seconds apart is, necessarily, live -- something in the car is
    updating it. That is a positive fact, and it is the only one available
    without moving the car. A DID that did not change is NOT thereby static:
    coolant at thermal equilibrium, or road speed at a standstill, hold still
    too. So this ranks, it does not delete: every hit is returned, in an order,
    with the evidence attached.

    Three buckets, most-worth-logging first:

      "moved"       bytes differ between the two reads. A live signal, certain.
      "static"      identical both times. Could be a sensor at equilibrium, or
                    could be a VIN fragment, a counter, or a config byte.
      "unpopulated" all 0x00 or all 0xFF both times. Answering, but carrying
                    nothing -- 224404-224407 on the F10 read 0000 every time.

    Duplicates are collapsed within a bucket: on the F10, 225817 and 2258EB were
    byte-identical on 99.51% of 1427 logged rows -- the same signal wearing two
    DIDs, which cost a drive to discover. Two DIDs identical on BOTH probes here
    are flagged as `duplicate_of` and dropped from the recommended list, so the
    drive spends its budget on distinct signals.

    Cost is one extra probe per hit -- 462 on the F10, about 40 s at the ~12
    probes/s a BLE link sustains. It is the cheapest evidence in the pipeline.

    NOT A DRIVE REPLACEMENT. Which of the movers is oil temperature still takes
    `log` + `correlate` and a thermal ramp. This only decides what gets logged.
    """
    # Same header-scoping rule as run_log: resolve a hit's header name against
    # the caller's header set, so a tampered sweep.json cannot steer a probe at
    # a module the preset never sanctioned.
    pool = allowed_headers if allowed_headers is not None else cat.all_known_headers()
    hdr_by_name = {h.name: h for h in pool}

    order = {"moved": 0, "static": 1, "unpopulated": 2}
    rows: list[dict] = []
    aborted = False
    error: str | None = None
    header_now: str | None = None

    try:
        for i, h in enumerate(hits):
            hx = hdr_by_name.get(h.header)
            if hx is None:                    # out of scope -> not probed at all
                continue
            if header_now != h.header:
                sess.set_header(hx)           # once per header, as run_sweep does
                header_now = h.header
            r = sess.probe(h.request)
            second = r.payload.hex().upper() if r.cls is Cls.POSITIVE else None
            blank = _is_blank(h.payload_hex) and _is_blank(second)
            if blank:
                kind = "unpopulated"
            elif second is None or second == h.payload_hex:
                kind = "static"
            else:
                kind = "moved"
            rows.append({"header": h.header, "request": h.request,
                         "first": h.payload_hex, "second": second,
                         "kind": kind, "hit": h})
            if progress:
                progress(i + 1, len(hits), h.request, kind)
    except OSError as e:
        # Same discipline as every other stage: a link that died partway must
        # not leave the untested remainder looking like a measured "static".
        aborted, error = True, str(e)

    # Collapse DIDs that read identically on BOTH probes -- the same signal
    # under two names. Keep the first, point the rest at it.
    seen: dict[tuple, dict] = {}
    for r in rows:
        if r["kind"] == "unpopulated":
            continue
        key = (r["header"], r["first"], r["second"])
        if key in seen:
            r["duplicate_of"] = seen[key]["request"]
        else:
            seen[key] = r

    rows.sort(key=lambda r: (order[r["kind"]], "duplicate_of" in r, r["request"]))
    recommended = [r["hit"] for r in rows
                   if r["kind"] == "moved" and "duplicate_of" not in r]
    return {"probed": len(rows), "rows": rows, "recommended": recommended,
            "moved": sum(1 for r in rows if r["kind"] == "moved"),
            "static": sum(1 for r in rows if r["kind"] == "static"),
            "unpopulated": sum(1 for r in rows if r["kind"] == "unpopulated"),
            "duplicates": sum(1 for r in rows if "duplicate_of" in r),
            "aborted": aborted, "error": error}


def _is_blank(payload: "str | None") -> bool:
    """True for a payload that is all zeros or all 0xFF -- answering, carrying nothing."""
    if not payload:
        return True
    p = payload.strip().upper()
    return bool(p) and (set(p) <= {"0"} or set(p) <= {"F"})


def run_log(sess: ElmSession, hits: list[Hit], path: str, hz: float = 1.0,
            duration_s: float | None = None, stop=None,
            allowed_headers=None) -> dict:
    """Round-robin poll the hit list plus the anchors, writing a wide CSV.

    Columns: iso_time, uptime_ms, one RAW HEX column per hit (deliberate --
    see _decode_anchor's docstring above), then decoded anchor columns
    (correlation needs those as numbers). Every row is flushed to disk the
    moment it is written: a lost logging session costs someone a drive, so
    that outweighs the cost of an fsync per row.

    Same OSError discipline as run_census/run_sweep, for the same reason: a
    link that dies mid-drive must not lose the rows already written to
    disk, and the caller must be able to tell a truncated log from a
    completed one. The row under construction when the fault hits is
    incomplete and is discarded -- it was never flushed -- but every row
    completed and flushed before the fault is real and stays on disk.
    "aborted"/"error" are the same keys, same meaning, as the other stages.
    """
    # Reconstruct each hit's header from its name. Scope resolution to the
    # caller-provided header set (the sweep's PRESET headers) so a tampered or
    # shared sweep.json cannot name an unsafe module (e.g. an ADAS 7E4) the
    # preset never declared -- the read-only MODE gate checks the service byte,
    # not the target ECU. Fall back to all_known_headers only when no scope is
    # given (direct/library use). A hit whose header isn't in scope is skipped,
    # never crashed on.
    pool = allowed_headers if allowed_headers is not None else cat.all_known_headers()
    hdr_by_name = {x.name: x for x in pool}
    kept: list[Hit] = []
    hit_headers = []
    dropped: list[str] = []
    for h in hits:
        hx = hdr_by_name.get(h.header)
        if hx is None:
            dropped.append(h.header)
            continue
        kept.append(h)
        hit_headers.append(hx)
    hits = kept
    hit_cols = [f"{h.request}@{h.header}" for h in hits]
    anchor_cols = list(cat.ANCHORS.keys())
    header = ["iso_time", "uptime_ms"] + hit_cols + anchor_cols

    # Header the anchors are asked under. Pinned ONCE for the whole drive.
    #
    # WHY THIS EXISTS: the anchor probes used to carry no set_header at all, so
    # each one inherited whatever header the last hit left selected -- a
    # PHYSICAL ECU address like 7E0. The anchors are generic Mode-01 PIDs and
    # belong on the FUNCTIONAL BROADCAST, addressed to the vehicle rather than
    # to one module. Asking 0110 of an ECU that does not serve it returns
    # nothing, and the anchor was then recorded as unsupported.
    #
    # Cost, measured: on the 2021 F-350 (2026-08-09) maf and ambient logged
    # 0/64 rows, `correlate` called both UNUSABLE, and all 391 candidate
    # columns were scored without airflow or ambient temperature. The Mode-22
    # mirror fallback below restored the DATA but not the diagnosis -- 0110 was
    # never unsupported on that truck, it was misaddressed.
    #
    # Bit width matters: the ford/gm/jeep presets declare BOTH broadcasts
    # (11-bit 7DF and 29-bit 18DB33F1), so pick the one matching the hits being
    # polled. Otherwise a 29-bit drive would swap protocol every cycle to ask
    # on a bus it is not using. Chosen from the hits, not from the session's
    # current protocol, so it is stable for the whole drive rather than a
    # function of whichever hit happened to run last.
    broadcasts = [h for h in pool if "functional broadcast" in h.role]
    anchor_hdr = None
    if broadcasts:
        n29 = sum(1 for h in hit_headers if h.bits == 29)
        want = 29 if n29 * 2 > len(hit_headers) else 11
        anchor_hdr = next((h for h in broadcasts if h.bits == want), broadcasts[0])

    # Request actually used for each anchor. Starts as the generic Mode-01
    # PID and may be swapped for its Mode-22 mirror on the first cycle -- see
    # cat.ANCHOR_MIRRORS and the resolution block in the loop below.
    anchor_req = dict(cat.ANCHORS)
    anchor_fallbacks: list[str] = []
    anchors_resolved = False

    t0 = _time.monotonic()
    period = 1.0 / hz if hz > 0 else 0.0
    rows = 0
    error_polls = 0
    aborted = False
    error: str | None = None

    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = _csv.writer(fh)
        w.writerow(header)
        fh.flush()
        while True:
            if duration_s is not None and (_time.monotonic() - t0) >= duration_s:
                break
            if stop is not None and stop():
                break
            cycle = _time.monotonic()
            row = [datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                   int((cycle - t0) * 1000)]
            try:
                for h, hdr in zip(hits, hit_headers):
                    sess.set_header(hdr)
                    r = sess.probe(h.request)
                    if r.cls is Cls.ELM_ERROR:
                        # Same blank cell a genuinely unsupported PID would
                        # write -- without counting it separately, a link
                        # that faults but never OSErrors produces a
                        # full-length CSV of blanks that `correlate` later
                        # calls "no-signal" throughout, indistinguishable
                        # from the vehicle not supporting the PID.
                        error_polls += 1
                    row.append(r.payload.hex().upper() if r.cls is Cls.POSITIVE else "")
                if anchor_hdr is not None:
                    sess.set_header(anchor_hdr)   # cached; one ATSH per cycle
                for name in cat.ANCHORS:
                    r = sess.probe(anchor_req[name])
                    # Anchor resolution, first cycle only. A silent generic
                    # PID does not mean the vehicle lacks the parameter --
                    # it may only serve it as the J1979 Mode-22 mirror. Try
                    # the mirror once and, if it answers, use it for the
                    # rest of the drive.
                    #
                    # Resolved HERE rather than before the loop so the probe
                    # runs under the same ELM header state the real polls
                    # will use (the last hit's header is still set), instead
                    # of whatever the session happened to default to.
                    #
                    # One-shot by design: paying a second probe every cycle
                    # for a permanently-dead anchor would cost sample density
                    # on every column, which matters far more than recovering
                    # an intermittent anchor.
                    if r.cls is not Cls.POSITIVE and not anchors_resolved:
                        mirror = cat.ANCHOR_MIRRORS.get(name)
                        if mirror and mirror != anchor_req[name]:
                            rm = sess.probe(mirror)
                            if rm.cls is Cls.POSITIVE:
                                anchor_fallbacks.append(
                                    f"{name}: {anchor_req[name]} silent -> {mirror}")
                                anchor_req[name] = mirror
                                r = rm
                    if r.cls is Cls.ELM_ERROR:
                        error_polls += 1
                    row.append(_decode_anchor(name, r.payload) if r.cls is Cls.POSITIVE else "")
                anchors_resolved = True
            except OSError as exc:
                # The row under construction is incomplete -- never written,
                # never flushed -- and is dropped here. Every prior row is
                # already on disk because each one was flushed as it was
                # written, so nothing already-collected is lost.
                aborted = True
                error = str(exc) or exc.__class__.__name__
                break

            w.writerow(row)
            fh.flush()                       # never lose a session to a buffer
            rows += 1

            sleep_for = period - (_time.monotonic() - cycle)
            if sleep_for > 0:
                _time.sleep(sleep_for)

    return {"rows": rows, "columns": header, "path": path,
            "error_polls": error_polls, "aborted": aborted, "error": error,
            "dropped_headers": sorted(set(dropped)),
            "anchor_requests": dict(anchor_req),
            "anchor_header": anchor_hdr.name if anchor_hdr else None,
            "anchor_fallbacks": anchor_fallbacks}
