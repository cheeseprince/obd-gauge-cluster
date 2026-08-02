"""
Data-driven definitions — headers, PID blocks, anchors, vehicle presets — plus
the safety whitelist that keeps this tool read-only.

SAFETY: this runs on vehicles belonging to other people. Only read services may
ever be transmitted. Enforcement lives here and is called from elm.py before
anything reaches the socket, and from the CLI at startup so a malformed catalog
fails before the session begins rather than mid-sweep.
"""
import re
from dataclasses import dataclass, field
from typing import Iterator


class UnsafeRequest(Exception):
    """Raised when a request or AT command is not on the read-only whitelist."""


# Read services only. 01 = current data, 03 = stored DTCs, 09 = vehicle info,
# 22 = read data by identifier (enhanced).
ALLOWED_MODES = frozenset({0x01, 0x03, 0x09, 0x22})

# AT commands the tool is permitted to send. Prefix match, uppercased, spaces
# removed. Deliberately excludes ATPP (programmable parameter writes) and any
# command that persists adapter state beyond the session.
ALLOWED_AT = (
    "ATZ", "ATE0", "ATE1", "ATL0", "ATS0", "ATH0", "ATH1", "ATAT0", "ATAT1",
    "ATAT2", "ATST", "ATSP", "ATDP", "ATSH", "ATCP", "ATCRA", "ATCEA",
    "ATCAF0", "ATCAF1", "ATRV", "ATI", "ATDPN", "ATFCSH", "ATFCSD", "ATFCSM",
)


# Comfortably longer than any legitimate request or AT command this tool ever
# sends (the longest today is "ATSHDA10F1" at 10 chars, and sweep requests
# top out around 8 hex chars). Cheap belt-and-braces against pathological
# input, independent of the other checks.
MAX_LEN = 32

# After a matched AT prefix, only a bounded, benign suffix is tolerated: hex
# digits (adapter/CAN addressing arguments like "7E0" or "DA10F1"). Anything
# else — letters that could form a different command, punctuation, etc. — is
# rejected. Spaces are handled separately: they are stripped before this
# check runs, so they never appear in the suffix itself.
_AT_SUFFIX_RE = re.compile(r"^[0-9A-F]*$")


def _reject_control_chars(raw: str) -> None:
    """Raise UnsafeRequest if `raw` contains a control character.

    Checked on the RAW string, before any whitespace normalisation. An
    ELM327 treats CR as a command terminator, so a validator that only ever
    inspects a whitespace-stripped derived string can be fooled: the caller
    transmits the original string, and an embedded CR/LF turns one
    benign-looking command into two commands at the adapter. Space and tab
    are tolerated (legitimate hex strings use them as byte separators, e.g.
    "22 F4 46"); every other ASCII control character — CR, LF, NUL, etc. —
    and DEL are rejected outright.
    """
    for c in raw:
        if c in (" ", "\t"):
            continue
        if ord(c) < 0x20 or ord(c) == 0x7F:
            raise UnsafeRequest(f"control character in {raw!r}")


def validate_request(req: str) -> None:
    """Raise UnsafeRequest unless `req` is a well-formed read-service request."""
    if len(req) > MAX_LEN:
        raise UnsafeRequest(f"request too long ({len(req)} chars): {req!r}")
    _reject_control_chars(req)
    compact = "".join(req.split()).upper()
    if not compact or len(compact) % 2 or any(c not in "0123456789ABCDEF" for c in compact):
        raise UnsafeRequest(f"malformed request {req!r}")
    mode = int(compact[:2], 16)
    if mode not in ALLOWED_MODES:
        raise UnsafeRequest(
            f"mode 0x{mode:02X} is not a read service — refusing to transmit {req!r}")
    # Mode 0x22 (read data by identifier) always carries a 2-byte DID; a request
    # with fewer bytes is truncated/malformed, not merely a short valid read.
    if mode == 0x22 and len(compact) < 6:
        raise UnsafeRequest(f"malformed request {req!r}")


def validate_at(cmd: str) -> None:
    """Raise UnsafeRequest unless `cmd` is an allowlisted AT command.

    A matched allowlist prefix permits only a bounded suffix of hex digits —
    an allowed prefix must not be able to smuggle an arbitrary, unbounded
    tail (e.g. a persistent ATPP write appended right after ATSH).
    """
    if len(cmd) > MAX_LEN:
        raise UnsafeRequest(f"AT command too long ({len(cmd)} chars): {cmd!r}")
    _reject_control_chars(cmd)
    compact = "".join(cmd.split()).upper()
    for a in ALLOWED_AT:
        if compact.startswith(a) and _AT_SUFFIX_RE.match(compact[len(a):]):
            return
    raise UnsafeRequest(f"AT command not on the allowlist: {cmd!r}")


