"""
Turn "this PID answers" into "this PID is probably EGT".

You do not know an unknown PID's byte layout, so do not assume one: enumerate
every plausible reading of the payload (u8 at each offset, u16 big-endian at
each offset pair, s16 where sign is plausible) and correlate EACH against EACH
anchor. Report the best-scoring pair.

This automates the reasoning that identified oil pressure on the GM truck by
hand.
"""
import os
from dataclasses import dataclass
from multiprocessing import Pool

import numpy as np
import pandas as pd

from . import catalog as cat

SENTINELS = {"FF", "00", "FFFF", "0000", "FFFFFF", "FFFFFFFF"}


def mirrored_pid(column: str) -> str | None:
    """The generic Mode-01 request a `22F4xx` column re-serves, else None.

    SAE J1979 reserves DIDs F400-F4FF for the Mode-01 PIDs served over
    Mode 22, so `22F405` carries the same bytes as `0105`. A sweep of the
    F4xx block therefore rediscovers the STANDARD PID set -- those hits are
    real data, but they are not enhanced-diagnostics finds, and on the 2021
    F-350 they occupied 9 of the top 10 rows of the ranking while the two
    parameters that were actually confirmed sat at ranks 10 and 28.

    Column names are "<request>@<header>", e.g. "22F405@7E0".
    """
    req = column.split("@", 1)[0].upper()
    if len(req) == 6 and req.startswith("22F4"):
        return "01" + req[4:6]
    return None

# A drive log at ~1 Hz yields hundreds to thousands of rows per column, so a
# correlation with fewer than this many overlapping (both-valid) samples is
# not evidence of anything -- on tiny n, |r| is dominated by chance and a
# near-perfect coincidence arises routinely. Below this floor there is no
# reliable r to tier at all -- see "too-few-samples" below.
MIN_SAMPLES = 30

# CRITICAL 2 fix: analyze() takes the MAX over every (interpretation, anchor)
# pair -- 70 pairs for a 4-byte payload, 154 for 8 bytes (see
# interpretations()). A high max-|r| on PURE NOISE is exactly what that
# search predicts, and n cannot warn anyone about it (n=600 looks just as
# reassuring on noise as on a real signal). A bare "correlated"/"no-signal"
# split let 12 of 12 pure-noise columns report "correlated" in the
# demonstration that motivated this fix. Two floors instead of one:
MIN_R_STRONG = 0.90   # report as "correlated" -- a real identification lead
MIN_R_WEAK   = 0.60   # report as "weak" -- worth a look, not a finding
# Below MIN_R_WEAK -> "no-signal". Between the two floors -> "weak".
#
# This does NOT fully close the hazard: two unrelated smooth, slowly-varying
# signals (e.g. a slow sensor drift and a coolant warm-up curve) were
# demonstrated to correlate at r=0.9978, n=600 -- comfortably above
# MIN_R_STRONG. A tiered floor cannot distinguish "these two curves happen
# to both rise" from "these two curves are the same physical quantity". See
# report.py's legend: for temperature/pressure candidates the cold-start
# comparison, not r, is the actual evidence.


@dataclass(frozen=True)
class Interp:
    offset: int
    width: int
    signed: bool
    label: str


def interpretations(nbytes: int) -> list[Interp]:
    """Every plausible scalar reading of an nbytes payload."""
    out: list[Interp] = []
    for off in range(nbytes):
        out.append(Interp(off, 1, False, f"u8@{off}"))
    for off in range(nbytes - 1):
        out.append(Interp(off, 2, False, f"u16@{off}"))
    for off in range(nbytes - 1):
        out.append(Interp(off, 2, True, f"s16@{off}"))
    return out


