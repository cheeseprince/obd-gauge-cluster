"""The README must say what the code does.

WHY THIS EXISTS. CI covers code and cannot normally hold documentation, so a
README claim is only as good as somebody noticing. On 2026-08-29 two wrong
claims shipped in the same commit and were both caught by a human asking rather
than by any check:

  * `pip install bleak   # BLE only; WiFi needs nothing extra` -- wrong on every
    OS. __main__.py imports pandas at module scope, so the CLI cannot print
    --help without it.
  * `python3 -m obd_scan ...` under a heading promising Windows works -- a
    stock Windows install has `python` and the `py` launcher, not `python3`.

Both were the first command a new reader would type. Every assertion below is
tied to a claim that has actually drifted or would mislead, not to a
hypothetical -- a guard that fires on cosmetic edits gets deleted, and then it
protects nothing.

Borrowed from radiohound/obd-discover's ReadmeClaimsTest, which caught a bad
prose claim from an outside contributor. Credit where it is due.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from obd_scan import catalog as cat

ROOT = pathlib.Path(__file__).resolve().parents[3]
README = (ROOT / "README.md").read_text(encoding="utf-8")
SCAN_README = (ROOT / "tools" / "obd_scan" / "README.md").read_text(encoding="utf-8")


def _page_names(cpp: pathlib.Path) -> "list[str]":
    """PAGE_NAMES from a vehicle profile. Multi-line arrays are the norm, so the
    match is non-greedy to the closing brace rather than line-based -- a
    line-based read silently undercounts and the test then passes for the wrong
    reason, which is worse than failing."""
    m = re.search(r'PAGE_NAMES\[\]\s*=\s*\{(.*?)\}\s*;', cpp.read_text(encoding="utf-8"), re.S)
    return re.findall(r'"([^"]+)"', m.group(1)) if m else []


def test_vehicle_table_page_counts_match_the_profiles():
    """The vehicles table states a page count and names the pages. Both come
    from PAGE_NAMES in the profile, so both can drift -- and did: PR #94 took
    the BMW from 3 pages to 6 and had to update this table by hand."""
    profiles = {
        "audi_q5.cpp": ("Audi", "Q5"),
        "bmw_f10_535i.cpp": ("BMW", "535i"),
        "gm_sierra_lz0.cpp": ("GMC", "Sierra 1500 |"),
        "jeep_ws.cpp": ("Jeep", "Wagoneer"),
    }
    for fname, (make, model) in profiles.items():
        names = _page_names(ROOT / "src" / "vehicles" / fname)
        assert names, f"{fname}: could not parse PAGE_NAMES"
        row = next((ln for ln in README.splitlines()
                    if ln.startswith(f"| {make} |") and model.rstrip(" |") in ln), None)
        assert row, f"no README table row for {make} {model}"
        assert f"**{len(names)}**" in row, (
            f"{make} {model}: profile has {len(names)} pages; the README row does not "
            f"say **{len(names)}**\n  row: {row[:120]}")
        for page in names:
            assert page in row, f"{make} {model}: page {page!r} is missing from the README row"


def test_documented_pip_install_covers_what_the_cli_imports():
    """A `pip install` line that omits a module the CLI imports at startup makes
    the very next line fail. This is the 2026-08-29 bug: the quickstart said
    WiFi needed nothing beyond the standard library, and `import pandas` at
    module scope meant --help could not run."""
    main = (ROOT / "tools" / "obd_scan" / "__main__.py").read_text(encoding="utf-8")
    third_party = {m for m in re.findall(r'^import (\w+)|^from (\w+) import',
                                         main, re.M) for m in (m if isinstance(m, str) else "",)}
    third_party = {n for n in re.findall(r'^import (\w+)', main, re.M)
                   if n not in ("argparse", "json", "os", "sys", "re", "time", "pathlib")}
    assert third_party, "expected at least one third-party import to check against"
    for doc in (README, SCAN_README):
        installs = re.findall(r'pip install ([^\n#]*)', doc)
        assert installs, "no pip install line found"
        covered = " ".join(installs)
        for mod in sorted(third_party):
            assert mod in covered, (
                f"__main__.py imports {mod!r} at module scope, so the CLI cannot start "
                f"without it, but no documented `pip install` line mentions it")


def _code_block_lines(doc: str) -> "list[str]":
    """Lines inside ``` fences -- the ones a reader can copy and paste."""
    out, inside = [], False
    for line in doc.splitlines():
        if line.lstrip().startswith("```"):
            inside = not inside
            continue
        if inside:
            out.append(line)
    return out


