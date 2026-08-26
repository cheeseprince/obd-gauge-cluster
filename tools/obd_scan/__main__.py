"""CLI: python3 -m obd_scan {census,discover,sweep,log,correlate}"""
import argparse
import json
import os
import sys
from dataclasses import asdict, is_dataclass

import pandas as pd

from . import catalog as cat
from .correlate import analyze, anchor_coverage
from .elm import AdapterUnreachable, ElmSession
from .report import write_report
from .stages import (
    Hit,
    default_session_gate,
    discover_headers,
    estimate_samples_per_pid,
    filter_hits_by_pids,
    probe_bmw_capability,
    read_vin,
    run_census,
    run_discover,
    run_log,
    run_sweep,
)


def _json_default(o):
    if is_dataclass(o):
        return asdict(o)
    return str(o)


def _session(args) -> ElmSession:
    s = ElmSession(args.host, args.port)
    s.connect()
    print(f"adapter: {s.init()}")
    print(f"protocol: {s.detect_protocol()}")
    return s


def _resolve_preset(args, sess):
    """Pick the VehiclePreset to scan with. For `--vehicle auto`, read the VIN
    and map its WMI to a preset; return None (caller aborts cleanly) if the VIN
    can't be read or its WMI has no preset — never silently fall back to a
    default, which is how the wrong preset gets run on the wrong car."""
    if args.vehicle != "auto":
        return cat.PRESETS[args.vehicle]
    choices = ", ".join(sorted(cat.PRESETS))
    vin = read_vin(sess)
    if not vin:
        print(f"AUTO: could not read the VIN (Mode-09 0902). Re-run with an "
              f"explicit --vehicle ({choices}).")
        return None
    name = cat.preset_for_vin(vin)
    if not name:
        print(f"AUTO: VIN {vin} (WMI {vin[:3]}) matches no preset. Re-run with an "
              f"explicit --vehicle ({choices}).")
        return None
    print(f"AUTO: VIN {vin} (WMI {vin[:3]}) -> preset '{name}'")
    return cat.PRESETS[name]


def _report_abort(res: dict) -> None:
    """Print an unmistakable banner when a stage's session was cut short.

    `aborted` is the ONLY reliable truncation signal (see module docstrings
    in stages.py): census's "headers" list only contains headers actually
    reached, while sweep's "headers_targeted"/"blocks_targeted" list the
    full targeted set regardless of how far the sweep got. A summary
    printed after an aborted run must never be allowed to read like a
    completed one, or a user in a driveway sees "alive: (none)" and
    concludes the vehicle exposes nothing, when the real cause was a
    dropped adapter.
    """
    if res.get("aborted"):
        print(f"\n*** SCAN INCOMPLETE -- link failed: {res.get('error')} ***")
        print("*** Results below only cover what was reached before the drop. ***")
        print("*** Reconnect the adapter and re-run this stage. ***")


# A run whose errors clear this fraction of its probes/polls is not a
# negative finding -- it is a transport that could not be trusted, and must
# be flagged with the same urgency as an outright abort (IMPORTANT 1).
# Probe rate used only to turn a probe COUNT into a human estimate. ~10/s is
# this repo's own documented figure -- the sweep section of README.md measures a
# NO DATA round trip at 60-100 ms under ATAT2 adaptive timing -- and it matches
# three BMW F10 BLE logs of 2026-08 independently at 11.9-12.1 probes/s. Kept
# slightly conservative: a positive reply carries more bytes than a NO DATA.
PROBES_PER_SEC = 10.0
DISCOVER_PROBE_WARN = 10_000   # ~17 min at PROBES_PER_SEC: warn before starting, not after
ERROR_FRACTION_ALARM = 0.5


def _report_upstream_incomplete(raw: dict, stage: str) -> None:
    """Warn when the input FILE this stage is about to consume was itself
    truncated (IMPORTANT 4). `census.json`/`sweep.json` faithfully carry
    "aborted": true from the stage that wrote them, but nothing downstream
    read it -- a user whose census aborted could run sweep and get a
    clean-looking result with no reminder the input was incomplete."""
    if raw.get("aborted"):
        print(f"\n*** INPUT INCOMPLETE -- upstream {stage} link failed: "
              f"{raw.get('error')} ***")
        print(f"*** This run only covers what the {stage} stage reached "
              "before it was cut short. ***")