def decode_series(hexes, interp: Interp) -> np.ndarray:
    """Decode a column of hex strings under one interpretation. Rows too short
    (or blank) become NaN rather than raising."""
    vals = np.full(len(hexes), np.nan)
    for i, h in enumerate(hexes):
        if not isinstance(h, str) or not h:
            continue
        # A row whose ENTIRE cell is sentinel padding (e.g. "FF", "FFFF")
        # carries no real reading -- treat it as missing, not as data. This
        # is deliberately conservative: it only fires when the whole cell
        # matches, never for an 0xFF byte embedded within a longer payload
        # (that can be legitimate signal, not padding).
        if h in SENTINELS:
            continue
        try:
            raw = bytes.fromhex(h)
        except ValueError:
            continue
        end = interp.offset + interp.width
        if len(raw) < end:
            continue
        v = int.from_bytes(raw[interp.offset:end], "big",
                           signed=interp.signed)
        vals[i] = v
    return vals


@dataclass
class Candidate:
    column: str
    best_interp: Interp | None
    best_anchor: str
    r: float
    vmin: float
    vmax: float
    # "correlated" | "weak" | "no-signal" | "too-few-samples" | "constant"
    # | "sentinel" | "mirror-tautology"
    verdict: str
    n: int = 0               # overlapping (both-valid) samples the winning r was computed over
    # How many (interpretation, anchor) pairs this column's search actually
    # evaluated -- interpretations(nbytes) x len(anchor_cols). r is a MAX
    # over this many candidate pairs, which is why it is optimistically
    # biased (CRITICAL 2). 0 for verdicts decided before any search ran
    # (constant/sentinel/no-data).
    pairs_searched: int = 0
    # The generic Mode-01 request this column re-serves, if it is a J1979
    # F4xx mirror (see mirrored_pid). None for enhanced/proprietary PIDs --
    # which is what a sweep of an unmapped vehicle is actually looking for.
    mirror_of: str | None = None


def _verdict_static(series: pd.Series) -> str | None:
    """Classify columns that need no correlation at all."""
    vals = [v for v in series if isinstance(v, str) and v]
    if not vals:
        return "no-signal"
    uniq = set(vals)
    if uniq <= SENTINELS:
        return "sentinel"
    if len(uniq) == 1:
        return "constant"
    return None


def analyze(df: pd.DataFrame, hit_cols: list[str], anchor_cols: list[str],
            workers: int | None = None) -> list[Candidate]:
    """Rank every hit column by its best (interpretation, anchor) correlation.

    Columns are independent, so they are analyzed in parallel — a real sweep
    yields 100+ candidates and each is scored against every interpretation ×
    anchor pair.
    """
    anchors = {a: pd.to_numeric(df[a], errors="coerce").to_numpy(dtype=float)
               for a in anchor_cols}
    jobs = [(col, df[col].tolist(), anchors) for col in hit_cols]

    n = workers if workers is not None else min(os.cpu_count() or 1, 4)
    if n > 1 and len(jobs) > 1:
        with Pool(min(n, len(jobs))) as pool:
            out = pool.map(_analyze_column, jobs)
    else:
        out = [_analyze_column(j) for j in jobs]

    out.sort(key=lambda c: abs(c.r), reverse=True)
    return out


