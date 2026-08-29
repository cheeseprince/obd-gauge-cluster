"""Markdown (+ optional PDF) report emission."""
import shutil
import subprocess
from pathlib import Path


def write_report(candidates, path: str, pdf: bool = False, meta: dict | None = None,
                anchor_coverage: dict | None = None) -> str:
    meta = meta or {}
    lines = [
        "---",
        'title: "OBD Scan — Candidate Identity Report"',
        f'subtitle: "{meta.get("vehicle", "unknown vehicle")}"',
        "---",
        "",
    ]

    # IMPORTANT 7: an all-blank anchor (e.g. a vehicle without ambient-air
    # 0146) silently degrades every verdict that would have used it. Surface
    # per-anchor valid/total coverage before any per-column verdict is read.
    if anchor_coverage:
        parts = []
        for name, (valid, total) in anchor_coverage.items():
            tag = " (UNUSABLE)" if valid == 0 else ""
            parts.append(f"{name} {valid}/{total}{tag}")
        lines += ["## Anchor coverage", "", " · ".join(parts), ""]

    # Split the ranking. A sweep of the J1979 F4xx block rediscovers the
    # STANDARD PID set, and those columns correlate against the anchors
    # superbly *because several of them are the anchors*. Ranked together
    # they crowd out exactly what the scan went looking for: on the 2021
    # F-350, 9 of the top 10 rows were mirror columns while the two
    # parameters that were subsequently confirmed sat at ranks 10 and 28.
    enhanced = [c for c in candidates if not getattr(c, "mirror_of", None)]
    mirror = [c for c in candidates if getattr(c, "mirror_of", None)]

    def _table(rows):
        out = [
            "| PID | Interpretation | Anchor | r | n | pairs | min | max | Verdict |",
            "| :-- | :-- | :-- | --: | --: | --: | --: | --: | :-- |",
        ]
        for c in rows:
            interp = c.best_interp.label if c.best_interp else "—"
            rng = "—" if c.best_interp is None else f"{c.vmin:g}"
            rng_max = "—" if c.best_interp is None else f"{c.vmax:g}"
            n_col = c.n if c.n else "—"
            pairs_col = c.pairs_searched if c.pairs_searched else "—"
            out.append(f"| `{c.column}` | {interp} | {c.best_anchor or '—'} | "
                       f"{c.r:.3f} | {n_col} | {pairs_col} | {rng} | {rng_max} | {c.verdict} |")
        return out

    lines += [
        "# Ranked candidates — enhanced",
        "",
        "Proprietary/enhanced PIDs: everything that is **not** a J1979 `F4xx`",
        "re-serving of a standard Mode-01 PID. These are the discoveries a sweep",
        "of an unmapped vehicle exists to find — read this table first.",
        "",
        "Each row is one PID that answered, decoded under the interpretation that",
        "best correlates with a known anchor. Correlation proves *association*, not",
        "identity — treat a high `r` as a strong lead to confirm, not a conclusion.",
        "`r` is a **maximum over the `pairs` column's count of candidate",
        "(interpretation, anchor) pairs** (~70–150 typical), which is therefore",
        "optimistically biased — see \"How to read this\" below.",
        "",
    ] + _table(enhanced)

    if mirror:
        lines += [
            "",
            f"# Standard J1979 mirror ({len(mirror)} columns)",
            "",
            "SAE J1979 reserves DIDs `F400`–`F4FF` for the Mode-01 PIDs re-served",
            "over Mode 22, so `22F4xx` returns the same bytes as PID `xx`. These",
            "columns are real data and are worth having — a vehicle may serve a",
            "parameter here that its generic block ignores — but **none of them is",
            "an enhanced-diagnostics find**, and a community list presenting them as",
            "proprietary is republishing the standard block.",
            "",
            "Expect high `r` here. Some of these columns *are* the anchors.",
            "",
        ] + _table(mirror)

    lines += [
        "",
        "## How to read this",
        "",
        "- **constant** — never changed across the drive; almost certainly not a live sensor.",
        "- **sentinel** — only ever `FF`/`00` padding; an unsupported slot.",
        "- **too-few-samples** — every interpretation that varied never had enough",
        "  overlapping (both-valid) rows to trust — an intermittently-answering PID,",
        "  not a verdict on whether it means anything. `n` is the best overlap seen.",
        "- **no-signal** — ample samples, but no interpretation correlated above the",
        "  weak floor; answered, but nothing here tracks a known anchor.",
        "- **weak** — a real lead worth a second look, not a finding. Confirm it before",
        "  relying on it.",
        "- **correlated** — the interpretation shown tracks the named anchor strongly.",
        "  Even so, `r` is a maximum over dozens to ~150 candidate pairs (the `pairs`",
        "  column), which biases it upward — it is a strong lead, not proof.",
        "- **mirror-tautology** — a `22F4xx` column whose winning anchor is the very",
        "  PID it mirrors: the same sensor on both axes. `22F405` against the coolant",
        "  anchor `0105` will score near 1.000 and tells you nothing about the vehicle.",
        "",
        "## A lagging signal is not a weak one",
        "",
        "Ranking by `r` alone systematically penalises a *separate physical system*",
        "for behaving correctly. Transmission fluid and engine oil sit on different",
        "thermal masses and different cooling circuits, so they necessarily lag",
        "coolant during warm-up and score a lower `r` than a second coolant sensor",
        "would. On the 2021 F-350, the confirmed ATF temperature (`221E1C`, decoding",
        "cleanly to 26.8 → 90.6 °C) scored r=0.851 and was labelled **weak**, while",
        "coolant mirrors scored 0.999 — and coolant reached 78 °C while ATF was still",
        "at 56.6 °C, which is exactly the lag that depressed the correlation. An r",
        "near 1.000 against coolant would have been evidence the PID was *another",
        "coolant sensor*.",
        "",
        "So for a thermal or pressure candidate, check whether the decoded `min`/`max`",
        "lands in a plausible engineering range before believing the verdict column.",
        "That range, and the cold-start comparison, are the evidence — `r` is the",
        "search tool that found the row.",
        "",
        "A high `r` between two smooth, slowly-varying signals (a sensor drift, a",
        "warm-up curve) is close to guaranteed by autocorrelation and is NOT, by",
        "itself, evidence the two are the same physical quantity — two demonstrably",
        "unrelated smooth curves have been observed to correlate at r > 0.99 over",
        "600 samples. For temperature and pressure candidates in particular, the",
        "cold-start comparison, not `r`, is the evidence: a cold-vs-hot reading at",
        "the same RPM is what finally identified oil pressure on the GM truck, where",
        "correlation alone was ambiguous.",
        "",
    ]
    # encoding is EXPLICIT: Path.write_text() otherwise uses the locale default,
    # which is cp1252 on Windows, and this report contains "\u2192" and "\u00b0".
    # Found by the Windows CI job -- it crashed writing a report, which is the
    # last step of a real scan, after the drive.
    Path(path).write_text("\n".join(lines), encoding="utf-8")
    if pdf and shutil.which("pandoc"):
        pdf_path = path[:-3] + ".pdf" if path.endswith(".md") else path + ".pdf"
        subprocess.run(["pandoc", path, "-o", pdf_path], check=False)
    return path
