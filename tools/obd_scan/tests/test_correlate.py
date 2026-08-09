import pathlib
import sys
import warnings

import numpy as np
import pandas as pd

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from obd_scan.correlate import Interp, analyze, decode_series, interpretations


def test_interpretations_cover_u8_u16_s16():
    got = interpretations(4)
    assert Interp(0, 1, False, "u8@0") in got
    assert Interp(1, 2, True, "s16@1") in got
    # 4 bytes -> 4 u8 offsets + 3 u16 offsets + 3 s16 offsets
    assert len(got) == 10


def test_decode_series_u16_big_endian():
    vals = decode_series(["06E10000", "07770000"], Interp(0, 2, False, "u16@0"))
    assert list(vals) == [0x06E1, 0x0777]


def test_decode_series_signed():
    # NOTE: "FFFF" itself is a whole-cell sentinel (see SENTINELS) and is
    # correctly decoded to NaN post-fix-3, so use "FFFE" (-2 signed) here to
    # exercise signed two's-complement decoding without hitting that guard.
    vals = decode_series(["FFFE"], Interp(0, 2, True, "s16@0"))
    assert vals[0] == -2


def test_analyze_finds_the_planted_answer():
    # Plant a candidate whose bytes 1-2 track RPM exactly; bytes 0 and 3 are noise.
    rng = np.random.default_rng(0)
    rpm = np.linspace(700, 3000, 200)
    hexes = []
    for i, r in enumerate(rpm):
        v = int(r) & 0xFFFF
        hexes.append(f"{rng.integers(0,256):02X}{v:04X}{rng.integers(0,256):02X}")
    df = pd.DataFrame({"220041@7E0": hexes, "rpm": rpm})

    cands = analyze(df, ["220041@7E0"], ["rpm"])
    c = cands[0]
    assert c.best_anchor == "rpm"
    assert c.best_interp.label == "u16@1"
    assert c.r > 0.99


def test_analyze_flags_constant_column():
    df = pd.DataFrame({"2200FF@7E0": ["1A"] * 50, "rpm": np.linspace(700, 3000, 50)})
    c = analyze(df, ["2200FF@7E0"], ["rpm"])[0]
    assert c.verdict == "constant"


def test_analyze_flags_sentinel_column():
    df = pd.DataFrame({"2200FE@7E0": ["FFFF"] * 50, "rpm": np.linspace(700, 3000, 50)})
    c = analyze(df, ["2200FE@7E0"], ["rpm"])[0]
    assert c.verdict == "sentinel"


def test_analyze_parallel_matches_serial():
    # Columns are independent, so worker count must not change the answer.
    #
    # ALSO (final review): the four columns used to all encode the IDENTICAL
    # rpm bytes, so the ordering assertion was satisfied by stable-sort tie
    # preservation regardless of which pool processed them -- a shuffled or
    # even wrongly-ordered result could pass this test as long as ties broke
    # the same way. Give each column a DIFFERENT noise amplitude so each has
    # a distinct correlation strength and the final sort order is genuinely
    # load-bearing.
    rng = np.random.default_rng(1)
    n = 120
    rpm = np.linspace(700, 3000, n)
    cols = {}
    noise_scales = [5, 40, 120, 400]              # strictly increasing noise -> strictly decreasing |r|
    for k, scale in enumerate(noise_scales):
        noisy = np.clip(rpm + rng.normal(0, scale, n), 0, 65535).astype(int)
        cols[f"22004{k}@7E0"] = [f"{v & 0xFFFF:04X}" for v in noisy]
    df = pd.DataFrame({**cols, "rpm": rpm})
    hit_cols = list(cols)
    serial = analyze(df, hit_cols, ["rpm"], workers=1)
    parallel = analyze(df, hit_cols, ["rpm"], workers=4)
    assert [(c.column, round(c.r, 6)) for c in serial] == \
           [(c.column, round(c.r, 6)) for c in parallel]
    # And the ordering itself is meaningful: strictly decreasing |r|.
    rs = [abs(c.r) for c in serial]
    assert rs == sorted(rs, reverse=True)
    assert len(set(round(r, 3) for r in rs)) == len(noise_scales)  # genuinely distinct


