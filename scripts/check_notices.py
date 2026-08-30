#!/usr/bin/env python3
"""CI guard -- keep THIRD-PARTY-NOTICES.md in step with what actually ships.

WHY THIS EXISTS. THIRD-PARTY-NOTICES.md is not documentation. It is published as
a release asset and to gh-pages beside crowpanel_obd.bin, and it is how this
project meets the licences that require their notices to be reproduced in
BINARY redistributions specifically -- LGPL-2.1 for the Arduino core above all.
Until 2026-08-29 it was hand-maintained and nothing checked it. tools/gen_sbom.py
warned about a package with no licence entry and told the reader to "update
THIRD-PARTY-NOTICES.md", then never verified that they had -- and that warning is
non-fatal (gen_sbom.py, end of main()), so a release stayed cuttable either way.
A dependency could therefore be added to platformio.ini and to the licence table
to quiet the SBOM, while the document that actually ships silently stayed behind.
A missing name is the failure mode here, and an absence is what nobody spots.

SINGLE SOURCE OF TRUTH. The component table lives in tools/gen_sbom.py (SHIPPED)
and is imported, not restated. Two copies of the list would be the same drift
this guard exists to catch, one file later.

WHAT IS CHECKED
  * every SHIPPED component has a row in the notices table
  * every notices row is a SHIPPED component (catches a stale row left behind
    when a dependency is dropped)
  * every dependency pinned in platformio.ini is claimed by a SHIPPED row
    (catches the main case: a NEW dependency that never reaches the notices)
  * the version in the notices table equals the pin in platformio.ini
  * the SPDX id in the notices table matches the id the SBOM will report

WHAT IS NOT CHECKED, AND WHY -- stated rather than implied, because a check that
looks like it covers licensing while quietly skipping a row converts an
unchecked obligation into an apparently-checked one:
  * Arduino core and ESP-IDF versions. platformio.ini pins the PLATFORM
    (platform-espressif32 53.03.13); those two versions live INSIDE that package
    and are knowable only from `pio pkg list` at build time, which needs a
    provisioned PlatformIO. This guard is pure stdlib and runs anywhere, so it
    reports them as UNVERIFIABLE on every run instead of passing over them. The
    Arduino core is the LGPL-2.1 row -- the one where being wrong matters most.
  * whether a licence is CORRECT. Nothing here reads upstream licence files;
    it checks that two files in this repo agree, not that either is right.
  * the full licence texts below the table. Only the table is parsed.
  * envs other than crowpanel_obd. That is the only env the release workflow
    builds, so it is the only binary the notices file describes. A dependency
    added to a non-shipped env is correctly ignored.

Run from anywhere in the repo:  python3 scripts/check_notices.py
Exit 0 = in step, 1 = drift (prints every disagreement, not just the first).
"""
import importlib.util
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
NOTICES = REPO / "THIRD-PARTY-NOTICES.md"
PIO_INI = REPO / "platformio.ini"
GEN_SBOM = REPO / "tools" / "gen_sbom.py"

# The env the release workflow builds, and therefore the binary the notices
# file describes. See .github/workflows/release.yml.
SHIPPED_ENV = "crowpanel_obd"

# A git dependency is pinned by full SHA in platformio.ini; the notices table
# carries a human-length prefix. Anything shorter than this is not a pin.
MIN_SHA_PREFIX = 7


def load_shipped(path=GEN_SBOM):
    """Import SHIPPED from tools/gen_sbom.py without running it.

    By path rather than as a package: tools/ has no __init__.py, and gen_sbom.py
    is a script invoked by the release workflow, not a module anyone installs.
    """
    spec = importlib.util.spec_from_file_location("gen_sbom", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.SHIPPED


def parse_platformio(text, env=SHIPPED_ENV):
    """Return {name: version} for every dependency pinned for `env`.

    Hand-rolled rather than configparser: platformio.ini uses ${section.key}
    interpolation, which configparser's default interpolation rejects outright,
    and the raw parser then hands back values whose inline `;` comments still
    have to be stripped. The two directives that matter are simple enough that
    reading them directly is less machinery than configuring a parser to be
    inert.
    """
    pins = {}
    in_env = False
    key = None
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].rstrip()      # strip inline comments
        if not line.strip():
            continue
        if line.startswith("["):
            in_env = line.strip() == f"[env:{env}]"
            key = None
            continue
        if not in_env:
            continue
        if not line[0].isspace() and "=" in line:
            key, _, rest = line.partition("=")
            key = key.strip()
            values = [rest.strip()] if rest.strip() else []
        elif key:
            values = [line.strip()]
        else:
            continue

        for value in values:
            if key == "platform":
                # .../releases/download/53.03.13/platform-espressif32.zip
                m = re.search(r"/releases/download/([^/]+)/([^/]+?)(?:\.zip)?$", value)
                if m:
                    pins[m.group(2)] = m.group(1)
            elif key == "lib_deps":
                if value.startswith(("http://", "https://", "git+", "git@")):
                    # https://github.com/owner/Repo.git#<40-hex sha>
                    m = re.match(r".*/([^/#]+?)(?:\.git)?#([0-9a-fA-F]{7,40})$", value)
                    if m:
                        pins[m.group(1)] = m.group(2)
                else:
                    # owner/Name@1.2.3  (or a bare Name@1.2.3)
                    m = re.match(r"(?:[^/]+/)?([^@/]+)@([^\s]+)$", value)
                    if m:
                        pins[m.group(1)] = m.group(2)
    return pins


