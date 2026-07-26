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

    t0 = _time.monotonic()
    period = 1.0 / hz if hz > 0 else 0.0
    rows = 0
    error_polls = 0
    aborted = False
    error: str | None = None

    with open(path, "w", newline="") as fh:
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
                for name, req in cat.ANCHORS.items():
                    r = sess.probe(req)
                    if r.cls is Cls.ELM_ERROR:
                        error_polls += 1
                    row.append(_decode_anchor(name, r.payload) if r.cls is Cls.POSITIVE else "")
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
            "dropped_headers": sorted(set(dropped))}