def test_analyze_rejects_tiny_sample_correlation():
    # Only 5 rows carry real data (perfectly tracking rpm by construction);
    # the other 35 rows are blank. A 5-sample correlation is coincidence,
    # not evidence, no matter how clean r looks -- must NOT be "correlated".
    #
    # IMPORTANT 6: this used to assert "no-signal", which report.py defines
    # as "answered, but no interpretation varied usefully" -- false here:
    # every interpretation DID vary (r would be ~1.0), there just was never
    # enough overlap to trust it. The honest label is "too-few-samples",
    # carrying the best n actually seen, so a human doesn't discard a
    # possibly-real, intermittently-answering PID on a false description.
    n_total = 40
    rpm = np.linspace(700, 3000, n_total)
    hexes = []
    valid_idx = {2, 10, 20, 30, 37}
    for i, r in enumerate(rpm):
        if i in valid_idx:
            hexes.append(f"{int(r) & 0xFFFF:04X}")
        else:
            hexes.append("")
    df = pd.DataFrame({"22FFAA@7E0": hexes, "rpm": rpm})
    c = analyze(df, ["22FFAA@7E0"], ["rpm"])[0]
    assert c.verdict == "too-few-samples"
    assert c.n == len(valid_idx)


def test_analyze_correlated_reports_sample_count():
    # Ample overlapping samples: verdict is "correlated" AND n reflects the
    # true overlapping-sample count so a human can weigh the evidence.
    rng = np.random.default_rng(0)
    rpm = np.linspace(700, 3000, 200)
    hexes = []
    for r in rpm:
        v = int(r) & 0xFFFF
        hexes.append(f"{rng.integers(0,256):02X}{v:04X}{rng.integers(0,256):02X}")
    df = pd.DataFrame({"220041@7E0": hexes, "rpm": rpm})
    c = analyze(df, ["220041@7E0"], ["rpm"])[0]
    assert c.verdict == "correlated"
    assert c.n == 200


def test_analyze_anchor_nan_pattern_does_not_spam_runtimewarning():
    # The anchor's NaN pattern strips all variance out of vals for the only
    # rows that survive the mask -- must not reach corrcoef with a
    # zero-variance array (RuntimeWarning + internal NaN).
    n_const, n_var = 35, 15
    const_hex = [f"{5:04X}"] * n_const              # constant payload
    var_hex = [f"{i:04X}" for i in range(1, n_var + 1)]  # varying payload
    hexes = const_hex + var_hex

    rpm_const = np.linspace(700, 3000, n_const)     # anchor varies here
    rpm_nan = [np.nan] * n_var                       # anchor absent here
    rpm = list(rpm_const) + rpm_nan

    df = pd.DataFrame({"22FFAB@7E0": hexes, "rpm": rpm})
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        cands = analyze(df, ["22FFAB@7E0"], ["rpm"])
    assert not any(issubclass(w.category, RuntimeWarning) for w in caught)
    assert cands  # no crash


def test_analyze_pure_noise_never_reports_correlated():
    # CRITICAL 2, demonstrated: analyze() takes the max over interpretations
    # x anchors (70 pairs for a 4-byte payload), so a high max-|r| on pure
    # noise is exactly what that search predicts -- n=600 looks reassuring
    # but cannot warn anyone, since it is the SEARCH WIDTH, not n, that
    # inflates |r|. 12 columns of pure random 4-byte payloads against a
    # realistic multi-anchor set must never clear the "correlated" bar.
    rng = np.random.default_rng(42)
    n = 600
    anchors = {
        "rpm":     np.linspace(700, 3000, n),
        "speed":   np.linspace(0, 70, n),
        "load":    np.linspace(10, 90, n),
        "coolant": np.linspace(60, 210, n),
    }
    cols = {}
    for k in range(12):
        cols[f"22FF{k:02X}@7E0"] = [
            f"{rng.integers(0, 256):02X}{rng.integers(0, 256):02X}"
            f"{rng.integers(0, 256):02X}{rng.integers(0, 256):02X}"
            for _ in range(n)
        ]
    df = pd.DataFrame({**cols, **anchors})
    cands = analyze(df, list(cols), list(anchors))
    verdicts = {c.verdict for c in cands}
    assert "correlated" not in verdicts


def test_analyze_mid_strength_correlation_is_weak():
    # CRITICAL 2: a correlation strong enough to be a real lead but nowhere
    # near identification-grade must land between the two floors -- a
    # "weak" verdict, reported as "a lead to confirm, not a finding", not
    # bucketed with genuine identifications.
    rng = np.random.default_rng(7)
    n = 200
    rpm = np.linspace(700, 3000, n)
    noisy = np.clip(rpm + rng.normal(0, 700, n), 0, 65535).astype(int)
    hexes = [f"{v & 0xFFFF:04X}" for v in noisy]
    df = pd.DataFrame({"22FFAD@7E0": hexes, "rpm": rpm})
    c = analyze(df, ["22FFAD@7E0"], ["rpm"])[0]
    assert 0.60 <= abs(c.r) < 0.90
    assert c.verdict == "weak"