def test_documented_subcommands_exist():
    """Every documented `python3 -m obd_scan ...` line must name a real
    subcommand. A documented command that does not exist fails on the reader's
    first line.

    THE ASSERTION IS "CONTAINS A REAL SUBCOMMAND", NOT "THE THIRD TOKEN IS ONE",
    and that weakening is deliberate. Two earlier drafts tried to identify the
    subcommand positionally and both produced FALSE POSITIVES on real lines:

      --host 192.168.0.10 census        read "192" as the subcommand
      --ble log --pids <the list ...>   --ble takes an OPTIONAL value, so
                                        skipping "its value" ate `log`

    A flag whose value is optional cannot be parsed without knowing the parser,
    and a guard that cries wolf on correct documentation gets deleted -- at
    which point it protects nothing. This form catches what actually matters: a
    command that does not exist at all, which is the failure a reader hits.
    """
    real = ("census", "sweep", "discover", "log", "correlate", "auto")
    checked = 0
    for doc, label in ((README, "README.md"), (SCAN_README, "obd_scan/README.md")):
        # ONLY fenced code blocks. Prose says things like "on Windows use
        # `py -m obd_scan ...`" with an ellipsis, which nobody runs verbatim.
        # What must be correct is what a reader can paste.
        for line in _code_block_lines(doc):
            if "-m obd_scan" not in line or "-m obd_scan.ble_bridge" in line:
                continue
            args = line.split("-m obd_scan", 1)[1].split("#", 1)[0]
            raw = args.replace("\\", " ").split()
            # `auto` is BOTH a subcommand and a valid --vehicle value, so a line
            # reading `--ble sweeep --vehicle auto` would otherwise satisfy this
            # check on the wrong token. Drop any token that is the value of
            # --vehicle before looking. Found by mutating a real line and
            # watching the test stay green.
            tokens = {t for i, t in enumerate(raw)
                      if not (i and raw[i - 1] == "--vehicle")}
            checked += 1
            assert tokens & set(real), (
                f"{label} documents a command with no recognised subcommand:\n  {line.strip()}")
    assert checked >= 5, f"expected several documented commands, saw {checked}"


def test_windows_claim_comes_with_the_windows_invocation():
    """The scanner section promises Windows works. `python3` does not exist on a
    stock Windows install -- python.org provides `python` and the `py` launcher
    -- so a Windows reader following the headline command gets
    "'python3' is not recognized" on the first line of a section that just told
    them it works for them."""
    for doc, label in ((README, "README.md"), (SCAN_README, "obd_scan/README.md")):
        if "Windows" not in doc:
            continue
        assert "py -m obd_scan" in doc, (
            f"{label} claims Windows support and shows `python3 -m obd_scan`, but never "
            f"gives the `py -m obd_scan` form that actually works there")


def test_preset_names_in_the_docs_are_real_presets():
    """The `--vehicle {...}` choices are generated from catalog.PRESETS, so a
    new preset appears in --help automatically and in prose only if someone
    remembers."""
    for doc, label in ((README, "README.md"), (SCAN_README, "obd_scan/README.md")):
        for listed in re.findall(r'--vehicle \{([a-z,]+)\}', doc):
            for name in listed.split(","):
                if name == "auto":
                    continue
                assert name in cat.PRESETS, f"{label} lists preset {name!r}, which does not exist"