def _analyze_column(job: tuple[str, list, dict]) -> Candidate:
    """Score one candidate column. Module-level so it is picklable for Pool."""
    col, values, anchors = job
    mirror_of = mirrored_pid(col)
    series = pd.Series(values)
    static = _verdict_static(series)
    if static:
        return Candidate(col, None, "", 0.0, float("nan"), float("nan"), static,
                         mirror_of=mirror_of)

    nbytes = max((len(v) // 2 for v in values if isinstance(v, str) and v), default=0)
    interps = interpretations(nbytes)
    # The full search width this column's r is a MAX over -- interpretations
    # x anchors. Recorded regardless of outcome so the report can show it
    # even for a column that ends up "no-signal" or "too-few-samples".
    pairs_searched = len(interps) * len(anchors)

    best = (0.0, None, "")
    best_vals = None
    best_n = 0
    # Best (highest) overlap seen across ANY pair, even ones that never hit
    # MIN_SAMPLES or never hit hit a usable r -- this is what lets
    # "too-few-samples" (IMPORTANT 6) report a true best-observed n instead
    # of silently reading as "no-signal".
    max_n_seen = 0
    for interp in interps:
        vals = decode_series(values, interp)
        if np.all(np.isnan(vals)):
            continue
        for anchor, ref in anchors.items():
            mask = ~np.isnan(vals) & ~np.isnan(ref)
            n_overlap = int(mask.sum())
            if n_overlap > max_n_seen:
                max_n_seen = n_overlap
            if n_overlap < MIN_SAMPLES:
                continue
            # Check BOTH sides' post-mask variance. The anchor's NaN pattern
            # can strip out every varying row of vals (or vice versa) even
            # though each looked fine over its own full column -- feeding a
            # zero-variance array into corrcoef divides by zero and spams
            # RuntimeWarning across a multi-hundred-column sweep.
            if np.std(vals[mask]) == 0 or np.std(ref[mask]) == 0:
                continue
            r = float(np.corrcoef(vals[mask], ref[mask])[0, 1])
            if abs(r) > abs(best[0]):
                best = (r, interp, anchor)
                best_vals = vals[mask]
                best_n = n_overlap

    if best[1] is None:
        # No (interpretation, anchor) pair ever reached MIN_SAMPLES overlap.
        # This is NOT "answered, but no interpretation varied usefully" --
        # it is "we never had enough overlapping data to judge" (IMPORTANT
        # 6), e.g. an intermittently-answering PID. Report the distinction
        # and carry the best n actually observed, even though it fell short.
        if max_n_seen > 0:
            return Candidate(col, None, "", 0.0, float("nan"), float("nan"),
                             "too-few-samples", n=max_n_seen, pairs_searched=pairs_searched,
                             mirror_of=mirror_of)
        return Candidate(col, None, "", 0.0, float("nan"), float("nan"), "no-signal",
                         pairs_searched=pairs_searched, mirror_of=mirror_of)

    r, interp, anchor = best
    if abs(r) >= MIN_R_STRONG:
        verdict = "correlated"
    elif abs(r) >= MIN_R_WEAK:
        verdict = "weak"
    else:
        verdict = "no-signal"
    # A mirror column whose winning anchor IS the PID it mirrors was compared
    # against itself: `22F405` vs the coolant anchor `0105` is the same
    # sensor on both axes, and its r=0.999 says nothing whatsoever about the
    # vehicle. Naming it is strictly better than letting it top the ranking
    # as "correlated".
    if mirror_of and cat.ANCHORS.get(anchor) == mirror_of:
        verdict = "mirror-tautology"
    return Candidate(col, interp, anchor, r,
                     float(np.min(best_vals)), float(np.max(best_vals)), verdict,
                     n=best_n, pairs_searched=pairs_searched, mirror_of=mirror_of)


def anchor_coverage(df: pd.DataFrame, anchor_cols: list[str]) -> dict[str, tuple[int, int]]:
    """Per-anchor (valid_rows, total_rows) over the whole drive log.

    IMPORTANT 7: an all-blank anchor (a vehicle may not support ambient-air
    0146 or baro 0133) contributes nothing to any correlation and is never
    mentioned today -- every verdict that would have used it silently
    degrades instead. In the extreme, every anchor blank reads as "this
    vehicle has nothing identifiable" rather than "the anchors themselves
    were unusable". Surfacing this is cheap and belongs at the top of the
    report, before any per-column verdict is read.
    """
    total = len(df)
    out: dict[str, tuple[int, int]] = {}
    for a in anchor_cols:
        valid = int(pd.to_numeric(df[a], errors="coerce").notna().sum()) if a in df.columns else 0
        out[a] = (valid, total)
    return out