def test_analyze_planted_answer_reports_pairs_searched():
    # CRITICAL 2, part 2: the search width must travel with the result so
    # the report can tell the reader r is a MAX over ~70-150 candidate
    # pairs, not a single confirmatory test. A 4-byte payload against 1
    # anchor searches interpretations(4) (10) x 1 anchor = 10 pairs.
    rng = np.random.default_rng(0)
    rpm = np.linspace(700, 3000, 200)
    hexes = []
    for r in rpm:
        v = int(r) & 0xFFFF
        hexes.append(f"{rng.integers(0,256):02X}{v:04X}{rng.integers(0,256):02X}")
    df = pd.DataFrame({"220041@7E0": hexes, "rpm": rpm})
    c = analyze(df, ["220041@7E0"], ["rpm"])[0]
    assert c.pairs_searched == 10


def test_anchor_coverage_flags_unusable_anchor():
    # IMPORTANT 7: an all-blank anchor column (e.g. a vehicle that doesn't
    # support ambient-air 0146) silently degrades every verdict that would
    # have used it and is never mentioned today. Surface per-anchor
    # valid/total counts so a human can see why every "correlated" answer
    # so far used rpm/speed and never ambient.
    from obd_scan.correlate import anchor_coverage
    n = 50
    df = pd.DataFrame({
        "rpm": np.linspace(700, 3000, n),
        "ambient": [""] * n,
    })
    cov = anchor_coverage(df, ["rpm", "ambient"])
    assert cov["rpm"] == (n, n)
    assert cov["ambient"] == (0, n)


def test_decode_series_skips_whole_cell_sentinel_rows():
    # A few whole-cell sentinel rows mixed into genuine signal must be
    # treated as NaN, not decoded into the correlation.
    rng = np.random.default_rng(2)
    rpm = np.linspace(700, 3000, 35)
    good_hexes = [f"{int(r) & 0xFFFF:04X}" for r in rpm]
    sentinel_hexes = ["FFFF"] * 5
    hexes = good_hexes + sentinel_hexes
    rpm_full = list(rpm) + [rng.uniform(700, 3000) for _ in range(5)]

    df = pd.DataFrame({"22FFAC@7E0": hexes, "rpm": rpm_full})
    c = analyze(df, ["22FFAC@7E0"], ["rpm"])[0]
    assert c.verdict == "correlated"
    assert c.n == 35  # the 5 sentinel rows must not count as overlapping samples


def test_decode_series_keeps_internal_0xff_byte():
    # Regression: an 0xFF byte WITHIN a longer, non-sentinel payload is
    # legitimate data and must still be decoded (no over-filtering).
    vals = decode_series(["FF1234"], Interp(0, 1, False, "u8@0"))
    assert vals[0] == 0xFF


def test_report_renders_candidates(tmp_path):
    from obd_scan.correlate import Candidate, Interp
    from obd_scan.report import write_report
    cands = [
        Candidate("220041@7E0", Interp(1, 2, False, "u16@1"), "rpm", 0.97, 300.0, 1000.0, "correlated", n=1240),
        Candidate("2200FF@7E0", None, "", 0.0, float("nan"), float("nan"), "constant"),
    ]
    out = tmp_path / "report.md"
    write_report(cands, str(out), pdf=False, meta={"vehicle": "test"})
    text = out.read_text()
    assert "220041@7E0" in text and "u16@1" in text and "0.97" in text
    assert "constant" in text
    # n travels with the result: "r=0.97 over 1240" reads very differently
    # from "r=0.99 over 31" -- the report must expose the sample count.
    assert "1240" in text
    header_line = next(ln for ln in text.splitlines() if ln.startswith("| PID"))
    assert "n" in [cell.strip() for cell in header_line.strip("|").split("|")]


def test_report_shows_anchor_coverage_and_pairs_searched(tmp_path):
    # IMPORTANT 7 + CRITICAL 2 part 2: an unusable anchor and the search
    # width must both be visible in the report, not just tracked internally.
    from obd_scan.correlate import Candidate, Interp
    from obd_scan.report import write_report
    cands = [
        Candidate("220041@7E0", Interp(1, 2, False, "u16@1"), "rpm", 0.97, 300.0, 1000.0,
                 "correlated", n=1240, pairs_searched=70),
    ]
    out = tmp_path / "report.md"
    write_report(cands, str(out), pdf=False, meta={"vehicle": "test"},
                anchor_coverage={"rpm": (1194, 1200), "ambient": (0, 1200)})
    text = out.read_text()
    assert "70" in text                       # pairs_searched surfaced
    assert "rpm 1194/1200" in text
    assert "ambient 0/1200 (UNUSABLE)" in text
    assert "weak" in text.lower()
    assert "too-few-samples" in text


# --- J1979 F4xx mirror vs enhanced ------------------------------------------
# 2021 F-350: 9 of the top 10 ranked rows were F4xx mirror columns correlating
# against anchors that ARE those same sensors, while the two parameters later
# confirmed (221E60 gear, 221E1C ATF temp) sat at ranks 10 and 28.

