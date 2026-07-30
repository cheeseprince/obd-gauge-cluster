"""The hardware layer: flashing, resetting, and capturing both ports.

Two hard-won constraints govern this file.

1. NEVER hold the native port open while pio or esptool is using it. A reader
   contending with esptool on the same node returns protocol garbage instead of
   a log — measured, not theorised.
2. The native port re-enumerates on every reset, so opening it is a retry loop,
   not a single call. The UART bridge does not, which is why it is opened once
   and held for the whole run: if the native reopen loses the race, the UART
   capture still holds the boot reason and any panic.
"""
from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

import serial
from serial.tools import list_ports

from .ports import DeviceEntry, PortResolutionError, PortSet, resolve_ports

BAUD = 115200
REPO_ROOT = Path(__file__).resolve().parents[3]


class RigError(RuntimeError):
    """Environment/hardware problem. Maps to exit code 2, never a test failure."""


def discover_ports(native_override=None, uart_override=None) -> PortSet:
    entries = [
        DeviceEntry(device=p.device, vid=p.vid, pid=p.pid)
        for p in list_ports.comports()
    ]
    try:
        return resolve_ports(
            entries,
            native_override=native_override or os.environ.get("HIL_PORT_NATIVE"),
            uart_override=uart_override or os.environ.get("HIL_PORT_UART"),
        )
    except PortResolutionError as e:
        raise RigError(str(e)) from e


def _open(port: str) -> serial.Serial:
    """Open a port, translating the two failures that actually happen into
    actionable messages instead of tracebacks."""
    try:
        return serial.Serial(port, BAUD, timeout=0.2)
    except serial.SerialException as e:
        msg = str(e)
        if "Permission denied" in msg or isinstance(e.__cause__, PermissionError):
            raise RigError(
                f"permission denied opening {port}. Add the user to the dialout "
                f"group (`sudo usermod -aG dialout $USER`, then log in again), or "
                f"for this session only: `sudo chmod 666 {port}`"
            ) from e
        raise RigError(f"cannot open {port}: {msg}") from e


class UartTap:
    """The always-on capture. Opened before flashing and held for the whole run,
    so boot evidence survives a lost native-reopen race."""

    def __init__(self, port: str):
        self._port = port
        self._ser: serial.Serial | None = None
        self.text = ""

    def __enter__(self) -> "UartTap":
        self._ser = _open(self._port)
        return self

    def __exit__(self, *exc) -> None:
        if self._ser:
            try:
                self.read_new()
            finally:
                self._ser.close()

    def read_new(self) -> str:
        """Drain whatever has arrived, append it to .text, and return just the
        new bytes. Decoding is lossy on purpose: a reset mid-frame produces
        partial UTF-8, and losing a byte beats raising."""
        assert self._ser
        chunk = self._ser.read(65536).decode("utf-8", errors="replace")
        self.text += chunk
        return chunk


def flash(env: str, native_port: str) -> tuple[bool, str]:
    """Build and upload. The native port must NOT be open when this is called.

    Returns (ok, combined output). A failure here is a rig error, and the caller
    must not go on to assert against a stale binary.
    """
    cmd = ["pio", "run", "-e", env, "-t", "upload", "--upload-port", native_port]
    proc = subprocess.run(
        cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=600
    )
    return proc.returncode == 0, proc.stdout + proc.stderr


def capture_native(port: str, seconds: float, reopen_budget: float = 5.0) -> str:
    """Capture the app log, absorbing the post-reset re-enumeration.

    The node disappears and returns after a reset, so opening it is a retry loop.
    Returns "" if it never came back — the caller turns that into SKIP, never
    into PASS.
    """
    deadline = time.monotonic() + reopen_budget
    ser = None
    while time.monotonic() < deadline:
        try:
            ser = serial.Serial(port, BAUD, timeout=0.2)
            break
        except serial.SerialException:
            time.sleep(0.25)
    if ser is None:
        return ""

    out = []
    try:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            out.append(ser.read(65536).decode("utf-8", errors="replace"))
    finally:
        ser.close()
    return "".join(out)


def send_key_and_capture(port: str, key: str, seconds: float) -> str:
    """Write one console key and capture the reply window.

    The firmware's console reads a single char per loop pass (main.cpp:244), so
    one byte is the whole protocol.
    """
    ser = _open(port)
    try:
        ser.reset_input_buffer()
        ser.write(key.encode())
        ser.flush()
        out = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            out.append(ser.read(65536).decode("utf-8", errors="replace"))
        return "".join(out)
    finally:
        ser.close()
