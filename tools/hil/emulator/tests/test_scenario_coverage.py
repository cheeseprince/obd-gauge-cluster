"""A scenario must answer everything its profile actually polls.

The gm_sierra scenario silently drifted out of coverage: the profile gained
eight enhanced DIDs (BOOST, INTAKE, FUEL, FUEL%, TORQUE, RefTq, EGR, CAC) and
the fixture kept answering NO DATA for all of them. Nothing failed — the dash
just showed "--" on the bench, which is indistinguishable from a firmware bug,
and is exactly how a rig starts lying to you.

So the poll set is read FROM THE FIRMWARE SOURCE rather than mirrored into a
list here. A list would need updating by the same person who forgot the
scenario, which is no guard at all.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import scenarios as sc

# tests -> emulator -> hil -> tools -> repo root
SRC = pathlib.Path(__file__).resolve().parents[4] / "src" / "vehicles"
assert SRC.is_dir(), f"profile source not found at {SRC} — path depth changed"

# A READOUTS row: {"NAME", "unit", decimals, T(...), fullScale, cmd, header, tier, ...}
#
# `cmd` is a quoted request OR `nullptr` for a COMPUTED row (MPG, HP, DSL FILL —
# derived, never polled). Both forms must match: READOUTS[] is indexed by
# StatId, so skipping the computed rows would shift every row after them and
# map each stat to the wrong enum entry. That misalignment is what made this
# test first report AMBIENT and PEDAL as missing when neither is even polled.
ROW = re.compile(
    r'\{\s*"(?P<name>[^"]+)"\s*,'      # stat name
    r'[^,]*,'                          # unit
    r'[^,]*,'                          # decimals
    r'\s*T\([^)]*\)\s*,'               # thresholds
    r'[^,]*,'                          # fullScale
    r'\s*(?P<cmd>"[0-9A-Fa-f]+"|nullptr)\s*,'   # ELM request, or computed
    r'\s*(?P<hdr>\d+)\s*,'             # header index
)

# Header index -> AT SH value. Declared per profile in its AddressingDef table;
# both GM and Ford use 0=functional, and differ on 1 (GM 7E2 trans, Ford 7E1).
HEADERS = {
    "gm_sierra_lz0.cpp": {0: "7DF", 1: "7E2", 2: "7E0"},
    "ford_sd_67.cpp":    {0: "7DF", 1: "7E1", 2: "7E0"},
}


APP_TYPES = pathlib.Path(__file__).resolve().parents[4] / "src" / "app_types.h"


def stat_id_order():
    """StatId names in declaration order. READOUTS[] is indexed by StatId, so
    the Nth table row IS the Nth enum entry — that is what lets a row be mapped
    back to the StatId the layout refers to."""
    text = APP_TYPES.read_text(encoding="utf-8")
    body = re.search(r"enum class StatId\s*\{(.*?)\}\s*;", text, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)          # strip comments between entries
    names = [n.strip() for n in body.split(",") if n.strip()]
    assert names[0] == "Trans" and names[-1] == "COUNT", f"unexpected StatId shape: {names[:3]}"
    return names[:-1]                              # drop COUNT


def poll_set(profile_file):
    """Every (request, header) the profile ACTUALLY POLLS.

    Mirrors the firmware's isActive(): a stat is polled if it occupies a layout
    cell or is a declared helper. Rows that are merely present in the table are
    deliberately NOT required — gm_sierra_lz0 declares AMBIENT (220046) and
    PEDAL (22004A) but puts neither on a page nor in HELPERS, so the dash never
    asks for them and a fixture owes them nothing.
    """
    text = (SRC / profile_file).read_text(encoding="utf-8")
    hdrs = HEADERS[profile_file]
    rows = [(m.group("cmd").strip('"').upper() if m.group("cmd") != "nullptr" else None,
             hdrs[int(m.group("hdr"))], m.group("name"))
            for m in ROW.finditer(text)]

    ids = stat_id_order()
    # Guard the regex AND the alignment: READOUTS must cover every StatId, in
    # order. A table reformat or a dropped row has to fail loudly here rather
    # than silently shift the mapping and accuse the wrong stats.
    assert len(rows) == len(ids), (
        f"{profile_file}: parsed {len(rows)} rows but StatId has {len(ids)} entries "
        f"— the row regex is stale, so the row-to-StatId mapping is wrong")

    referenced = set(re.findall(r"StatId::(\w+)", text))   # PAGES cells + HELPERS
    active = [r for r, sid in zip(rows, ids) if sid in referenced and r[0] is not None]
    assert len(active) >= 15, f"only {len(active)} active polled rows in {profile_file}"
    return active


def unanswered(scenario, rows):
    missing = []
    for cmd, hdr, name in rows:
        if cmd.startswith("01"):
            ok = cmd[2:].upper() in {k.upper() for k in scenario.mode01}
        elif cmd.startswith("22"):
            ok = cmd[2:].upper() in {k.upper() for k in scenario.mode22.get(hdr, {})}
        else:
            continue                    # mode 09 (VIN) is served separately
        if not ok:
            missing.append(f"{cmd}@{hdr} ({name})")
    return missing


def test_gm_scenario_answers_every_pid_the_gm_profile_polls():
    rows = poll_set("gm_sierra_lz0.cpp")
    missing = unanswered(sc.GM_SIERRA, rows)
    assert not missing, (
        "gm_sierra answers NO DATA for stats the dash actually polls, so they "
        "render '--' on the bench and look like firmware bugs: " + ", ".join(missing))


def test_ford_scenario_answers_every_pid_the_ford_profile_polls():
    rows = poll_set("ford_sd_67.cpp")
    missing = unanswered(sc.FORD_SD_67, rows)
    assert not missing, "ford_sd_67 scenario has gaps: " + ", ".join(missing)


def test_gm_scenario_is_a_loaded_state_not_idle():
    """Zero is the one value a fixture must not serve for these.

    boost and EGR both read 0.00 at idle, which is exactly what a decoder
    returning nothing looks like. Serving a loaded row is what makes a broken
    decode visible on the bench.
    """
    assert sc.GM_SIERRA.mode22["7E0"]["000B"] != "00", "boost must be non-zero"
    assert sc.GM_SIERRA.mode22["7E0"]["002C"] != "00", "EGR must be non-zero"
    # rpm override must actually be in effect, not shadowed by the idle baseline
    assert sc.GM_SIERRA.mode01["0C"] == "2178", "rpm should be the loaded 2142, not idle"
