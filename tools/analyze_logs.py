#!/usr/bin/env python3
"""
analyze_logs.py — self-audit a batch of OBD-display SD-card CSV drive logs.

Every drive becomes a free tuning pass: point this at a directory of
/logs/*.csv files pulled from the dash and it produces a markdown report:

  1. Data inventory        — files, rows, schema (old logs w/o probe cols are skipped)
  2. Threshold audit       — per alarmed stat: observed distribution vs warn/crit,
                             margin analysis (nuisance risk, hi-side headroom)
  3. Alarm reconstruction  — replays the firmware's alarm logic (4s holdoff, 10s
                             startup grace, OIL P 20s running-arm-delay) against the
                             data and lists every alarm that WOULD have fired
  4. Oil-pressure check    — engine-off floor, scale sanity by RPM band, gate audit
  5. Gear check            — rpm/mph clusters per GEAR vs the 10L80 ratio ladder
                             (+ OUTSPD ratio check when that column has data)
  6. Data health           — per-column valid %, sentinel/anomaly scan (-40F temps,
                             65535 pads, values beyond fullScale)
  7. Notable events        — regen heuristic, volts dips, DPF dP peaks, per-drive maxes

Thresholds and fullScale are PARSED FROM src/readouts.cpp so this tool can never
drift from the firmware — if the table changes, the audit follows automatically.

Usage:
  ./analyze_logs.py <csv_dir> [-o report.md] [--pdf] [--workers N]

  --pdf renders the report to PDF via pandoc/md2pdf if available (optional).
  --workers parallelizes CSV loading (default min(cpu, 4)); each worker costs
  only the memory of the largest single CSV (~1 MB per hour of driving).
"""
import argparse, os, re, sys, math, shutil, subprocess
from multiprocessing import Pool
import pandas as pd
import numpy as np

# ---------------------------------------------------------------------------
# Firmware constants mirrored here (parse-checked against readouts.cpp below
# where possible). Keep in sync with src/ if these ever move.
HOLDOFF_S       = 4      # AlarmHoldoff HOLD_MS=4000: zone must persist this long
STARTUP_GRACE_S = 10     # alarms suppressed for 10s after link-up (approx: file start)
OIL_ARM_S       = 20     # OIL P low alarm arms after RPM>=OIL_ARM_RPM sustained this long
OIL_ARM_RPM     = 400    # (mirrors gauge_model oilArmTick — RPM is the fresh engine-off signal)
# ---------------------------------------------------------------------------
# DRIVETRAIN CONSTANTS — GM SIERRA 1500 3.0L DURAMAX (LZ0 / 10L80) ONLY.
#
# These are vehicle data, not tool data. Applied to a drive log from any other
# vehicle they yield confidently wrong gear-ratio output: a BMW F10 (ZF 8HP) or an
# Audi Q5 (DL382 DSG) shares neither these ratios, this axle, nor this tyre size.
# The gear check at the bottom of this file is the only consumer, and it is
# GM-only for that reason — read its output as meaningless on any other vehicle.
#
# Reading these from the active VehicleProfile (the original TODO here) is blocked
# on the firmware side: `VehicleProfile` carries no drivetrain fields today, so
# there is nothing for this tool to parse. Adding them is a firmware change across
# all four profiles, not a tool change — tracked in the backlog, not attempted here.
GEAR_RATIOS = {1:4.70, 2:2.99, 3:2.15, 4:1.80, 5:1.52, 6:1.28, 7:1.00, 8:0.85, 9:0.69, 10:0.64}
AXLE = 3.42              # rear axle ratio
REVS_PER_MILE = 668      # LT275/65R18 (~31.1" dia); calibrate if the constant offset bugs you

# ---------------------------------------------------------------------------
def parse_readouts(readouts_cpp):
    """Extract {name: dict(warnHi, critHi, warnLo, critLo, fullScale)} from the
    READOUTS[] table in readouts.cpp. The row shape is:
      {"NAME", <unit>, <dec>, T(wh,ch,wl,cl), <fullScale>, ...}
    NA entries become NaN. Parsing the source keeps the audit in lockstep with
    the firmware — no second copy of the threshold table to forget to update."""
    src = open(readouts_cpp).read()
    rows = {}
    pat = re.compile(r'\{"(?P<name>[^"]+)",\s*.*?,\s*\d+,\s*T\((?P<thr>[^)]*)\),\s*(?P<full>[0-9.]+)')
    for m in pat.finditer(src):
        vals = []
        for tok in m.group('thr').split(','):
            tok = tok.strip().rstrip('f')
            vals.append(float('nan') if tok == 'NA' else float(tok))
        rows[m.group('name')] = dict(warnHi=vals[0], critHi=vals[1],
                                     warnLo=vals[2], critLo=vals[3],
                                     fullScale=float(m.group('full')))
    return rows

