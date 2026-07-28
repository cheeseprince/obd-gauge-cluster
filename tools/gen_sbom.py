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


def component(name, version, spec, scope, direct):
    c = {
        "type": "library",
        "name": name,
        "version": version,
        "scope": "required" if scope == "runtime" else "excluded",
        "properties": [
            {"name": "platformio:scope", "value": scope},
            {"name": "platformio:relationship",
             "value": "direct" if direct else "transitive"},
        ],
    }
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

    comps, floating, seen = [], [], set()
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

    doc = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
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
                 "value": "Runtime components are compiled into the image. Build-scope "
                          "components shape it but are not shipped inside it. CI-only "
                          "Python packages are deliberately excluded: they never reach "
                          "the device."},
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


if __name__ == "__main__":
    main()