@dataclass(frozen=True)
class Header:
    """One CAN request header, with the AT commands needed to select it."""
    name: str                  # display name, e.g. "7E0" or "18DA10F1"
    at_sh: str                 # value for AT SH
    at_sp: str                 # protocol for AT SP ("6" = 11-bit/500k, "7" = 29-bit/500k)
    at_cp: str | None = None   # AT CP priority byte, 29-bit only
    at_cea: str | None = None   # ISO-TP extended-addressing target byte (BMW 6F1); None = off
    at_cra: str | None = None   # CAN RX-address filter (BMW responds on 0x600+addr)
    role: str = ""             # human note, e.g. "engine", "functional"

    @property
    def bits(self) -> int:
        return 29 if self.at_sp == "7" else 11


@dataclass(frozen=True)
class Block:
    """A contiguous PID range to sweep, e.g. 22F400..22F4FF."""
    name: str
    prefix: int                # e.g. 0x22F4 — service byte + high PID byte
    lo: int = 0x00
    hi: int = 0xFF
    note: str = ""

    def requests(self) -> Iterator[str]:
        width = 2 if self.prefix <= 0xFFFF else 3
        for n in range(self.lo, self.hi + 1):
            yield f"{self.prefix:0{width * 2}X}{n:02X}"


@dataclass
class VehiclePreset:
    name: str
    blocks: list[Block] = field(default_factory=list)
    probes: list[str] = field(default_factory=list)   # cheap census probes
    note: str = ""
    headers: list[Header] = field(default_factory=lambda: list(HEADERS_11BIT_PT + HEADERS_29BIT))


def validate_preset(p: VehiclePreset) -> None:
    """Validate every request a preset could ever emit. Called at startup."""
    for req in p.probes:
        validate_request(req)
    for b in p.blocks:
        validate_request(next(iter(b.requests())))     # prefix decides the mode


def validate_headers(headers: "list[Header]") -> None:
    """Validate every AT command a catalog Header could ever emit.

    IMPORTANT 5: validate_preset() covers `probes` and `blocks`, but
    Header.at_sh/at_sp/at_cp is the OTHER data-driven input to the transmit
    path (elm.py's set_header() builds "ATSP{at_sp}", "ATCP{at_cp}",
    "ATSH{at_sh}" directly from these fields) and was not gated at startup.
    A typo'd header -- and Ford/BMW headers are hand-authored, unlike the
    curated GM list -- was only caught mid-session, contradicting the
    README's claim that a malformed catalog fails before the session opens.
    Called at startup alongside validate_preset().
    """
    for h in headers:
        validate_at(f"ATSP{h.at_sp}")
        if h.at_cp:
            validate_at(f"ATCP{h.at_cp}")
        validate_at(f"ATSH{h.at_sh}")
        # BMW extended addressing / RX filter (None => the "off/clear" bare forms).
        validate_at(f"ATCEA{h.at_cea}" if h.at_cea else "ATCEA")
        if h.at_cra:
            validate_at(f"ATCRA{h.at_cra}")


# --- Candidate headers ------------------------------------------------------
HEADERS_11BIT = [
    Header("7DF", "7DF", "6", role="functional broadcast"),
    *[Header(f"7E{n:X}", f"7E{n:X}", "6", role=f"physical ECU {n}") for n in range(8)],
]

# Powertrain-only subset: functional broadcast + engine + the two adjacent
# powertrain ECUs (GM uses 7E2). Sweeping the full 7E0-7E7 range can reach
# chassis/ADAS modules — on the 2018 Audi Q5, 7E4 is a driver-assist controller,
# and reading its DIDs tripped pre-sense / "RPM too high" dashboard warnings. A
# powertrain scan must never probe 7E3-7E7. Presets default to this pool.
HEADERS_11BIT_PT = [h for h in HEADERS_11BIT if h.name in ("7DF", "7E0", "7E1", "7E2")]

# 29-bit physical addressing is 18DA<target><source>; F1 is the tester. The
# target list is curated: ISO-standard 10/18 plus addresses seen on GM Global B
# and plausible Ford module addresses. The census is what decides which exist.
_29BIT_TARGETS = [0x10, 0x18, 0x1A, 0x28, 0x00, 0x11, 0x17, 0x58, 0x60, 0x7E]
HEADERS_29BIT = [
    Header("18DB33F1", "DB33F1", "7", at_cp="18", role="functional broadcast"),
    *[Header(f"18DA{t:02X}F1", f"DA{t:02X}F1", "7", at_cp="18",
             role=f"physical target 0x{t:02X}") for t in _29BIT_TARGETS],
]