def cmd_census(args):
    s = _session(args)
    preset = _resolve_preset(args, s)             # --vehicle auto reads the VIN here
    if preset is None:
        s.close(); return
    cat.validate_preset(preset)                   # our own preset data (developer guard)
    cat.validate_headers(preset.headers)          # gate THIS preset's headers (IMPORTANT 5)
    if preset.name == "bmw":
        if not probe_bmw_capability(s):
            print("ABORT: adapter cannot do CAN extended addressing (AT CEA) — "
                  "BMW enhanced diagnostics are unreachable with this adapter.")
            s.close(); return
        gate = default_session_gate(s, next(h for h in preset.headers if h.name == "BMW-618"))
        print(f"default-session 22DA25 gate: {gate}")
        if gate == "negative":
            print("  module alive but DA25 is session-gated — reaching it needs "
                  "service 0x10 (a design decision; this tool will not send it).")
        elif gate == "silent":
            print("  no answer — check addressing/adapter before sweeping.")
    res = run_census(s, preset)
    s.close()
    print(f"\n{'header':<12} {'bits':>4}  {'evidence':<9} supported")
    for r in res["headers"]:
        print(f"{r.header.name:<12} {r.bits:>4}  {r.evidence:<9} "
              f"{len(r.supported_pids)} generic PIDs")
    print(f"\nalive: {', '.join(res['alive']) or '(none)'}")
    # IMPORTANT 2: after CRITICAL 1's fix, a wedged/faulting link yields rows
    # with evidence="error" and an EMPTY "alive" list, with res["aborted"]
    # still False (no OSError was ever raised). Without this, the CLI has no
    # way to say "determined nothing" -- the README tells the user to read
    # the "alive:" line first, and a bare "alive: (none)" reads as "this
    # vehicle exposes nothing" instead of "the transport could not be
    # trusted".
    n_err = sum(1 for r in res["headers"] if r.evidence == "error")
    if n_err:
        print(f"*** {n_err} header(s) UNDETERMINED (transport fault) — not a finding. ***")
        if not res["alive"]:
            print("*** No header was successfully determined. Check the adapter link "
                  "and re-run before concluding anything about this vehicle. ***")
    _report_abort(res)
    with open(args.out, "w") as fh:
        json.dump(res, fh, indent=1, default=_json_default)
    print(f"wrote {args.out}")


def _rehydrate_alive_headers(census_raw, preset):
    """Resolve a census's ALIVE header rows back to Header objects, using THIS
    preset's declared headers ONLY (not the global HEADERS_11BIT/29BIT pools).

    Rehydrating from ``preset.headers`` does two things at once:
      (a) includes preset-specific headers like BMW's 612/618, so a sweep whose
          census marks a 6F1 header alive no longer KeyErrors (the same class of
          bug ``all_known_headers()`` fixed for ``run_log``), and
      (b) rejects any header a shared/edited census.json names that this preset
          never declared -- so a tampered census cannot steer the sweep onto an
          unsafe module (e.g. an ADAS 7E4) that the read-only MODE gate alone
          would not catch (it validates the service byte, not the target ECU).

    Returns ``(rows, skipped_names)`` where rows carry ``.alive`` and ``.header``.
    """
    by_name = {h.name: h for h in preset.headers}
    rows, skipped = [], []
    for r in census_raw["headers"]:
        if not r["alive"]:
            continue
        name = r["header"]["name"]
        if name not in by_name:
            skipped.append(name)
            continue
        rows.append(type("Row", (), {"alive": True, "header": by_name[name]})())
    return rows, skipped


