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
# Shared across every open() call site so they don't drift independently —
# 0.2 s is short enough that the bounded read loops below stay responsive.
READ_TIMEOUT = 0.2
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


def _permission_error(e: serial.SerialException) -> bool:
    """True when a failed open is a permissions problem rather than an absent node.

    Worth distinguishing: an absent node during a capture is the expected
    re-enumeration race and should be retried, while a permissions problem will
    never resolve on its own and has a specific, actionable fix. Shared between
    `_open` (one-shot opens) and `capture_native`'s retry loop so both classify
    failures identically instead of drifting apart.
    """
    return "Permission denied" in str(e) or isinstance(e.__cause__, PermissionError)


def _permission_hint(port: str) -> str:
    """The actionable remedy for a permission-denied open, shared by every caller
    that needs to surface it (fresh machines hit this before anything else)."""
    return (
        f"permission denied opening {port}. Add the user to the dialout group "
        f"(`sudo usermod -aG dialout $USER`, then log in again), or for this "
        f"session only: `sudo chmod 666 {port}`"
    )


def _open(port: str) -> serial.Serial:
    """Open a port, translating the two failures that actually happen into
    actionable messages instead of tracebacks."""
    try:
        return serial.Serial(port, BAUD, timeout=READ_TIMEOUT)
    except serial.SerialException as e:
        if _permission_error(e):
            raise RigError(_permission_hint(port)) from e
        raise RigError(f"cannot open {port}: {e}") from e


class UartTap:
    """The always-on capture. Opened before flashing and held for the whole run,
    so boot evidence survives a lost native-reopen race."""

    def __init__(self, port: str):
        self._port = port
        self._ser: serial.Serial | None = None
        self.text = ""
        self.error: str | None = None  # set if the tap died mid-run

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
        """Drain whatever has arrived, append it to .text, and return the new bytes.

        Never raises. `__exit__` calls this on the way out, and raising from a
        context manager's exit would replace whatever exception was already
        propagating with a less useful one. The UART bridge does NOT
        re-enumerate on reset (unlike the native port), so losing it mid-run is
        a real problem — cable out, hub reset, port stolen — not the expected
        re-enumeration race. Record that on `.error` for the caller to surface;
        `.text` keeps whatever was captured before the tap died, which is still
        the best evidence available. Decoding is lossy on purpose: a reset
        mid-frame produces partial UTF-8, and losing a byte beats raising.
        """
        if self._ser is None:
            return ""
        try:
            chunk = self._ser.read(65536).decode("utf-8", errors="replace")
        except serial.SerialException as e:
            self.error = f"UART tap died: {e}"
            return ""
        self.text += chunk
        return chunk


def flash(env: str, native_port: str) -> tuple[bool, str]:
    """Build and upload. The native port must NOT be open when this is called.

    Returns (ok, combined output) for a *firmware*-shaped failure (pio ran and
    reported a build/upload error). A missing `pio` or a wedged build are
    environment problems, not firmware ones — those raise RigError instead of
    coming back as (False, ...), so the CLI exits 2 rather than reporting a
    test failure against nothing.
    """
    cmd = ["pio", "run", "-e", env, "-t", "upload", "--upload-port", native_port]
    try:
        proc = subprocess.run(
            cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=600
        )
    except FileNotFoundError as e:
        # pio not installed or not on PATH — an environment problem, not a
        # firmware one, so it must reach the CLI as exit 2.
        raise RigError(
            "`pio` is not on PATH — install PlatformIO or activate its venv"
        ) from e
    except subprocess.TimeoutExpired as e:
        # A cold ~/.platformio can legitimately take minutes to pull the
        # pinned platform down, which is why the budget is 600 s; blowing
        # through it means something is actually wedged, not just slow.
        partial = (e.stdout or "") + (e.stderr or "")
        raise RigError(
            f"`pio run -e {env} -t upload` exceeded 600 s and was killed; "
            f"last output:\n{partial[-2000:]}"
        ) from e
    return proc.returncode == 0, proc.stdout + proc.stderr


def capture_native(port: str, seconds: float, reopen_budget: float = 5.0) -> str:
    """Capture the app log, absorbing the post-reset re-enumeration.

    The node disappears and returns after a reset, so opening it is a retry loop.
    Returns "" if it never came back — the caller turns that into SKIP, never
    into PASS.

    A disconnect *during* the capture (SerialException mid-read) is treated as
    a firmware event, not a rig fault: the cause is almost always the board
    resetting or panicking, which is the thing under test. Whatever text was
    captured before the drop is returned rather than discarded — it may hold
    the panic itself, and the UART tap holds the boot reason regardless. The
    verdict layer reaches the right outcome from a truncated capture on its
    own (no [BOOT] -> SKIP; a missing reply -> FAIL) without needing a
    traceback to tell it something went wrong.
    """
    deadline = time.monotonic() + reopen_budget
    ser = None
    while time.monotonic() < deadline:
        try:
            ser = serial.Serial(port, BAUD, timeout=READ_TIMEOUT)
            break
        except serial.SerialException as e:
            if _permission_error(e):
                # Retrying will never fix this — it isn't the re-enumeration
                # race, it's a fixable local misconfiguration. Reporting it as
                # "" (-> SKIP) would hide an actionable problem behind a
                # benign-looking "board didn't come back".
                raise RigError(_permission_hint(port)) from e
            time.sleep(0.25)
    if ser is None:
        return ""

    out = []
    try:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            out.append(ser.read(65536).decode("utf-8", errors="replace"))
    except serial.SerialException:
        # Node vanished mid-capture — see docstring: keep what we have.
        pass
    finally:
        ser.close()
    return "".join(out)


def send_key_and_capture(port: str, key: str, seconds: float) -> str:
    """Write one console key and capture the reply window.

    The firmware's console reads a single char per loop pass (main.cpp:244), so
    one byte is the whole protocol.

    As in capture_native, a disconnect while waiting for the reply is a
    firmware event (the board reset while being poked), not a rig fault: the
    partial window is returned, and a missing ack then surfaces as a FAILED
    liveness check at the verdict layer — the correct outcome, reached without
    a traceback.
    """
    ser = _open(port)
    try:
        out = []
        try:
            ser.reset_input_buffer()
            ser.write(key.encode())
            ser.flush()
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                out.append(ser.read(65536).decode("utf-8", errors="replace"))
        except serial.SerialException:
            # Board reset while we were poking it — see docstring.
            pass
        return "".join(out)
    finally:
        ser.close()
