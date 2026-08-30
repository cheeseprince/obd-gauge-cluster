#!/usr/bin/env python3
"""Generate a CycloneDX SBOM describing what is actually inside the firmware image.

WHY NOT GITHUB'S AUTOMATIC SBOM. GitHub's dependency graph sees requirements/*.txt
and .github/workflows/, so its SBOM for this repo lists pytest, numpy, pandas and
a set of GitHub Actions. None of that is in the firmware. Publishing it beside a
.bin would be an authoritative-looking document asserting that a vehicle gauge
cluster contains pandas -- worse than shipping no SBOM at all, because it would be
believed.

This reads what PlatformIO actually resolved for the firmware environment: the
platform, the Arduino/ESP-IDF frameworks, and the C++ libraries that get compiled
and linked. That is the set a person asking "what is in this binary" means.

Build tools (compilers, cmake, ninja, esptool) are recorded separately from
runtime components. They shape the binary but are not shipped inside it, and
conflating the two is how an SBOM becomes noise.

WHAT `scope` MEANS HERE. It answers "is this inside the shipped binary?" and is
decided by the SHIPPED / NOT_LINKED / BUILD_ONLY table below, not by whether
PlatformIO happens to call a package a tool. Deriving it from `pio pkg list
--only-tools` -- which is what v0.4.10 and earlier did -- made the field
uncorrelated with the binary: it scoped the statically-linked, LGPL-2.1 Arduino
core `excluded`, and scoped seven --gc-sections-discarded libraries `required`.

Usage:
    python3 tools/gen_sbom.py --env crowpanel_obd --bin out/crowpanel_obd.bin \\
        --version v0.1.3 --output out/crowpanel_obd.bin.cdx.json
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import uuid
from typing import NamedTuple

# "├── Name @ 1.2.3 (required: spec)" / "│   ├── Name @ 1.2.3 (required: spec)"
ENTRY = re.compile(r"^[│\s]*[├└]──\s+(?P<name>.+?)\s+@\s+(?P<version>\S+)"
                   r"(?:\s+\(required:\s+(?P<spec>.*?)\))?\s*$")


def pio_list(env, flag):
    """Run `pio pkg list` and return parsed (name, version, spec, depth) rows."""
    out = subprocess.run(["pio", "pkg", "list", "-e", env, flag],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        m = ENTRY.match(line)
        if not m:
            continue
        # Depth from the tree gutter: nested entries are transitive dependencies.
        depth = (len(line) - len(line.lstrip("│ "))) // 4
        rows.append((m.group("name"), m.group("version"),
                     (m.group("spec") or "").strip(), depth))
    return rows


# WHAT IS IN THE FIRMWARE, AND WHERE EACH NOTICE HAS TO APPEAR.
#
# `pio pkg list` does not report licences, so this table is curated -- each entry
# was read from the upstream project's own licence file on 2026-08-04, not
# inferred from a registry summary.
#
# An SBOM that omits licences omits the main thing an SBOM is consulted for, so
# an UNKNOWN package is surfaced loudly (see main()) rather than silently
# emitting a component with no licence field.
#
# THE THREE GROUPS ARE MACHINE-READABLE ON PURPOSE. Only SHIPPED carries a
# redistribution obligation, because only those components are inside the .bin.
# scripts/check_notices.py imports SHIPPED from this file and fails CI when a
# component is missing from THIRD-PARTY-NOTICES.md, when its version disagrees
# with platformio.ini, or when platformio.ini gains a dependency that no row
# claims. Until 2026-08-29 the split lived only in the prose of these comments
# and NOTHING checked the notices file: a new dependency could be added here to
# quiet the SBOM warning while THIRD-PARTY-NOTICES.md -- the document that
# actually ships beside the binary to satisfy the licence -- silently stayed
# behind. Keep this the single source of truth. The guard must not restate it,
# or the two copies become the same class of drift one file later.


class Shipped(NamedTuple):
    """A component linked into the distributed firmware image."""

    notices: str      # exact text of the Component cell in THIRD-PARTY-NOTICES.md
    package: str      # `pio pkg list` package name; "" when it never appears there
    licence: str      # SPDX id recorded in the SBOM
    ini: str          # platformio.ini name whose pin fixes the version, or ""
                      # when the version is NOT derivable from that file
    notices_licence: str = ""   # only when the notices Licence cell cannot be the
                                # bare SPDX id -- see LovyanGFX


# Linked into the shipped image. `ini` is empty for the two framework packages:
# platformio.ini pins the PLATFORM (53.03.13) and the Arduino core and ESP-IDF
# versions live inside that package, so they are knowable only from `pio pkg
# list` at build time. The guard reports them as unverifiable rather than
# quietly skipping them -- the Arduino core is the LGPL-2.1 row, and a check
# that appears to cover it while not covering it is worse than no check.
SHIPPED = (
    Shipped("Arduino core for ESP32", "framework-arduinoespressif32",
            "LGPL-2.1-or-later", ""),
    Shipped("ESP-IDF", "framework-arduinoespressif32-libs",
            "Apache-2.0", ""),                          # prebuilt ESP-IDF
    Shipped("NimBLE-Arduino", "NimBLE-Arduino", "Apache-2.0", "NimBLE-Arduino"),
    Shipped("LVGL", "lvgl", "MIT", "lvgl"),
    Shipped("Modulino", "Arduino_Modulino", "MPL-2.0", "Modulino"),
    # LovyanGFX's licence file is a composite -- FreeBSD/BSD-2 for LovyanGFX
    # itself plus retained Adafruit (MIT/BSD) and TFT_eSPI (FreeBSD) notices.
    # BSD-2-Clause is the closest single SPDX id; the file is reproduced whole in
    # THIRD-PARTY-NOTICES.md because no identifier captures it, which is also why
    # the notices Licence cell cannot be the bare id.
    Shipped("LovyanGFX", "LovyanGFX", "BSD-2-Clause", "LovyanGFX",
            notices_licence="FreeBSD/BSD-2"),
    # The platform package itself is a declared, pinned dependency and is listed
    # in the notices file. `package` is empty because it does not appear in the
    # SBOM under a name this table would match -- see the note in main().
    Shipped("platform-espressif32", "", "Apache-2.0", "platform-espressif32"),
)

# Resolved as dependencies of Modulino and compiled, but NOT present in the
# firmware: --gc-sections discards them, verified 2026-08-04 with `nm` on
# firmware.elf (zero surviving symbols for each). Recorded for completeness, and
# deliberately absent from THIRD-PARTY-NOTICES.md -- reproducing a notice for
# code that is not in the binary would misstate what is being redistributed.
NOT_LINKED = {
    "ArduinoGraphics":                   "MPL-2.0",
    "Arduino_HS300x":                    "LGPL-2.1-only",
    "Arduino_LPS22HB":                   "LGPL-2.1-only",
    "Arduino_LSM6DSOX":                  "LGPL-2.1-only",
    "Arduino_LTR381RGB":                 "MPL-2.0",
    "STM32duino VL53L4CD":               "BSD-3-Clause",
    "STM32duino VL53L4ED":               "BSD-3-Clause",
}

# Build tools -- they produce the image, none of their code is inside it, so none
# of them belongs in THIRD-PARTY-NOTICES.md.
BUILD_ONLY = {
    "framework-espidf":                  "Apache-2.0",
    "tool-cmake":                        "BSD-3-Clause",
    "tool-ninja":                        "Apache-2.0",
    "tool-esptoolpy":                    "GPL-2.0-or-later",
    "tool-mklittlefs":                   "MIT",
    "tool-mkspiffs":                     "MIT",
    "tool-mkfatfs":                      "Apache-2.0",
    "toolchain-esp32ulp":                "GPL-2.0-or-later",   # binutils
    "tool-riscv32-esp-elf-gdb":          "GPL-3.0-or-later",
    "tool-xtensa-esp-elf-gdb":           "GPL-3.0-or-later",
    # GCC: GPL-3.0 WITH the Runtime Library Exception, which is precisely what
    # permits distributing a binary it compiled without that binary becoming
    # GPL. Spelling out the exception matters -- "GPL-3.0" alone would misstate
    # the position of every compiled artefact here.
    "toolchain-riscv32-esp":             "GPL-3.0-or-later WITH GCC-exception-3.1",
    "toolchain-xtensa-esp-elf":          "GPL-3.0-or-later WITH GCC-exception-3.1",
}

# WHY a component is excluded, recorded on the component itself. CycloneDX has
# one `excluded` value and no way to distinguish "never linked, it is a compiler"
# from "linked object discarded at link time" -- they read identically to a
# consumer. That distinction is load-bearing for three entries in NOT_LINKED:
# Arduino_HS300x, Arduino_LPS22HB and Arduino_LSM6DSOX are LGPL-2.1-only, so
# scoping them `excluded` is an assertion that LGPL code is NOT in the artefact.
# If that assertion is wrong it is an unmet copyleft obligation, so the evidence
# for it belongs in the published document rather than only in this comment.
# Uniform per group by construction: one reason each, so they cannot drift
# entry by entry.
GC_SECTIONED_REASON = ("resolved and compiled, then discarded at link by "
                       "--gc-sections; nm on firmware.elf showed zero surviving "
                       "symbols (verified 2026-08-04)")
BUILD_TOOL_REASON = ("build tool: it produces the image, none of its code is "
                     "linked into it")

# The packages that are actually INSIDE the image. This, not `pio pkg list
# --only-tools`, is what decides a component's CycloneDX scope -- see component().
IN_IMAGE = frozenset(c.package for c in SHIPPED if c.package)

# Flat lookup used when emitting a component. Derived, never hand-maintained:
# adding a row above is the only edit needed, and the guard reads the groups
# rather than this.
LICENCES = {
    **{c.package: c.licence for c in SHIPPED if c.package},
    **NOT_LINKED,
    **BUILD_ONLY,
}


def linkage(name):
    """True if the package is inside the image, False if not, None if unknown.

    NOT derived from `pio pkg list --only-tools`. That answers "is this a build
    tool or a library?", which is a different question from "is this in the
    binary?", and using it for scope got BOTH answers wrong in v0.4.10: the two
    framework packages are reported as tools by PlatformIO and were scoped
    `excluded` despite being statically linked, while the seven Modulino
    transitive dependencies were scoped `required` despite --gc-sections having
    discarded them. Nine of nineteen components, in both directions.
    """
    if name in IN_IMAGE:
        return True
    if name in NOT_LINKED or name in BUILD_ONLY:
        return False
    return None


def component(name, version, spec, scope, direct):
    c = {
        "type": "library",
        "name": name,
        "version": version,
        "properties": [
            # What PlatformIO resolved it as. Kept because it is a true fact
            # about the build, and deliberately no longer the basis for scope.
            {"name": "platformio:scope", "value": scope},
            {"name": "platformio:relationship",
             "value": "direct" if direct else "transitive"},
        ],
    }
    # CycloneDX 1.5 defines `excluded` as "not reachable within a call graph at
    # runtime" and `required` as "required for runtime" (bom-1.5.xsd). Those are
    # statements about the shipped binary, so they are answered from the table:
    #
    #   SHIPPED     linked into the image                     -> required
    #   NOT_LINKED  --gc-sections discarded it; `nm` shows
    #               zero surviving symbols, which is literally
    #               "not reachable within a call graph"       -> excluded
    #   BUILD_ONLY  produces the image, never inside it       -> excluded
    #
    # An UNCLASSIFIED package gets no scope at all. The spec says a consumer
    # SHOULD assume `required` when scope is absent, so silence over-discloses
    # rather than under-discloses -- the safe direction for a licence document.
    # Emitting `excluded` instead would assert non-reachability nobody verified.
    linked = linkage(name)
    if linked is not None:
        c["scope"] = "required" if linked else "excluded"
        c["properties"].append(
            {"name": "firmware:linked", "value": "true" if linked else "false"})
    if linked is False:
        c["properties"].append(
            {"name": "firmware:exclusion-reason",
             "value": GC_SECTIONED_REASON if name in NOT_LINKED
                      else BUILD_TOOL_REASON})
    licence = LICENCES.get(name)
    if licence:
        # CycloneDX: an SPDX id goes in `license.id`; an expression carrying a
        # WITH clause is not a bare id and must use `expression` instead, or the
        # document fails schema validation.
        if " WITH " in licence or " OR " in licence or " AND " in licence:
            c["licenses"] = [{"expression": licence}]
        else:
            c["licenses"] = [{"license": {"id": licence}}]
    if spec:
        c["properties"].append({"name": "platformio:required-spec", "value": spec})
        # A caret/tilde range means this version was chosen at BUILD time and can
        # differ on a later build of the same commit. Flag it in the document
        # rather than leaving the reader to infer it from the spec string.
        if any(spec.startswith(p) or f"@ {p}" in spec for p in ("^", "~")):
            c["properties"].append({"name": "platformio:floating", "value": "true"})
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--repo", default="https://github.com/cheeseprince/obd-gauge-cluster")
    a = ap.parse_args()

    blob = open(a.bin, "rb").read()
    sha = hashlib.sha256(blob).hexdigest()

    # Tools FIRST, and remember their names. `pio pkg list --only-platforms`
    # prints the whole platform tree, toolchains included, so without this the
    # compiler and cmake would be recorded as runtime components of the firmware
    # -- the exact build-vs-runtime conflation this file exists to avoid.
    tool_names = {name for name, _, _, _ in pio_list(a.env, "--only-tools")}

    comps, floating, seen, unlicensed, unclassified = [], [], set(), [], []
    for flag in ("--only-tools", "--only-libraries", "--only-platforms"):
        for name, version, spec, depth in pio_list(a.env, flag):
            # Same package appears under more than one listing; keep it once.
            if (name, version) in seen:
                continue
            seen.add((name, version))
            scope = "build" if name in tool_names else "runtime"
            c = component(name, version, spec, scope, depth == 0)
            comps.append(c)
            if any(p["name"] == "platformio:floating" for p in c["properties"]):
                floating.append(f"[{scope}] {name} {version} (spec {spec})")
            if "licenses" not in c:
                unlicensed.append(f"[{scope}] {name} {version}")
            if linkage(name) is None:
                unclassified.append(f"[{scope}] {name} {version}")

    # serialNumber is OPTIONAL in the CycloneDX spec but MANDATORY for GitHub's
    # attestation action, whose format sniffing is:
    #     checkIsCycloneDX = !!(bomFormat && serialNumber && specVersion)
    # Without it a spec-valid document is rejected as "Unsupported SBOM format",
    # which is what failed the v0.1.4 release after the build had already
    # succeeded and been signed.
    #
    # Derived, not random: a UUIDv5 over the repo, version and firmware digest
    # means re-running this on the same binary reproduces the same document
    # byte for byte. A uuid4 here would make every regeneration a diff and
    # destroy that property for no benefit — the value only has to be unique
    # per BOM, and (repo, version, image) already is.
    serial = uuid.uuid5(uuid.NAMESPACE_URL, f"{a.repo}@{a.version}#{sha}")

    doc = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "component": {
                "type": "firmware",
                "name": a.env,
                "version": a.version,
                "description": "ESP32-S3 OBD-II gauge cluster firmware",
                "hashes": [{"alg": "SHA-256", "content": sha}],
                "externalReferences": [{"type": "vcs", "url": a.repo}],
            },
            "properties": [
                {"name": "sbom:generator", "value": "tools/gen_sbom.py"},
                {"name": "sbom:source", "value": "pio pkg list"},
                {"name": "sbom:note",
                 "value": "scope=required means the component is linked into this "
                          "image; scope=excluded means it is not reachable in the "
                          "binary -- either a build tool, or a resolved library "
                          "that --gc-sections discarded (firmware:linked=false). "
                          "platformio:scope records what PlatformIO resolved the "
                          "package as and does NOT determine scope. CI-only Python "
                          "packages appear nowhere: they never reach the device. "
                          "Every excluded component carries a "
                          "firmware:exclusion-reason saying which kind of "
                          "exclusion it is, because the spec's single `excluded` "
                          "value cannot distinguish them."},
            ],
        },
        "components": comps,
    }
    with open(a.output, "w") as f:
        json.dump(doc, f, indent=2)

    print(f"SBOM: {len(comps)} components -> {a.output}")
    print(f"  firmware sha256 {sha}")
    if floating:
        # Not fatal. A release must still be cuttable. But an unpinned library is
        # how v0.1.0 shipped broken -- NimBLE 2.x changed setConnectTimeout from
        # seconds to milliseconds under a caret range -- so it is stated loudly
        # every time rather than buried in the document.
        print(f"  WARNING: {len(floating)} dependencies are NOT pinned; the same "
              f"commit can build a different binary later:", file=sys.stderr)
        for f_ in floating:
            print(f"    - {f_}", file=sys.stderr)

    if unclassified:
        # Scope is omitted for these, so a consumer assumes `required` -- the
        # over-disclosing direction, which is the right default but not an
        # answer. A new linked library landing here silently would be a
        # component whose licence obligation nobody has looked at.
        print(f"  WARNING: {len(unclassified)} component(s) are in NO group in "
              f"tools/gen_sbom.py (SHIPPED / NOT_LINKED / BUILD_ONLY), so their "
              f"CycloneDX scope is left unstated. Decide whether each is linked "
              f"into the image and add it to a group:", file=sys.stderr)
        for u in unclassified:
            print(f"    - {u}", file=sys.stderr)

    if unlicensed:
        # Also not fatal -- a release must stay cuttable -- but stated loudly.
        # A new dependency arriving with no licence entry is exactly when
        # someone needs to go read its licence file and decide whether it can
        # ship at all. Silence here would let an incompatible dependency (a
        # GPL or non-commercial library) into the firmware unremarked, and the
        # published SBOM would assert nothing either way.
        print(f"  WARNING: {len(unlicensed)} component(s) have NO licence in "
              f"tools/gen_sbom.py's LICENCES table. Read the upstream licence, "
              f"add it there, and update THIRD-PARTY-NOTICES.md:", file=sys.stderr)
        for u in unlicensed:
            print(f"    - {u}", file=sys.stderr)


if __name__ == "__main__":
    main()