def _blocks_from_discover(path: str) -> "list[cat.Block]":
    """Rebuild sweep-able blocks from a discover.json.

    This is what lets an unlisted vehicle go census -> discover -> sweep with no
    source edit at all, which matters for any caller that cannot patch
    catalog.py (a phone app, a fork-less contributor).

    Every rebuilt block is revalidated here rather than trusted. The file is
    ordinary JSON on disk: it can be hand-edited, copied between machines, or
    shared, so a prefix in it is exactly as untrusted as a preset read from a
    shared census.json -- which cmd_sweep already refuses to take on faith.
    Running validate_request over the block's own first request re-applies the
    read-only whitelist, so a doctored prefix naming a write service is rejected
    before the link opens, not after.
    """
    with open(path) as fh:
        raw = json.load(fh)
    out: list[cat.Block] = []
    for b in raw.get("blocks", []):
        try:
            block = cat.Block(str(b["name"]), int(b["prefix"]),
                              lo=int(b.get("lo", 0x00)), hi=int(b.get("hi", 0xFF)),
                              note=str(b.get("note", "")))
        except (KeyError, TypeError, ValueError) as exc:
            raise SystemExit(f"sweep: {path} has an unusable block entry ({exc})") from None
        cat.validate_request(next(iter(block.requests())))    # read-only whitelist
        out.append(block)
    return out


def cmd_sweep(args):
    with open(args.census) as fh:
        census_raw = json.load(fh)
    # --vehicle auto: inherit the preset the census already resolved from the VIN.
    vehicle = census_raw.get("preset") if args.vehicle == "auto" else args.vehicle
    if vehicle not in cat.PRESETS:
        print(f"sweep: census.json has no usable preset ('{vehicle}'); re-run census, "
              f"or pass an explicit --vehicle ({', '.join(sorted(cat.PRESETS))}).")
        return
    if args.vehicle == "auto":
        print(f"AUTO: using preset '{vehicle}' (from census.json)")
    preset = cat.PRESETS[vehicle]
    cat.validate_preset(preset)
    cat.validate_headers(cat.HEADERS_11BIT + cat.HEADERS_29BIT)   # IMPORTANT 5
    _report_upstream_incomplete(census_raw, "census")           # IMPORTANT 4
    # Rehydrate the alive header rows from THIS preset's declared headers only.
    census_rows, skipped = _rehydrate_alive_headers(census_raw, preset)
    for name in skipped:
        print(f"sweep: refusing census header '{name}' -- not declared by preset "
              f"'{vehicle}' (tampered/shared census.json?); skipping it.")
    census = {"headers": census_rows}
    blocks = None
    if args.blocks_from:
        blocks = _blocks_from_discover(args.blocks_from)
        if not blocks:
            print(f"sweep: {args.blocks_from} lists no blocks -- nothing to sweep.")
            return
        print(f"sweeping {len(blocks)} discovered block(s) from {args.blocks_from}: "
              f"{', '.join(b.name for b in blocks)}")
    s = _session(args)

    def progress(header, block, req, cls, hits):
        print(f"\r{header} {block} {req} -> {cls:<18} hits={hits}", end="", flush=True)

    res = run_sweep(s, census, preset, blocks=blocks, progress=progress)
    s.close()
    print(f"\n{len(res['hits'])} hits / {res['probes']} probes "
          f"({res['negatives']} negative responses, {res['errors']} transport errors)")
    # IMPORTANT 1: an ELM_ERROR that never raised OSError still means part of
    # this sweep could not be trusted -- a mostly-erroring run must not read
    # like a negative finding just because "aborted" is False.
    if res["errors"] and res["probes"] and res["errors"] / res["probes"] >= ERROR_FRACTION_ALARM:
        print(f"*** {res['errors']}/{res['probes']} probes hit a transport error "
              "-- this is NOT a negative finding. Check the adapter link. ***")
    _report_abort(res)
    res["preset"] = vehicle   # record the preset so `log --vehicle auto` can scope headers safely
    with open(args.out, "w") as fh:
        json.dump(res, fh, indent=1, default=_json_default)
    print(f"wrote {args.out}")


def _eta(probes: int) -> str:
    """Probe count as a human duration. Minutes below an hour, hours above."""
    secs = probes / PROBES_PER_SEC
    return f"{secs / 60:.0f} min" if secs < 3600 else f"{secs / 3600:.1f} h"