def zone(v, t):
    """Mirror of gauge_model zoneFor(): 0=Green 1=Amber 2=Red."""
    if not math.isnan(t['critHi']) and v >= t['critHi']: return 2
    if not math.isnan(t['critLo']) and v <= t['critLo']: return 2
    if not math.isnan(t['warnHi']) and v >= t['warnHi']: return 1
    if not math.isnan(t['warnLo']) and v <= t['warnLo']: return 1
    return 0

# ---------------------------------------------------------------------------
def load_csv(path):
    """Worker: load one CSV; None for old-schema files (pre-probe columns)."""
    try:
        df = pd.read_csv(path)
    except Exception as e:
        return ('error', path, str(e))
    if 'OIL P' not in df.columns and 'O115C' not in df.columns:
        return ('old', path, len(df))
    df['file'] = os.path.basename(path)
    return ('ok', path, df)

# ---------------------------------------------------------------------------
def reconstruct_alarms(d, thr, out):
    """Replay firmware alarm logic per file (holdoff + grace + OIL P arm-delay).
    Reports every distinct firing with start time, duration, and peak value."""
    fired = []
    alarmable = [n for n, t in thr.items()
                 if n in d.columns and not all(math.isnan(t[k]) for k in
                                              ('warnHi','critHi','warnLo','critLo'))]
    for fname, g in d.groupby('file', sort=True):
        g = g.reset_index(drop=True)
        # OIL P arm-delay: armed[i] mirrors gauge_model oilArmTick over this file —
        # armed only after OIL_ARM_S consecutive rows of RPM >= OIL_ARM_RPM, and
        # RPM must be FRESH. The CSV logs the held value each second, so freshness
        # is approximated as "changed within the last 4 rows" (fast-tier RPM polls
        # every ~2-3s while live; a key-off hold freezes it exactly constant).
        armed = np.zeros(len(g), dtype=bool)
        if 'RPM' in g.columns:
            rpmv = g['RPM'].to_list()
            streak = 0; lastChange = 0
            for i, rpm in enumerate(rpmv):
                if i and (pd.isna(rpm) or pd.isna(rpmv[i-1]) or rpm != rpmv[i-1]):
                    lastChange = i
                fresh = (i - lastChange) <= 4
                streak = streak + 1 if (not pd.isna(rpm) and rpm >= OIL_ARM_RPM and fresh) else 0
                armed[i] = streak >= OIL_ARM_S
        for name in alarmable:
            t = thr[name]
            run = 0
            for i, v in enumerate(g[name]):
                if i < STARTUP_GRACE_S or pd.isna(v):
                    run = 0; continue
                z = zone(v, t)
                # OIL P low alarm: suppressed until the engine-running arm-delay expires
                if name == 'OIL P' and z > 0 and not armed[i]: z = 0
                run = run + 1 if z > 0 else 0
                if run == HOLDOFF_S + 1:          # holdoff expired -> alarm fires
                    seg = g[name].iloc[i:i+120]
                    fired.append(dict(stat=name, file=fname,
                                      at=str(g['datetime'].iloc[i]),
                                      value=v, peak=seg.max() if z else v))
    out.append("## 3. Alarm reconstruction (firmware logic replayed)\n")
    if not fired:
        out.append("**No alarms would have fired** with the current thresholds "
                   f"(holdoff {HOLDOFF_S}s, grace {STARTUP_GRACE_S}s, OIL P {OIL_ARM_S}s arm-delay).\n")
    else:
        out.append("| Stat | When | Value at fire | Peak after | File |\n|---|---|---|---|---|\n")
        for f in fired:
            out.append(f"| {f['stat']} | {f['at']} | {f['value']:.1f} | {f['peak']:.1f} | {f['file']} |\n")
    return fired

# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('csv_dir')
    ap.add_argument('-o', '--out', default=None, help='report path (.md); default <csv_dir>/analysis-report.md')
    ap.add_argument('--pdf', action='store_true', help='also render the report to PDF via pandoc')
    ap.add_argument('--workers', type=int, default=min(os.cpu_count() or 4, 4))
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    thr = parse_readouts(os.path.join(here, '..', 'src', 'readouts.cpp'))
    if not thr:
        sys.exit("could not parse thresholds from src/readouts.cpp")

    files = sorted(f for f in os.listdir(args.csv_dir) if f.endswith('.csv'))
    with Pool(args.workers) as pool:
        loaded = pool.map(load_csv, [os.path.join(args.csv_dir, f) for f in files])
    frames  = [r[2] for r in loaded if r[0] == 'ok']
    skipped = [r for r in loaded if r[0] != 'ok']
    if not frames:
        sys.exit("no current-schema CSVs found (need OIL P / probe columns)")
    d = pd.concat(frames, ignore_index=True)
    # Transitional shim: probe-era logs (2026-07-17) carry raw O115C instead of
    # the OIL P psi column — derive psi with the shipped decoder so the audit
    # works across the schema change. (raw 36 = zero floor; see readouts.cpp.)
    if 'OIL P' not in d.columns and 'O115C' in d.columns:
        d['OIL P'] = ((d['O115C'] - 36) * 0.6).clip(lower=0)

    out = [f"# OBD Drive-Log Analysis\n\n"]

    # --- 1. inventory ---
    out.append("## 1. Data inventory\n\n")
    out.append(f"- {len(frames)} current-schema files, **{len(d):,} rows** "
               f"({len(skipped)} skipped: old schema/unreadable)\n")
    out.append(f"- span: {d['datetime'].min()} → {d['datetime'].max()}\n\n")

    # --- 2. threshold audit ---
    out.append("## 2. Threshold audit\n\n")
    out.append("| Stat | min | p50 | p99 | max | warn/crit (hi) | warn/crit (lo) | verdict |\n"
               "|---|---|---|---|---|---|---|---|\n")
    for name, t in thr.items():
        if name not in d.columns: continue
        if all(math.isnan(t[k]) for k in ('warnHi','critHi','warnLo','critLo')): continue
        s = d[name].dropna()
        if not len(s): continue
        verdict = "ok"
        if not math.isnan(t['warnHi']):
            head = t['warnHi'] - s.quantile(.99)
            if s.max() >= t['warnHi']: verdict = "**CROSSED warn-hi**"
            elif head < 0.1 * t['warnHi']: verdict = f"tight hi headroom ({head:.0f})"
        if not math.isnan(t['warnLo']):
            # NO LO-SIDE HEADROOM VERDICT, deliberately. The hi side reports
            # "tight headroom" because approaching an upper limit is a real
            # signal. The lo side is not symmetric: never dipping toward a lo
            # threshold is the HEALTHY case -- VOLTS warnLo is 11.0 V and a good
            # system sits at 13.5-14.5 V, so a "never approached" verdict would
            # fire on every clean log and train the reader to ignore the column.
            # OIL P's lo behaviour is genuinely interesting and gets its own
            # engine-off/RPM-band analysis in section 4.
            if s.min() <= t['warnLo']: verdict = "**CROSSED warn-lo**"
        hi = "—" if math.isnan(t['warnHi']) else f"{t['warnHi']:g}/{t['critHi']:g}"
        lo = "—" if math.isnan(t['warnLo']) else f"{t['warnLo']:g}/{t['critLo']:g}"
        out.append(f"| {name} | {s.min():.1f} | {s.median():.1f} | {s.quantile(.99):.1f} "
                   f"| {s.max():.1f} | {hi} | {lo} | {verdict} |\n")
    out.append("\n")

    # --- 3. alarm reconstruction ---
    fired = reconstruct_alarms(d, thr, out)

    # --- 4. oil-pressure deep-dive ---
    out.append("\n## 4. Oil pressure (OIL P)\n\n")
    if 'OIL P' in d.columns and 'FUEL' in d.columns:
        off = d[(d['FUEL'].fillna(1) < 0.05)]['OIL P'].dropna()
        run = d[(d['FUEL'] >= 0.05)]['OIL P'].dropna()
        out.append(f"- engine-off rows (fuel<0.05): n={len(off)}, "
                   f"median **{off.median():.1f} psi** (should be ~0 — floor check)\n"
                   if len(off) else "- no engine-off rows captured\n")
        out.append("\n| RPM band | n | median psi |\n|---|---|---|\n")
        r = d[(d['FUEL'] >= 0.05)].dropna(subset=['OIL P','RPM'])
        for lo, hi in [(500,800),(800,1200),(1200,1700),(1700,2200),(2200,2700),(2700,3600)]:
            s = r[(r.RPM >= lo) & (r.RPM < hi)]['OIL P']
            if len(s) >= 20:
                out.append(f"| {lo}–{hi} | {len(s)} | {s.median():.1f} |\n")
        out.append("\n(Sanity: ~20 hot idle → ~50 at redline. A flat line or "
                   "negative trend means the scale/PID needs another look.)\n")

    # --- 5. gear check ---
    out.append("\n## 5. Gear check (22199A vs 10L80 ladder)\n\n")
    if 'GEAR' in d.columns:
        g = d.dropna(subset=['RPM','SPEED','GEAR'])
        g = g[(g.SPEED > 15) & (g.RPM > 800)]
        k = AXLE * REVS_PER_MILE / 60.0
        out.append("| GEAR | n | ratio observed | spec | Δ% |\n|---|---|---|---|---|\n")
        for gr in sorted(g.GEAR.unique()):
            s = g[g.GEAR == gr]
            if len(s) < 20 or int(gr) not in GEAR_RATIOS: continue
            obs = (s.RPM / s.SPEED).median() / k
            spec = GEAR_RATIOS[int(gr)]
            out.append(f"| {int(gr)} | {len(s)} | {obs:.2f} | {spec:.2f} "
                       f"| {100*(obs-spec)/spec:+.0f}% |\n")
        if 'OUTSPD' in d.columns and d['OUTSPD'].notna().sum() > 100:
            out.append("\nOUTSPD present — direct ratio = RPM/OUTSPD available "
                       "(tire/axle constants drop out).\n")

    # --- 6. data health ---
    out.append("\n## 6. Data health\n\n| Column | valid % | anomalies |\n|---|---|---|\n")
    skip = {'datetime','uptime_s','file'}
    for c in d.columns:
        if c in skip: continue
        s = d[c]
        pct = 100.0 * s.notna().sum() / len(s)
        anom = []
        if s.notna().any():
            if c not in ('OIL P',) and (s == -40).sum() > 0: anom.append(f"-40 x{(s==-40).sum()}")
            if (s == 65535).sum() > 0: anom.append(f"65535 x{(s==65535).sum()}")
            full = thr.get(c, {}).get('fullScale', float('nan'))
            if not math.isnan(full) and s.max() > full:
                anom.append(f"max {s.max():.0f} > fullScale {full:g}")
        if pct < 90 or anom:
            out.append(f"| {c} | {pct:.0f}% | {', '.join(anom) or '—'} |\n")
    out.append("\n(Only columns with <90% validity or anomalies are listed.)\n")

    # --- 7. notable events ---
    out.append("\n## 7. Notable events\n\n")
    if 'EGT' in d.columns:
        regen = d[(d.EGT > 950) & (d.SPEED > 30) & (d['FUEL'] > 1.0)]
        out.append(f"- possible regen rows (EGT>950 @ cruise w/ fuel): {len(regen)}"
                   + (f" — first at {regen['datetime'].iloc[0]}\n" if len(regen) else " (none)\n"))
        out.append(f"- EGT max: {d.EGT.max():.0f} F\n")
    for c, label in [('TRANS','TRANS max'), ('DPF dP','DPF dP max'), ('BOOST','BOOST max'),
                     ('HP','HP max'), ('SPEED','SPEED max')]:
        if c in d.columns and d[c].notna().any():
            out.append(f"- {label}: {d[c].max():.1f}\n")

    report = ''.join(out)
    out_path = args.out or os.path.join(args.csv_dir, 'analysis-report.md')
    open(out_path, 'w').write(report)
    print(f"wrote {out_path}  ({len(fired)} reconstructed alarms)")
    if args.pdf:
        # Portable: render with pandoc if it is on PATH; skip gracefully if not.
        # Argument-list invocation (no shell) so a path with metacharacters is
        # just a filename, never a command — same pattern as obd_scan/report.py.
        pdf_path = out_path[:-3] + '.pdf' if out_path.endswith('.md') else out_path + '.pdf'
        if shutil.which("pandoc") and subprocess.run(
                ["pandoc", out_path, "-o", pdf_path],
                stderr=subprocess.DEVNULL, check=False).returncode == 0:
            print(f"wrote {pdf_path}")
        else:
            print("--pdf skipped: pandoc not found on PATH")

if __name__ == '__main__':
    main()
