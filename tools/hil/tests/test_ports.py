"""Tests for VID:PID port resolution — the pure half of the HIL rig's device layer.

Faked `DeviceEntry` lists stand in for `serial.tools.list_ports.comports()`, so
these tests run with no board and no pyserial installed (`ports.py` is
stdlib-only; see `hil/ports.py`'s module docstring for why).
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import pytest
from hil.ports import DeviceEntry, PortResolutionError, resolve_ports

NATIVE = DeviceEntry(device="/dev/ttyACM0", vid=0x303A, pid=0x1001)
UART = DeviceEntry(device="/dev/ttyUSB0", vid=0x1A86, pid=0x7522)
UNRELATED = DeviceEntry(device="/dev/ttyUSB9", vid=0x0403, pid=0x6001)


def test_resolves_both_ports_by_vid_pid():
    p = resolve_ports([UNRELATED, UART, NATIVE])
    assert p.native == "/dev/ttyACM0"
    assert p.uart == "/dev/ttyUSB0"


def test_order_does_not_matter():
    assert resolve_ports([NATIVE, UART]) == resolve_ports([UART, NATIVE])


def test_missing_native_is_a_rig_error():
    # Exit code 2 territory: no board is never a test failure.
    with pytest.raises(PortResolutionError, match="native"):
        resolve_ports([UART])


def test_missing_uart_is_a_rig_error():
    with pytest.raises(PortResolutionError, match="UART"):
        resolve_ports([NATIVE])


def test_empty_list_names_both_missing_ports():
    with pytest.raises(PortResolutionError) as e:
        resolve_ports([])
    assert "native" in str(e.value) and "UART" in str(e.value)


def test_overrides_win_and_skip_discovery():
    # A second board, or a board behind a USB hub that reports odd IDs.
    p = resolve_ports([], native_override="/dev/x", uart_override="/dev/y")
    assert p.native == "/dev/x" and p.uart == "/dev/y"


def test_a_single_override_still_requires_discovering_the_other():
    p = resolve_ports([UART], native_override="/dev/x")
    assert p.native == "/dev/x" and p.uart == "/dev/ttyUSB0"


def test_ambiguous_duplicate_native_is_an_error():
    # Two S3s attached: refuse rather than silently flashing the wrong one.
    dup = DeviceEntry(device="/dev/ttyACM1", vid=0x303A, pid=0x1001)
    with pytest.raises(PortResolutionError, match="more than one"):
        resolve_ports([NATIVE, dup, UART])


def test_entries_with_none_vid_pid_do_not_crash():
    # pyserial returns vid=None/pid=None for some devices (e.g. Bluetooth
    # rfcomm shims, some virtual ports). These must be safely skipped rather
    # than raising or spuriously matching.
    ghost = DeviceEntry(device="/dev/rfcomm0", vid=None, pid=None)
    p = resolve_ports([ghost, NATIVE, UART])
    assert p.native == "/dev/ttyACM0"
    assert p.uart == "/dev/ttyUSB0"


def test_error_message_names_vid_pid_and_override_escape_hatch():
    # Actionable errors: the VID:PID searched for, and how to route around it.
    with pytest.raises(PortResolutionError) as e:
        resolve_ports([])
    msg = str(e.value)
    assert "303a:1001" in msg
    assert "1a86:7522" in msg
    assert "override" in msg.lower()
