"""Tests for the CycloneDX `scope` that tools/gen_sbom.py assigns.

WHY THIS FILE EXISTS. Up to and including v0.4.10 the SBOM derived scope from
`pio pkg list --only-tools`, which answers "is this a build tool?" -- a different
question from "is this inside the binary?". The published v0.4.10 document was
wrong on 9 of its 19 components, in BOTH directions:

  framework-arduinoespressif32 (LGPL-2.1) scope=excluded, though statically linked
  framework-arduinoespressif32-libs       scope=excluded, though linked
  the 7 Modulino transitive deps          scope=required, though --gc-sections
                                          discarded them (nm: zero symbols)

CycloneDX 1.5 defines excluded as "not reachable within a call graph at runtime"
and required as "required for runtime" (bom-1.5.xsd), so those were not stylistic
choices -- the document asserted the copyleft component was not in the binary
while THIRD-PARTY-NOTICES.md, published in the same release, explained that it is.

The component names below are the ones in the REAL v0.4.10 SBOM, so this is a
regression test against a shipped artefact rather than a restatement of the table.
"""
import importlib.util
import pathlib
import sys

import pytest

_REPO = pathlib.Path(__file__).resolve().parents[3]
_GEN = _REPO / "tools" / "gen_sbom.py"
assert _GEN.is_file(), f"generator not found at {_GEN}"
_spec = importlib.util.spec_from_file_location("gen_sbom", _GEN)
sbom = importlib.util.module_from_spec(_spec)
sys.modules["gen_sbom"] = sbom
_spec.loader.exec_module(sbom)

# Reported as tools by PlatformIO -- which is why they were misclassified -- but
# statically linked into crowpanel_obd.bin. The Arduino core is the LGPL-2.1 one.
LINKED_FRAMEWORKS = [
    "framework-arduinoespressif32",
    "framework-arduinoespressif32-libs",
]

# Resolved as Modulino dependencies and compiled, then discarded at link.
# gen_sbom.py records the evidence: nm on firmware.elf, zero surviving symbols
# for each, 2026-08-04.
GC_SECTIONED = [
    "ArduinoGraphics",
    "Arduino_HS300x",
    "Arduino_LPS22HB",
    "Arduino_LSM6DSOX",
    "Arduino_LTR381RGB",
    "STM32duino VL53L4CD",
    "STM32duino VL53L4ED",
]

# Present in v0.4.10 and genuinely not in the image.
BUILD_TOOLS = [
    "tool-esptoolpy",
    "tool-mklittlefs",
    "tool-riscv32-esp-elf-gdb",
    "tool-xtensa-esp-elf-gdb",
    "toolchain-riscv32-esp",
    "toolchain-xtensa-esp-elf",
]

# Linked and correctly scoped even before the fix.
ALREADY_CORRECT = ["Arduino_Modulino", "LovyanGFX", "NimBLE-Arduino", "lvgl"]


def scope_of(name):
    """The scope the generator would emit, or None when it emits none."""
    return sbom.component(name, "1.0.0", "", "runtime", True).get("scope")


# --- the regression: 9 components that were wrong in v0.4.10 -----------------

@pytest.mark.parametrize("name", LINKED_FRAMEWORKS)
def test_linked_frameworks_are_required(name):
    """v0.4.10 said excluded -- i.e. not reachable at runtime -- and was wrong."""
    assert sbom.linkage(name) is True
    assert scope_of(name) == "required"


@pytest.mark.parametrize("name", GC_SECTIONED)
def test_gc_sectioned_libraries_are_excluded(name):
    """v0.4.10 said required. nm says zero surviving symbols."""
    assert sbom.linkage(name) is False
    assert scope_of(name) == "excluded"


@pytest.mark.parametrize("name", ALREADY_CORRECT)
def test_linked_libraries_stay_required(name):
    assert scope_of(name) == "required"


@pytest.mark.parametrize("name", BUILD_TOOLS)
def test_build_tools_stay_excluded(name):
    assert scope_of(name) == "excluded"