def _parse_offsets(text: str) -> "tuple[int, ...]":
    """Parse "00,01,40" into (0x00, 0x01, 0x40). Hex without 0x, like every
    other PID the tool prints."""
    out = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            v = int(part, 16)
        except ValueError:
            raise SystemExit(f"discover: --offsets got '{part}', which is not hex") from None
        if not 0x00 <= v <= 0xFF:
            raise SystemExit(f"discover: offset {part} is out of range (00-FF)")
        out.append(v)
    if not out:
        raise SystemExit("discover: --offsets is empty")
    return tuple(dict.fromkeys(out))          # de-dupe, keep order


def cmd_discover(args):
    with open(args.census) as fh:
        census_raw = json.load(fh)
    vehicle = census_raw.get("preset") if args.vehicle == "auto" else args.vehicle
    if vehicle not in cat.PRESETS:
        print(f"discover: census.json has no usable preset ('{vehicle}'); re-run census, "
              f"or pass an explicit --vehicle ({', '.join(sorted(cat.PRESETS))}).")
        return
    if args.vehicle == "auto":
        print(f"AUTO: using preset '{vehicle}' (from census.json)")
    preset = cat.PRESETS[vehicle]
    cat.validate_preset(preset)
    cat.validate_headers(cat.HEADERS_11BIT + cat.HEADERS_29BIT)   # IMPORTANT 5
    _report_upstream_incomplete(census_raw, "census")             # IMPORTANT 4
    census_rows, skipped = _rehydrate_alive_headers(census_raw, preset)
    for name in skipped:
        print(f"discover: refusing census header '{name}' -- not declared by preset "
              f"'{vehicle}' (tampered/shared census.json?); skipping it.")
    census = {"headers": census_rows}
    offsets = _parse_offsets(args.offsets)
    want = [h.strip() for h in args.headers.split(",") if h.strip()] or None
    scoped, scope = discover_headers(census, want)
    if not scoped:
        print("discover: no census header matches -- nothing to probe. Check "
              "--headers against the census's alive list.")
        return
    names = ", ".join(r.header.name for r in scoped)
    total = 256 * len(offsets) * len(scoped)
    if total > DISCOVER_PROBE_WARN:
        # Price it honestly BEFORE the link opens. A run this size is measured
        # in hours, and a driver who starts one without knowing that will kill
        # it partway -- producing a truncated result that looks like a finding.
        print(f"*** {total} probes is roughly {_eta(total)} at ~{PROBES_PER_SEC:.0f} probes/s. "
              f"Narrow it with --headers (alive: {names}) or fewer --offsets, "
              f"or plan to leave it running. ***")
    print(f"discovering {len(offsets)} offsets x 256 blocks x {len(scoped)} header(s) "
          f"[{scope}: {names}] = {total} probes "
          f"(~{_eta(total)} at ~{PROBES_PER_SEC:.0f} probes/s)")
    s = _session(args)

    def progress(header, block, req, cls, found):
        print(f"\r{header} {block} {req} -> {cls:<18} blocks={found}", end="", flush=True)

    res = run_discover(s, census, offsets=offsets, headers=want, progress=progress)
    s.close()
    print(f"\n{len(res['blocks'])} blocks / {res['probes']} probes "
          f"({len(res['hits'])} hits, {res['negatives']} negative responses, "
          f"{res['errors']} transport errors)")
    if res["errors"] and res["probes"] and res["errors"] / res["probes"] >= ERROR_FRACTION_ALARM:
        print(f"*** {res['errors']}/{res['probes']} probes hit a transport error "
              "-- this is NOT a negative finding. Check the adapter link. ***")
    _report_abort(res)

    if res["blocks"]:
        # Print the blocks in the exact shape catalog.py wants, so adding a
        # vehicle to the repo is a paste rather than a transcription. This is
        # the point of the stage: a preset is what an unlisted car is missing.
        print("\nCandidate preset blocks -- paste into a VehiclePreset in catalog.py:\n")
        print("    blocks=[")
        for b in res["blocks"]:
            print(f'        Block("{b.name}", 0x{b.prefix:04X}, '
                  f'note="{b.note}"),')
        print("    ],")
        print("\nThen run `sweep` to read every DID in those blocks.")
    else:
        print("\nNo blocks answered. Either this module exposes no Mode-22 data at "
              "these offsets, or the census found the wrong header. Widen with "
              "--offsets before concluding the vehicle has nothing.")

    res["preset"] = vehicle
    with open(args.out, "w") as fh:
        json.dump(res, fh, indent=1, default=_json_default)
    print(f"wrote {args.out}")


