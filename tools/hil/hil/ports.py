"""Resolve the two USB nodes this rig needs, by VID:PID.

Never hardcode /dev/ttyACM0. Two reasons, both real:
  * ttyACMn numbering shifts across the S3's re-enumeration on every reset —
    docs/INSTALL.md:57 already warns against globbing the port.
  * the board's /dev/serial/by-id string embeds its MAC address, which has no
    business being committed to a public repository.

This module takes an injected device list rather than calling pyserial, so it
imports cleanly in CI where no board and no pyserial exist — that purity is what
lets the rig's tests run without a lockfile entry for pyserial, so keep it.
`runner.py` is the only place the real serial.tools.list_ports.comports() call
lives, and reading the environment-variable overrides (HIL_PORT_NATIVE /
HIL_PORT_UART) is likewise runner.py's job — this module only accepts them as
parameters.
"""
from __future__ import annotations

from dataclasses import dataclass

# ESP32-S3 native USB (USB-Serial/JTAG): flashing + the app's Serial via CDC.
NATIVE_VID_PID = (0x303A, 0x1001)
# CH340 bridge on UART0: ROM log, IDF ESP_LOG, panics. Survives resets.
UART_VID_PID = (0x1A86, 0x7522)


class PortResolutionError(RuntimeError):
    """No board, or an ambiguous one.

    This is always a rig error (the CLI should exit 2, "the rig is broken"),
    never a test failure ("the firmware is broken", exit 1). A HIL run that
    reports PASS with no hardware attached is worse than having no rig at
    all, so any code path that cannot prove it is talking to real hardware
    must raise this rather than let checks silently SKIP their way to green.
    """


@dataclass(frozen=True)
class DeviceEntry:
    """One row from serial.tools.list_ports.comports(), captured as data.

    vid/pid are Optional because pyserial reports None for some devices
    (Bluetooth rfcomm shims, some virtual ports) — those must be safely
    skipped during matching, never crash the resolver.
    """
    device: str
    vid: int | None
    pid: int | None


@dataclass(frozen=True)
class PortSet:
    """The resolved pair the rig needs for one run."""
    native: str
    uart: str


def _one(
    entries: list[DeviceEntry], want: tuple[int, int], label: str, override_var: str
) -> str:
    """Find exactly one entry matching `want`, or raise.

    Two failure shapes, both real:
      * zero matches -> board not plugged in (or CI with no hardware at all).
      * two-or-more matches -> two boards attached; picking one silently
        would risk flashing the wrong device, so refuse instead of guessing.
    Both messages name the VID:PID searched for and *only their own*
    override env var as the escape hatch — deliberately not the other
    port's var name too, so a native-only message can never accidentally
    contain the substring "UART" (or vice versa). That distinction is what
    lets a test tell "both ports reported missing" apart from "only one
    was", which resolve_ports()'s error aggregation depends on.
    """
    hits = [e.device for e in entries if (e.vid, e.pid) == want]
    if not hits:
        raise PortResolutionError(
            f"no {label} port found (looking for "
            f"{want[0]:04x}:{want[1]:04x}); is the board plugged in? "
            f"override with the {override_var} env var"
        )
    if len(hits) > 1:
        raise PortResolutionError(
            f"more than one {label} port found ({', '.join(sorted(hits))}); "
            f"refusing to guess which one to use — disambiguate with the "
            f"{override_var} override env var"
        )
    return hits[0]


def resolve_ports(
    entries: list[DeviceEntry],
    native_override: str | None = None,
    uart_override: str | None = None,
) -> PortSet:
    """Resolve both ports, reporting *both* problems at once when both are absent.

    Overrides skip discovery entirely, so a second board — or one behind a
    hub that reports different IDs — stays usable without touching this
    module. Discovery failures for native and UART are collected
    independently before either is raised: someone debugging a fresh machine
    should learn everything that's wrong in one run, not peel it one error
    at a time by re-running after each fix.
    """
    problems: list[str] = []
    native = uart = None

    if native_override:
        native = native_override
    else:
        try:
            native = _one(entries, NATIVE_VID_PID, "native USB", "HIL_PORT_NATIVE")
        except PortResolutionError as e:
            problems.append(str(e))

    if uart_override:
        uart = uart_override
    else:
        try:
            uart = _one(entries, UART_VID_PID, "UART bridge", "HIL_PORT_UART")
        except PortResolutionError as e:
            problems.append(str(e))

    if problems:
        raise PortResolutionError("; ".join(problems))
    # Unreachable in practice: if we get here, both branches above assigned a
    # value (from an override or from a successful _one() call) since any
    # failure would have appended to `problems` and raised above. The assert
    # documents that invariant for a reader, and for type-checkers.
    assert native and uart
    return PortSet(native=native, uart=uart)