def test_mirrored_pid_recognises_the_f4xx_block():
    from obd_scan.correlate import mirrored_pid
    assert mirrored_pid("22F405@7E0") == "0105"     # coolant, re-served
    assert mirrored_pid("22f446@7DF") == "0146"     # case-insensitive
    # Enhanced PIDs are NOT mirrors -- this is the distinction the split relies on.
    assert mirrored_pid("221E1C@7E1") is None
    assert mirrored_pid("2204AA@7E0") is None
    assert mirrored_pid("0105") is None
    # 22F4 with a wrong-length request must not be mistaken for a mirror.
    assert mirrored_pid("22F4@7E0") is None


def test_self_comparison_is_called_a_tautology_not_a_correlation():
    # A mirror column scored against the anchor it mirrors is the same sensor
    # on both axes. It will score ~1.0 and must not read as a finding.
    coolant = np.linspace(30, 90, 120)
    df = pd.DataFrame({
        "coolant": coolant,
        # Deliberately NOT a ramp: a second monotonic anchor is collinear with
        # coolant and would tie against it for no physical reason.
        "rpm": np.random.default_rng(0).uniform(700, 2500, 120),
        # 22F405 IS PID 0105 -- byte 0 carries the same raw coolant value.
        "22F405@7E0": [f"{int(c) + 40:02X}" for c in coolant],
        # An enhanced PID tracking the same ramp is a genuine lead, not a tautology.
        "221E1C@7E1": [f"{int(c * 16):04X}" for c in coolant],
    })
    got = {c.column: c for c in analyze(df, ["22F405@7E0", "221E1C@7E1"],
                                        ["coolant", "rpm"], workers=1)}

    mirror = got["22F405@7E0"]
    assert mirror.mirror_of == "0105"
    assert mirror.best_anchor == "coolant"
    assert abs(mirror.r) > 0.99                      # it does correlate -- with itself
    assert mirror.verdict == "mirror-tautology"

    enhanced = got["221E1C@7E1"]
    assert enhanced.mirror_of is None
    assert enhanced.verdict == "correlated"          # a real lead, kept as one


def test_mirror_against_a_different_anchor_is_not_a_tautology():
    # 22F45C is engine oil temp. Correlating it against COOLANT is a real
    # (if unsurprising) association between two different sensors -- only a
    # self-comparison earns the tautology label.
    coolant = np.linspace(30, 90, 120)
    df = pd.DataFrame({
        "coolant": coolant,
        "22F45C@7E0": [f"{int(c * 0.9) + 40:02X}" for c in coolant],
    })
    c = analyze(df, ["22F45C@7E0"], ["coolant"], workers=1)[0]
    assert c.mirror_of == "015C"
    assert c.verdict == "correlated"


def test_report_puts_enhanced_pids_ahead_of_the_mirror_block(tmp_path):
    from obd_scan.correlate import Candidate, Interp
    from obd_scan.report import write_report
    cands = [
        # Ordered as analyze() would: the tautology outranks the real find.
        Candidate("22F405@7E0", Interp(0, 1, False, "u8@0"), "coolant", 0.999,
                  69, 121, "mirror-tautology", n=64, pairs_searched=7, mirror_of="0105"),
        Candidate("221E1C@7E1", Interp(0, 2, False, "u16@0"), "coolant", 0.851,
                  429, 1450, "weak", n=64, pairs_searched=28),
    ]
    out = tmp_path / "report.md"
    write_report(cands, str(out), pdf=False, meta={"vehicle": "ford"})
    text = out.read_text()

    # Both still reported -- nothing is hidden.
    assert "221E1C@7E1" in text and "22F405@7E0" in text
    # But the enhanced find now appears BEFORE the mirror block.
    assert text.index("221E1C@7E1") < text.index("22F405@7E0")
    assert text.index("# Ranked candidates — enhanced") < text.index("# Standard J1979 mirror")
    assert "Standard J1979 mirror (1 columns)" in text
    # The lag caveat that would have rescued this find is stated.
    assert "A lagging signal is not a weak one" in text
    assert "mirror-tautology" in text


def test_report_omits_the_mirror_section_when_there_is_none(tmp_path):
    from obd_scan.correlate import Candidate, Interp
    from obd_scan.report import write_report
    cands = [Candidate("221E60@7E1", Interp(0, 1, False, "u8@0"), "speed", 0.967,
                       1, 10, "correlated", n=64, pairs_searched=7)]
    out = tmp_path / "report.md"
    write_report(cands, str(out), pdf=False, meta={"vehicle": "ford"})
    text = out.read_text()
    assert "Standard J1979 mirror" not in text
    assert "221E60@7E1" in text