def test_exactly_nine_of_the_published_nineteen_change():
    """Guards the size of the claim, not just its direction.

    If a later edit widens or narrows which components flip, this fails and the
    release note describing the change has to be rewritten with it.
    """
    published_required = set(GC_SECTIONED) | set(ALREADY_CORRECT)
    published_excluded = set(LINKED_FRAMEWORKS) | set(BUILD_TOOLS)
    changed = [n for n in published_required if scope_of(n) != "required"]
    changed += [n for n in published_excluded if scope_of(n) != "excluded"]
    assert sorted(changed) == sorted(GC_SECTIONED + LINKED_FRAMEWORKS)
    assert len(changed) == 9


# --- the unknown-package case, which is the one that can still bite ----------

def test_unclassified_package_gets_no_scope_at_all():
    """Silence over-discloses; `excluded` would assert unverified absence.

    CycloneDX says a consumer SHOULD assume `required` when scope is absent. For
    a licence document, over-disclosing an unknown component is the safe
    direction -- claiming it is "not reachable within a call graph" is not.
    """
    c = sbom.component("some-new-library", "1.0.0", "", "runtime", True)
    assert sbom.linkage("some-new-library") is None
    assert "scope" not in c
    assert not any(p["name"] == "firmware:linked" for p in c["properties"])


def test_linked_flag_is_emitted_when_known():
    linked = sbom.component("lvgl", "8.4.0", "", "runtime", True)
    assert {"name": "firmware:linked", "value": "true"} in linked["properties"]
    discarded = sbom.component("ArduinoGraphics", "1.1.5", "", "runtime", False)
    assert {"name": "firmware:linked", "value": "false"} in discarded["properties"]


# --- invariants of the table itself ------------------------------------------

def test_the_three_groups_are_disjoint():
    """Overlap would make linkage() depend on the order of its own branches."""
    assert not (sbom.IN_IMAGE & set(sbom.NOT_LINKED))
    assert not (sbom.IN_IMAGE & set(sbom.BUILD_ONLY))
    assert not (set(sbom.NOT_LINKED) & set(sbom.BUILD_ONLY))


def test_platformio_scope_is_kept_but_no_longer_decides():
    """The old signal stays recorded as a fact, and stops steering the answer.

    Same package, both PlatformIO classifications: the emitted scope must not
    move. That is precisely what was broken before.
    """
    as_tool = sbom.component("framework-arduinoespressif32", "3.1.3", "", "build", True)
    as_lib = sbom.component("framework-arduinoespressif32", "3.1.3", "", "runtime", True)
    assert as_tool["scope"] == as_lib["scope"] == "required"
    assert {"name": "platformio:scope", "value": "build"} in as_tool["properties"]


# --- why a component is excluded, not merely that it is ----------------------

@pytest.mark.parametrize("name", GC_SECTIONED)
def test_gc_sectioned_records_the_nm_evidence(name):
    """Three of these are LGPL-2.1-only, so `excluded` is a copyleft assertion.

    CycloneDX cannot distinguish "it is a compiler" from "the linker discarded
    it" -- both are bare `excluded`. For Arduino_HS300x, Arduino_LPS22HB and
    Arduino_LSM6DSOX that difference is the difference between a documented
    absence and an unmet obligation, so the evidence has to reach the published
    document rather than stopping at a source comment.
    """
    props = sbom.component(name, "1.0.0", "", "runtime", False)["properties"]
    reason = next(p["value"] for p in props
                  if p["name"] == "firmware:exclusion-reason")
    assert "gc-sections" in reason and "nm" in reason


@pytest.mark.parametrize("name", BUILD_TOOLS)
def test_build_tools_say_they_are_build_tools(name):
    props = sbom.component(name, "1.0.0", "", "build", True)["properties"]
    reason = next(p["value"] for p in props
                  if p["name"] == "firmware:exclusion-reason")
    assert "build tool" in reason


def test_the_three_lgpl_only_entries_are_still_in_not_linked():
    """If one of these ever becomes linked, `excluded` silently becomes false.

    Named explicitly so that moving one between groups cannot be a quiet edit:
    it has to break this test and be argued for.
    """
    for name in ("Arduino_HS300x", "Arduino_LPS22HB", "Arduino_LSM6DSOX"):
        assert sbom.NOT_LINKED[name] == "LGPL-2.1-only"
        assert sbom.linkage(name) is False


def test_required_and_unknown_components_carry_no_exclusion_reason():
    for name in ("lvgl", "some-new-library"):
        props = sbom.component(name, "1.0.0", "", "runtime", True)["properties"]
        assert not any(p["name"] == "firmware:exclusion-reason" for p in props)