def parse_notices(text):
    """Return {component: (version, licence)} from the FIRST markdown table.

    Only the first table is read: everything after it is reproduced licence
    text, which contains pipe characters of its own.
    """
    rows = {}
    started = False
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            if started:
                break            # table ended; do not read the licence texts
            continue
        cells = [c.strip() for c in stripped.strip("|").split("|")]
        if len(cells) < 3:
            continue
        if set("".join(cells)) <= set(": -"):
            continue             # the |:---|:---| separator row
        if cells[0].lower() == "component":
            started = True       # header row
            continue
        if not started:
            continue
        rows[_plain(cells[0])] = (_plain(cells[1]), _plain(cells[2]))
    return rows


def _plain(cell):
    """Strip the markdown emphasis and code ticks the table uses for display."""
    return cell.replace("**", "").replace("`", "").strip()


def _version_matches(notices_version, pinned):
    """True when the notices table's version is the platformio.ini pin.

    A git dependency is pinned by full SHA and shown by prefix, so a prefix of
    at least MIN_SHA_PREFIX counts. Everything else must be exact -- a release
    version is not a prefix relationship, and treating it as one would let
    1.2.2 pass against a 1.2.26 pin.
    """
    if notices_version == pinned:
        return True
    if re.fullmatch(r"[0-9a-fA-F]{40}", pinned):
        return (len(notices_version) >= MIN_SHA_PREFIX
                and pinned.lower().startswith(notices_version.lower()))
    return False


def check(shipped, pins, notices):
    """Return (errors, unverifiable). Every disagreement, not just the first."""
    errors, unverifiable = [], []
    claimed = set()

    for c in shipped:
        if c.notices not in notices:
            errors.append(
                f"{c.notices}: linked into the firmware but has NO row in "
                f"THIRD-PARTY-NOTICES.md. That file ships beside the binary to "
                f"satisfy its licence ({c.licence}); add a row.")
            continue
        version, licence = notices[c.notices]

        if c.ini:
            claimed.add(c.ini)
            if c.ini not in pins:
                errors.append(
                    f"{c.notices}: no longer pinned in platformio.ini under "
                    f"'{c.ini}' for env {SHIPPED_ENV}. Either it was removed "
                    f"(drop the notices row) or renamed (update SHIPPED in "
                    f"tools/gen_sbom.py).")
            elif not _version_matches(version, pins[c.ini]):
                errors.append(
                    f"{c.notices}: notices say version {version!r}, "
                    f"platformio.ini pins {pins[c.ini]!r}. The notices file "
                    f"names the version that was linked, so it must follow "
                    f"the pin.")
        else:
            if not version:
                errors.append(f"{c.notices}: notices row has no version.")
            unverifiable.append(f"{c.notices} {version} ({c.licence})")

        expected = c.notices_licence or c.licence
        if expected not in licence:
            errors.append(
                f"{c.notices}: notices say licence {licence!r}, which does not "
                f"contain {expected!r} -- the id the SBOM reports for it.")

    for name in sorted(set(notices) - {c.notices for c in shipped}):
        errors.append(
            f"{name}: has a row in THIRD-PARTY-NOTICES.md but is not a SHIPPED "
            f"component in tools/gen_sbom.py. A notice for something not in the "
            f"binary misstates what is being redistributed; drop the row, or "
            f"add it to SHIPPED if it really does ship.")

    for name in sorted(set(pins) - claimed):
        errors.append(
            f"{name}: pinned in platformio.ini for env {SHIPPED_ENV} but no "
            f"SHIPPED row in tools/gen_sbom.py claims it, so nothing requires "
            f"it to appear in THIRD-PARTY-NOTICES.md. Read its upstream licence, "
            f"add a Shipped(...) row, and add the notice.")

    return errors, unverifiable


def main():
    shipped = load_shipped()
    pins = parse_platformio(PIO_INI.read_text(encoding="utf-8"))
    notices = parse_notices(NOTICES.read_text(encoding="utf-8"))

    if not pins:
        sys.exit(f"no dependencies parsed from platformio.ini for env "
                 f"{SHIPPED_ENV} -- the guard would pass vacuously. "
                 f"Was the env renamed?")
    if not notices:
        sys.exit("no component table parsed from THIRD-PARTY-NOTICES.md -- the "
                 "guard would pass vacuously. Was the table reformatted?")

    errors, unverifiable = check(shipped, pins, notices)

    for u in unverifiable:
        print(f"  UNVERIFIABLE (version lives inside platform-espressif32): {u}")
    sys.stdout.flush()   # keep the unverifiable list above the errors below it

    if errors:
        print(f"\nTHIRD-PARTY-NOTICES.md is out of step "
              f"({len(errors)} problem(s)):\n", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        print("\nThis file ships to satisfy binary-redistribution licence "
              "terms. Fix it before releasing.", file=sys.stderr)
        return 1

    print(f"THIRD-PARTY-NOTICES.md is in step: {len(shipped)} shipped "
          f"component(s), {len(pins)} pinned in platformio.ini, "
          f"{len(unverifiable)} version(s) not checkable offline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