# --- BMW F10 headers --------------------------------------------------------
# BMW F-series enhanced diagnostics: tester ID 0x6F1, responses at 0x600+module,
# ISO-TP extended addressing (target byte). 11-bit/500k (at_sp "6"). The standard
# 7DF header turns extended addressing OFF (at_cea=None) so Mode-01 frames are clean.
BMW_STD = Header("7DF", "7DF", "6", role="functional broadcast (Mode-01)")
BMW_HEADERS = [
    BMW_STD,
    Header("BMW-612", "6F1", "6", at_cea="12", at_cra="612", role="DME — oil pressure 586F"),
    Header("BMW-618", "6F1", "6", at_cea="18", at_cra="618", role="oil temp DA25, ATF DA12"),
]

# --- Anchors ----------------------------------------------------------------
# Generic Mode-01 PIDs near-certain to exist on any OBD-II vehicle. Logged every
# cycle during `log` so correlation always has references, even on a platform
# whose enhanced map is entirely unknown.
ANCHORS = {
    "rpm":     "010C",
    "speed":   "010D",
    "load":    "0104",
    "coolant": "0105",
    "maf":     "0110",
    "baro":    "0133",
    "ambient": "0146",
}

# --- Vehicle presets --------------------------------------------------------
FORD_67 = VehiclePreset(
    name="ford",
    note="Ford 6.7L Power Stroke Super Duty. Every block is an UNVERIFIED sweep "
         "target from 2013-era community data — nothing here is a claim.",
    blocks=[
        Block("22F4xx", 0x22F4,
              note="densest known: ambient F446, EGT F478, oil temp F45C, regen F48B, fuel level F42F"),
        Block("2204xx", 0x2204, note="distance since regen 0434, soot 042C"),
        Block("2211xx", 0x2211, note="DPF differential pressure 116C"),
        Block("221Exx", 0x221E, note="transmission fluid temp 1E1C"),
        Block("2200xx", 0x2200, note="does Ford mirror generic PIDs the way GM does?"),
    ],
    probes=["0100", "22F446"],
)

GM_LZ0 = VehiclePreset(
    name="gm",
    note="2025 Sierra 3.0L Duramax LZ0. Known-answer vehicle — this preset is the "
         "regression fixture that proves the tool works before it goes to an unknown truck.",
    blocks=[
        Block("2200xx", 0x2200, note="mirror of the generic 01xx block"),
        Block("2219xx", 0x2219, note="transmission block: temp 221940, gear 22199A"),
    ],
    probes=["0100", "220005"],
)

BMW_F10 = VehiclePreset(
    name="bmw",
    note="2013 BMW 535i (F10, N55). DA25 oil temp @ 618 is confirmed-decodable; "
         "DA12 (ATF @ 618) and 586F (oil pressure @ 612) are confirmed DIDs whose "
         "SCALE is unverified — priority confirm-on-car targets. 42xx/45xx/58xx are "
         "UNVERIFIED community combustion blocks — nothing here is a claim.",
    blocks=[
        Block("22DAxx", 0x22DA, note="OBDb block: oil temp DA25, ATF DA12"),
        Block("2258xx", 0x2258, note="oil pressure 586F (unverified scale); community lambda 582C"),
        Block("2242xx", 0x2242, note="UNVERIFIED community: boost 4205, coolant 4300"),
        Block("2245xx", 0x2245, note="UNVERIFIED community: VANOS 4506/4507"),
    ],
    probes=["0100", "22DA25"],
    headers=BMW_HEADERS,
)

AUDI_Q5 = VehiclePreset(
    name="audi",
    note="2018 Audi Q5 (typ FY, MLB Evo, 2.0T EA888 gen3). Standard 11-bit UDS Mode-22: "
         "engine ECM 7E0/7E8, trans/TCM 7E1/7E9 (DL382 dual-clutch). The DIDs below are "
         "confirmed to ANSWER (OBDb 2018 capture) but their SCALING is unverified — confirm "
         "formulas on-car by correlation. No BMW-style extended addressing needed.",
    blocks=[
        Block("2211xx", 0x2211, note="engine 7E0: coolant 221135, oil temp 2211BE, boost-reg 2211CC"),
        Block("2216xx", 0x2216, note="engine 7E0: radiator-outlet coolant 221626"),
        Block("2220xx", 0x2220, note="engine 7E0: charge-air press 22202A, calc oil temp 2220A1"),
        Block("2221xx", 0x2221, note="trans 7E1: ATF/gearbox temp 222104"),
        Block("2210xx", 0x2210, note="trans 7E1: DL382 clutch temps/pressures"),
    ],
    probes=["0100", "221135", "222104"],
    # Audi powertrain only: engine 7E0, TCM 7E1. NEVER 7E2-7E7 — 7E4 is a
    # driver-assist module and reading it trips pre-sense warnings. Skip 29-bit
    # (Audi is 11-bit UDS).
    headers=[h for h in HEADERS_11BIT if h.name in ("7DF", "7E0", "7E1")],
)