def cmd_log(args):
    with open(args.sweep) as fh:
        sweep = json.load(fh)
    _report_upstream_incomplete(sweep, "sweep")                  # IMPORTANT 4
    # Scope header resolution to this sweep's PRESET (from --vehicle, else the
    # preset the sweep recorded) so a tampered/shared sweep.json can't steer the
    # log at an unsafe module (e.g. an ADAS 7E4) the preset never declared.
    veh = sweep.get("preset") if args.vehicle == "auto" else args.vehicle
    preset = cat.PRESETS.get(veh)
    if preset is None:
        print(f"log: sweep.json has no usable preset ('{veh}'); pass an explicit "
              f"--vehicle ({', '.join(sorted(cat.PRESETS))}).")
        return
    cat.validate_preset(preset)
    hits = [Hit(**h) for h in sweep["hits"]]
    hits, missing = filter_hits_by_pids(hits, args.pids)
    if args.pids and missing:
        print(f"--pids: {len(missing)} requested DID(s) not in the sweep (skipped): {', '.join(missing)}")
    if not hits:
        print("nothing to log (empty hit list, or --pids matched no sweep hits).")
        return
    # Sample-density guidance: a dense drive needs FEW PIDs (see filter_hits_by_pids).
    n_polls = len(hits) + len(cat.ANCHORS)
    est = estimate_samples_per_pid(n_polls)
    print(f"logging {len(hits)} PIDs + {len(cat.ANCHORS)} anchors — ~{est} samples/PID projected in a "
          f"15-min drive. Fewer PIDs = denser samples; use --pids to focus on candidates. Ctrl-C to stop.")
    s = _session(args)
    try:
        res = run_log(s, hits, args.out, hz=args.hz, allowed_headers=preset.headers)
    except KeyboardInterrupt:
        res = {"rows": "?", "error_polls": 0, "aborted": False, "error": None, "dropped_headers": []}
    s.close()
    if res.get("dropped_headers"):
        print(f"*** skipped {len(res['dropped_headers'])} hit(s) with unresolvable header(s): "
              f"{', '.join(res['dropped_headers'])} — not logged. ***")
    # An anchor that fell back is still a fully usable anchor, but the reader
    # should know the column came from the Mode-22 mirror rather than the
    # generic PID -- otherwise a later "why does this vehicle answer 22F410
    # but not 0110?" has no record to answer it.
    for line in res.get("anchor_fallbacks", []):
        print(f"anchor fallback -- {line}")
    print(f"\nwrote {args.out} ({res['rows']} rows)")
    # IMPORTANT 1: surfaced the same way as sweep's transport-error count --
    # a blank cell from an ELM_ERROR must not be mistaken for "unsupported".
    if res["error_polls"]:
        print(f"*** {res['error_polls']} poll(s) hit a transport error during this log -- "
              "the corresponding cells are blank but this does NOT mean the vehicle "
              "lacks that PID. ***")
    _report_abort(res)


def cmd_correlate(args):
    df = pd.read_csv(args.csv, dtype=str)
    anchors = [c for c in cat.ANCHORS if c in df.columns]
    hit_cols = [c for c in df.columns
                if c not in ("iso_time", "uptime_ms") and c not in anchors]
    cands = analyze(df, hit_cols, anchors, workers=args.workers)
    cov = anchor_coverage(df, anchors)
    write_report(cands, args.out, pdf=args.pdf, meta={"vehicle": args.csv}, anchor_coverage=cov)
    print(f"wrote {args.out}")
    # Console preview mirrors the report's split. Printing the flat top-10 put
    # nine J1979 mirror columns on screen and the first genuinely enhanced
    # find at the very bottom -- and the console is what gets read first.
    enhanced = [c for c in cands if not c.mirror_of]
    mirror = [c for c in cands if c.mirror_of]
    print(f"\ntop enhanced candidates ({len(enhanced)} of {len(cands)} columns):")
    for c in enhanced[:10]:
        label = c.best_interp.label if c.best_interp else "—"
        print(f"  {c.column:<18} {label:<8} {c.best_anchor:<8} r={c.r:+.2f}  {c.verdict}")
    if mirror:
        print(f"\n{len(mirror)} J1979 F4xx mirror column(s) — the standard PID set "
              f"re-served over Mode 22, not enhanced finds. See the report.")


