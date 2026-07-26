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

    lines += [
        "# Ranked candidates",
        "",
        "Each row is one PID that answered, decoded under the interpretation that",
        "best correlates with a known anchor. Correlation proves *association*, not",
        "identity — treat a high `r` as a strong lead to confirm, not a conclusion.",
        "`r` is a **maximum over the `pairs` column's count of candidate",
        "(interpretation, anchor) pairs** (~70–150 typical), which is therefore",
        "optimistically biased — see \"How to read this\" below.",
        "",
        "| PID | Interpretation | Anchor | r | n | pairs | min | max | Verdict |",
        "| :-- | :-- | :-- | --: | --: | --: | --: | --: | :-- |",
    ]
    for c in candidates:
        interp = c.best_interp.label if c.best_interp else "—"
        rng = "—" if c.best_interp is None else f"{c.vmin:g}"
        rng_max = "—" if c.best_interp is None else f"{c.vmax:g}"
        n_col = c.n if c.n else "—"
        pairs_col = c.pairs_searched if c.pairs_searched else "—"
        lines.append(f"| `{c.column}` | {interp} | {c.best_anchor or '—'} | "
                     f"{c.r:.3f} | {n_col} | {pairs_col} | {rng} | {rng_max} | {c.verdict} |")

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
    Path(path).write_text("\n".join(lines))
    if pdf and shutil.which("pandoc"):
        pdf_path = path[:-3] + ".pdf" if path.endswith(".md") else path + ".pdf"
        subprocess.run(["pandoc", path, "-o", pdf_path], check=False)
    return path