JEEP_WS = VehiclePreset(
    name="jeep",
    note="2022 Jeep Wagoneer 5.7L Hemi eTorque (WS platform, ZF 8HP75). Addressing is "
         "the unusual part: the transmission answers on 29-bit 18DA18F1, NOT on 11-bit "
         "7E1 — a captured 2024 Wagoneer lists 7E1.7E9.2204FE as UNSUPPORTED, so the "
         "Grand Cherokee's 11-bit path does not transfer. Gear 22051A @ DA18 is VERIFIED "
         "on that capture. ATF temp 2204FE is VERIFIED on a Jeep Grand Cherokee 4th-gen "
         "at DA18 (OBDb signalset, degC = A-40) but has NEVER been probed on a Wagoneer "
         "at DA18 — it is the single reason this preset exists. Mode 01 is richer on the "
         "29-bit functional header DB33F1 than on 11-bit 7E0: engine oil temp 015C and "
         "torque 0162/0163 appear only there. eTorque 48V data is NOT reachable — its "
         "BPCM sits behind the Security Gateway, and the same capture lists "
         "6B4.22D410 (HV battery SoC) as UNSUPPORTED. Do not spend scan time on it.",
    blocks=[
        Block("2204xx", 0x2204, note="TCM: ATF temp candidate 04FE — the point of the scan"),
        Block("2205xx", 0x2205, note="TCM: gear 051A CONFIRMED on a 2024 Wagoneer"),
    ],
    probes=["0100", "22051A"],       # 22051A is the enhanced go/no-go, like GM's 220005
    # Powertrain only. 11-bit 7DF/7E0 plus the two 29-bit modules the capture
    # proves are alive: DB33F1 (functional) and DA18F1 (TCM). Deliberately NOT
    # DAC7 (tire pressure) or DA30 (wheel speeds/lateral G) — neither is
    # powertrain, and DA30 is chassis-adjacent.
    headers=[h for h in HEADERS_11BIT if h.name in ("7DF", "7E0")]
            + [h for h in HEADERS_29BIT if h.name in ("18DB33F1", "18DA18F1")],
)

PRESETS = {p.name: p for p in (FORD_67, GM_LZ0, BMW_F10, AUDI_Q5, JEEP_WS)}


# --- VIN-based preset auto-detection (--vehicle auto) ------------------------
# The VIN's WMI (World Manufacturer Identifier, chars 1-3) identifies the maker.
# Deliberately small — an unknown WMI makes `auto` abort with a clear message
# rather than guessing, so a scan never silently runs the wrong preset (which
# can sweep the wrong/unsafe modules; see HEADERS_11BIT_PT). Extend as vehicles
# are added.
WMI_PRESET = {
    "WAU": "audi", "WA1": "audi", "WUA": "audi", "TRU": "audi",                 # Audi
    "1FT": "ford", "1FD": "ford", "1FM": "ford", "1FA": "ford", "3FA": "ford",  # Ford
    "1GT": "gm",   "3GT": "gm",   "1GC": "gm",   "3GC": "gm",                    # GM (GMC/Chevy)
    "WBA": "bmw",  "WBS": "bmw",  "5UX": "bmw",  "4US": "bmw",                   # BMW
    "1C4": "jeep", "1J4": "jeep", "3C4": "jeep",                                 # Jeep (Stellantis)
}


def preset_for_vin(vin: str) -> "str | None":
    """Map a VIN's WMI (first 3 chars) to a preset name, or None if unknown."""
    if not vin or len(vin) < 3:
        return None
    return WMI_PRESET.get(vin[:3].upper())


def all_known_headers() -> "list[Header]":
    """Every header any preset might target, de-duplicated by name — the base
    11-bit/29-bit pools plus each preset's own headers (e.g. BMW's 612/618).
    run_log reconstructs a hit's header from its name using this; without the
    preset-specific ones, logging a BMW sweep would crash."""
    seen: "dict[str, Header]" = {}
    for h in HEADERS_11BIT + HEADERS_29BIT:
        seen.setdefault(h.name, h)
    for p in PRESETS.values():
        for h in p.headers:
            seen.setdefault(h.name, h)
    return list(seen.values())


def parse_vin_from_payload(payload: bytes) -> "str | None":
    """Extract the 17-char VIN from a Mode-09 PID-02 reply payload.

    The reply to 0902 is '49 02 01 <17 ASCII bytes>'; reply.classify() strips
    the '49 02' service + echoed-PID, leaving a leading count byte (0x01) then
    the VIN. Take the trailing 17 bytes and require them to be printable,
    alphanumeric VIN characters (a garbled read yields None; an unknown WMI is
    handled by the caller)."""
    if not payload or len(payload) < 17:
        return None
    try:
        vin = bytes(payload[-17:]).decode("ascii")
    except UnicodeDecodeError:
        return None
    vin = vin.upper()
    return vin if (len(vin) == 17 and vin.isalnum()) else None