def main(argv=None):
    p = argparse.ArgumentParser(prog="obd_scan",
                                description="Discover an unknown vehicle's OBD-II PID map.")
    p.add_argument("--host", default="192.168.0.10", help="adapter IP (default: iCar Pro WiFi)")
    p.add_argument("--port", type=int, default=35000)
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("census", help="discover addressing and live modules")
    c.add_argument("--vehicle", choices=sorted([*cat.PRESETS, "auto"]), default="auto")
    c.add_argument("-o", "--out", default="census.json")
    c.set_defaults(func=cmd_census)

    w = sub.add_parser("sweep", help="sweep PID blocks at live headers")
    w.add_argument("--census", default="census.json")
    w.add_argument("--vehicle", choices=sorted([*cat.PRESETS, "auto"]), default="auto")
    w.add_argument("--blocks-from", default="",
                   help="sweep the blocks in a discover.json instead of the preset's "
                        "(for a vehicle with no preset of its own)")
    w.add_argument("-o", "--out", default="sweep.json")
    w.set_defaults(func=cmd_sweep)

    d = sub.add_parser("discover",
                       help="find Mode-22 blocks on a vehicle with no preset")
    d.add_argument("--census", default="census.json")
    d.add_argument("--vehicle", choices=sorted([*cat.PRESETS, "auto"]), default="auto")
    d.add_argument("--offsets",
                   default=",".join(f"{o:02X}" for o in cat.DISCOVER_OFFSETS),
                   help="hex offsets to probe in each block (default: %(default)s)")
    d.add_argument("--headers", default="",
                   help="comma-separated census header names to probe "
                        "(default: the functional broadcasts the census found alive)")
    d.add_argument("-o", "--out", default="discover.json")
    d.set_defaults(func=cmd_discover)

    lg = sub.add_parser("log", help="log the hit list during a drive")
    lg.add_argument("--sweep", default="sweep.json")
    lg.add_argument("--vehicle", choices=sorted([*cat.PRESETS, "auto"]), default="auto",
                    help="scope header resolution to this preset (default: the preset the sweep recorded)")
    lg.add_argument("--pids", default="",
                    help="comma-separated DIDs to log, e.g. 22DA25,22DA12 (default: all sweep hits). "
                         "Log ~20-30 candidates for dense samples on a 15-20 min drive.")
    lg.add_argument("--hz", type=float, default=1.0)
    lg.add_argument("-o", "--out", default="drive.csv")
    lg.set_defaults(func=cmd_log)

    r = sub.add_parser("correlate", help="rank candidates against anchors")
    r.add_argument("csv")
    r.add_argument("-o", "--out", default="report.md")
    r.add_argument("--pdf", action="store_true")
    r.add_argument("--workers", type=int, default=min(os.cpu_count() or 1, 4))
    r.set_defaults(func=cmd_correlate)

    args = p.parse_args(argv)
    try:
        args.func(args)
    except cat.UnsafeRequest as e:
        print(f"REFUSED: {e}", file=sys.stderr)
        return 2
    except AdapterUnreachable as e:
        # Distinct exit code from REFUSED (2): a link problem is a "try again
        # once the adapter is up", not a safety refusal. The diagnosis is
        # pre-formatted and multi-line -- print it verbatim, no traceback.
        print(f"\nADAPTER NOT REACHED\n\n{e}", file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        # Backstop for census/sweep. cmd_log handles its own Ctrl-C (that is
        # the documented way to end a drive log, and it keeps the CSV); these
        # two just abandon the stage, and a traceback makes a deliberate Ctrl-C
        # in a parking lot look like a crash.
        print("\ninterrupted — stage abandoned, no output file written.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
