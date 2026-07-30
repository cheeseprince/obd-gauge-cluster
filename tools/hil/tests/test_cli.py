"""Tests for the CLI's non-obvious guarantees, driven with fakes — no board.

Two things are pinned here, both of them logic that a refactor could quietly
undo while every other test stayed green:

1. **Artifacts survive any unwind.** `_run_env` writes its captures into the
   caller's `art` dict from a `finally`, so a `RigError` or a mid-soak Ctrl-C
   still leaves the caller holding every log captured before the failure. This
   was the worst bug this branch fixed, and nothing pinned it: moving those
   assignments back out of `finally` used to pass the whole suite.
2. **The long path drains and reports like the short one.** The UART is drained
   on every soak step, `--soak 0` reports SKIP rather than a PASS for zero
   probes, a dead tap downgrades its checks, and a `pio` timeout still gets its
   partial output into `pio.log`.
"""
import argparse
import pathlib
import sys
import types
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

# --- why sys.modules is stubbed --------------------------------------------
#
# `hil.runner` imports pyserial at module scope and `hil.__main__` imports
# `hil.runner`, so importing the CLI at all normally requires pyserial. This
# suite must run with no board AND no pyserial: parse.py and ports.py are kept
# pure precisely so CI can run these tests without adding pyserial to the
# --require-hashes lockfile, and a test that dragged pyserial into CI would
# undo that. `unittest.mock` and `types` are stdlib, so the stub costs nothing.
#
# setdefault, not assignment: where real pyserial IS installed (the bench VM)
# the real module is used and the import succeeds by itself. Either way nothing
# below ever opens a port — every hardware call is replaced with a fake.
_serial = types.ModuleType("serial")
_serial.Serial = mock.MagicMock(name="serial.Serial")
_serial.SerialException = type("SerialException", (Exception,), {})
_tools = types.ModuleType("serial.tools")
_list_ports = types.ModuleType("serial.tools.list_ports")
_list_ports.comports = lambda: []
_tools.list_ports = _list_ports
_serial.tools = _tools
sys.modules.setdefault("serial", _serial)
sys.modules.setdefault("serial.tools", _tools)
sys.modules.setdefault("serial.tools.list_ports", _list_ports)

import pytest  # noqa: E402
from hil import __main__ as cli  # noqa: E402
from hil.parse import Status  # noqa: E402
from hil.ports import PortSet  # noqa: E402
from hil.runner import RigError  # noqa: E402

PORTS = PortSet(native="/dev/fake-native", uart="/dev/fake-uart")

HEALTHY_UART = (
    "ESP-ROM:esp32s3-20210327\n"
    "rst:0x15 (USB_UART_CHIP_RESET),boot:0xa (SPI_FAST_FLASH_BOOT)\n"
)
HEALTHY_NATIVE = (
    "[BOOT] env=crowpanel ver=local git=local profile=generic\n"
    "[BOOT] psram=8MB flash=16MB reset=1 heap=193668\n"
    "[encoder] found\n"
)
# One reply covering both console probes: `_run_env` concatenates every reply
# window into one native log, so a single canned reply satisfies the 'n' ->
# [THEME] and 's' -> [MOCK] checks without the fake having to model which key it
# was handed.
HEALTHY_REPLY = "[THEME] mode=ON\n[MOCK] alarm sweep\n"


class FakeUart:
    """Stands in for UartTap: a context manager with .text, .error, .read_new().

    Records how many times it was drained, which is how the soak-drain test
    distinguishes "drained at both ends" from "drained every step".
    """

    def __init__(self, port, error=None):
        self.port = port
        self.text = ""
        self.error = error
        self.reads = 0
        self.exited = False

    def __enter__(self):
        self.text += HEALTHY_UART  # as if the boot landed while the tap was open
        return self

    def __exit__(self, *exc):
        self.exited = True
        return False  # never swallow the exception under test

    def read_new(self):
        self.reads += 1
        return ""


def _install(monkeypatch, *, uart=None, flash=None, capture=None, send=None):
    """Replace every hardware call `_run_env` makes. Returns the FakeUart used."""
    tap = uart if uart is not None else FakeUart(PORTS.uart)
    monkeypatch.setattr(cli, "UartTap", lambda port: tap)
    monkeypatch.setattr(cli, "flash", flash or (lambda env, port: (True, "pio ok")))
    monkeypatch.setattr(cli, "capture_native",
                        capture or (lambda port, secs: HEALTHY_NATIVE))
    monkeypatch.setattr(cli, "send_key_and_capture",
                        send or (lambda port, key, secs: HEALTHY_REPLY))
    return tap


def _args(*argv):
    return cli._args(["--env", "crowpanel", *argv])


def _scripted_send(script):
    """A send_key_and_capture whose Nth call does script[N] (str, or an exception
    instance/class to raise). Anything past the end repeats the last entry."""
    calls = []

    def send(port, key, secs):
        step = script[min(len(calls), len(script) - 1)]
        calls.append(key)
        if isinstance(step, BaseException) or (
                isinstance(step, type) and issubclass(step, BaseException)):
            raise step
        return step

    send.calls = calls
    return send


# --- 1. artifacts survive any unwind ---------------------------------------

@pytest.mark.parametrize("boom", [RigError("port vanished"), KeyboardInterrupt()])
def test_artifacts_are_populated_even_when_the_run_unwinds(monkeypatch, boom):
    """THE regression test for this branch's worst bug.

    A RigError and a mid-soak Ctrl-C are the two realistic unwinds (the soak is
    300 s by default, so Ctrl-C is arguably the likeliest). Either way the
    caller's `art` must come back holding the UART text, the boot capture and
    whatever soak output existed — that is exactly the evidence a rig error is
    diagnosed from, and returning a dict instead of mutating the caller's would
    lose all of it.
    """
    # Calls in order: the boot-window 'n' probe, the 's' probe (mock env), then
    # the soak probes — so the failure lands mid-soak, after real soak output.
    send = _scripted_send([HEALTHY_REPLY, HEALTHY_REPLY, "[THEME] soak ok\n", boom])
    tap = _install(monkeypatch, send=send)
    art: dict = {"env": "crowpanel"}

    with pytest.raises(type(boom)):
        cli._run_env("crowpanel", PORTS, _args("--soak", "300"), art)

    assert art["uart"] == HEALTHY_UART, "UART text must survive the unwind"
    assert art["uart_error"] is None
    assert art["native_boot"].startswith("[BOOT] env=crowpanel"), \
        "the boot capture must survive the unwind"
    assert "[THEME] soak ok" in art["native_soak"], \
        "soak output captured before the failure must survive too"
    assert tap.exited, "the tap must still be closed on the way out"


def test_a_flash_timeout_still_writes_its_partial_pio_log(monkeypatch):
    # When `pio` blows its 600 s budget there is no return code and no complete
    # log, so the partial output is the ONLY evidence of what wedged — and it
    # used to be dropped, leaving pio.log empty on the one run that needed it.
    def flash(env, port):
        raise RigError("pio exceeded 600 s", partial_output="Building...\nwedged here\n")

    _install(monkeypatch, flash=flash)
    art: dict = {"env": "crowpanel"}
    with pytest.raises(RigError):
        cli._run_env("crowpanel", PORTS, _args(), art)
    assert "wedged here" in art["pio"]


def test_a_reported_flash_failure_records_the_full_pio_output(monkeypatch):
    # The other flash-failure shape: pio ran and returned non-zero. Still a rig
    # error (never assert against a stale binary), still must keep the log.
    _install(monkeypatch, flash=lambda env, port: (False, "Linking...\nundefined ref\n"))
    art: dict = {"env": "crowpanel"}
    with pytest.raises(RigError):
        cli._run_env("crowpanel", PORTS, _args(), art)
    assert "undefined ref" in art["pio"]


# --- 2. the long path behaves like the short one ---------------------------

def test_the_uart_is_drained_on_every_soak_step(monkeypatch):
    """115200 baud x 300 s does not fit in the OS tty buffer.

    Before this, the only drains were the post-boot read and __exit__, so the
    whole soak window had to survive in the kernel buffer and any overflow was
    silent — while this rig's primary deliverable IS the serial log.
    """
    tap = _install(monkeypatch)
    cli._run_env("crowpanel", PORTS, _args("--soak", "300"), art={"env": "crowpanel"})
    # 300 s / 30 s steps = 10 probes, plus the one after the boot window.
    assert tap.reads >= 11, f"only {tap.reads} drains for a 10-step soak"


def test_soak_zero_reports_skip_not_a_pass_after_zero_probes(monkeypatch):
    # `--soak 0` runs the loop zero times. Reporting PASS for a check that never
    # executed is the exact failure this rig exists to prevent, so it is an
    # honest SKIP — which correctly makes the run exit 3.
    _install(monkeypatch)
    verdicts = cli._run_env("crowpanel", PORTS, _args("--soak", "0"),
                            art={"env": "crowpanel"})
    soak = next(v for v in verdicts if v.check == "soak liveness")
    assert soak.status is Status.SKIP
    assert "--soak 0" in soak.detail


def test_a_normal_soak_still_passes(monkeypatch):
    # The control for the test above: the SKIP must be specific to soak<=0, not
    # a blanket downgrade that stops the check ever passing.
    _install(monkeypatch)
    verdicts = cli._run_env("crowpanel", PORTS, _args("--soak", "60"),
                            art={"env": "crowpanel"})
    soak = next(v for v in verdicts if v.check == "soak liveness")
    assert soak.status is Status.PASS


def test_a_dead_tap_downgrades_the_uart_checks_end_to_end(monkeypatch):
    # The tap's .text keeps everything captured before it died, so all three
    # UART checks would otherwise PASS off retained evidence and the run would
    # report "all checks green" having watched a fraction of its window.
    tap = FakeUart(PORTS.uart, error="UART tap died: [Errno 5] I/O error")
    _install(monkeypatch, uart=tap)
    verdicts = cli._run_env("crowpanel", PORTS, _args("--soak", "30"),
                            art={"env": "crowpanel"})
    by_name = {v.check: v for v in verdicts}
    for name in ("boot mode", "no panic", "no task_wdt spam"):
        assert by_name[name].status is Status.SKIP, f"{name} still reads as PASS"
    from hil.parse import exit_code
    assert exit_code(verdicts) != 0, "a dead tap must not exit 0"


def test_the_probe_windows_are_five_seconds(monkeypatch):
    # Pinned so nobody tightens them back to 2.0 s and reintroduces the
    # 1-in-3 'mock alarm ack' flake measured on hardware 2026-07-29. 'n'
    # repaints the whole 480x320 panel over SPI and the 's' key is not read
    # until loop() finishes that work, so the budget must cover both.
    seen = []

    def send(port, key, secs):
        seen.append((key, secs))
        return HEALTHY_REPLY

    _install(monkeypatch, send=send)
    cli._run_env("crowpanel", PORTS, _args("--soak", "0"), art={"env": "crowpanel"})
    assert seen == [("n", 5.0), ("s", 5.0)]


def test_soak_final_step_reply_window_is_never_below_the_ack_floor(monkeypatch):
    """`--soak 31` leaves a final step of `31 mod 30 == 1.0` s. Before this
    fix that 1.0 s `step` was handed straight to `send_key_and_capture` as
    the reply BUDGET too -- well under the 2.0 s window already proven
    unreliable on hardware (see `test_the_probe_windows_are_five_seconds`).
    The reply window must be floored at `ACK_WINDOW_S` regardless of how far
    `elapsed` actually advances on that step."""
    seen = []

    def send(port, key, secs):
        seen.append((key, secs))
        return HEALTHY_REPLY

    _install(monkeypatch, send=send)
    cli._run_env("crowpanel", PORTS, _args("--soak", "31"), art={"env": "crowpanel"})
    # First two calls are the boot-window 'n' and 's' probes; the rest are the
    # soak's steps (30 s, then the 1 s tail -- floored to ACK_WINDOW_S).
    soak_calls = seen[2:]
    assert soak_calls == [("n", 30.0), ("n", cli.ACK_WINDOW_S)]
    assert all(secs >= cli.ACK_WINDOW_S for _, secs in soak_calls), \
        "no soak reply window may fall below the ACK_WINDOW_S floor"


def test_soak_step_count_and_termination_unchanged_for_an_exact_multiple(monkeypatch):
    """The floor must only ever WIDEN a reply window, never change how many
    steps run or when the loop stops. `--soak 60` is an exact multiple of the
    30 s step (the same shape the hardware-validated 60 s and 300 s runs
    use), so this pins that the fix is a no-op there: still exactly two 30 s
    steps, each already at or above the floor."""
    seen = []

    def send(port, key, secs):
        seen.append((key, secs))
        return HEALTHY_REPLY

    _install(monkeypatch, send=send)
    cli._run_env("crowpanel", PORTS, _args("--soak", "60"), art={"env": "crowpanel"})
    soak_calls = seen[2:]
    assert soak_calls == [("n", 30.0), ("n", 30.0)], \
        "60s / 30s-step must still be exactly two unfloored 30 s steps"


def test_an_ack_arriving_only_during_the_soak_still_passes(monkeypatch):
    """The 2026-07-29 flake, end to end.

    Both probe windows come back EMPTY and the ack lands in the first soak
    reply instead — which is exactly what happened on hardware, `[MOCK] alarm
    sweep` present in the capture but a beat past its window. Checks 7 and 8
    assert that the console answered and the render did not kill the firmware,
    not that it answered inside a timer, so this must PASS.
    """
    send = _scripted_send(["", "", "[THEME] mode=ON\n[MOCK] alarm sweep\n"])
    _install(monkeypatch, send=send)
    verdicts = cli._run_env("crowpanel", PORTS, _args("--soak", "60"),
                            art={"env": "crowpanel"})
    by_name = {v.check: v for v in verdicts}
    assert by_name["theme ack ('n')"].status is Status.PASS
    assert by_name["mock alarm ack ('s')"].status is Status.PASS
    # ...and latency is still asserted, just by the check that owns it.
    assert by_name["soak liveness"].status is Status.PASS


def test_a_console_that_never_answers_at_all_still_fails(monkeypatch):
    # The negative that keeps the widened scope honest: no ack anywhere in the
    # capture is positive evidence of a wedged console (a key WAS written), so
    # both acks FAIL — not SKIP — and the soak's latency check fails too.
    _install(monkeypatch, send=lambda port, key, secs: "")
    verdicts = cli._run_env("crowpanel", PORTS, _args("--soak", "60"),
                            art={"env": "crowpanel"})
    by_name = {v.check: v for v in verdicts}
    assert by_name["theme ack ('n')"].status is Status.FAIL
    assert by_name["mock alarm ack ('s')"].status is Status.FAIL
    assert by_name["soak liveness"].status is Status.FAIL
    from hil.parse import exit_code
    assert exit_code(verdicts) == 1


def test_a_healthy_run_passes_every_check(monkeypatch):
    # The baseline this all sits on: with healthy fakes the rig is green, so the
    # SKIPs above are real signal rather than a rig that can no longer pass.
    _install(monkeypatch)
    verdicts = cli._run_env("crowpanel", PORTS, _args("--soak", "60"),
                            art={"env": "crowpanel"})
    assert [v.status for v in verdicts] == [Status.PASS] * len(verdicts), \
        [v for v in verdicts if v.status is not Status.PASS]


# --- 3. exit codes at the main() boundary ----------------------------------

def test_unhandled_exceptions_exit_2_not_1(monkeypatch, capsys):
    # Exit 1 means "the firmware is broken". Python's default for an uncaught
    # exception is also 1, so a bug in the rig (or an OSError writing artifacts)
    # used to be indistinguishable from a real firmware failure.
    monkeypatch.setattr(cli, "main", lambda: (_ for _ in ()).throw(OSError("read-only fs")))
    assert cli._cli() == 2
    err = capsys.readouterr().err
    assert "read-only fs" in err, "the traceback must not be swallowed"
    assert "RIG ERROR" in err


def test_ctrl_c_outside_the_guarded_call_exits_2(monkeypatch):
    monkeypatch.setattr(cli, "main", lambda: (_ for _ in ()).throw(KeyboardInterrupt()))
    assert cli._cli() == 2


def test_a_rig_error_in_the_second_env_still_renders_the_first(capsys):
    # Exit-2 precedence is right; suppressing the table is not. A genuine FAIL
    # found in `crowpanel` must not become invisible because `crowpanel_obd`
    # errored afterwards.
    from hil.parse import Verdict
    fails = {"crowpanel": [Verdict("no task_wdt spam", Status.FAIL, "40 lines")]}
    cli._render_verdict_table(fails)
    assert "FAIL" in capsys.readouterr().out


def test_the_verdict_table_tolerates_an_env_with_no_verdicts(capsys):
    # It is rendered on the rig-error path now, where the failing env
    # contributed nothing.
    assert cli._render_verdict_table({"crowpanel": []}) == (0, 0)
    assert cli._render_verdict_table({}) == (0, 0)
    capsys.readouterr()


def test_the_dead_tap_warning_says_what_actually_happens(capsys):
    # The old message claimed the affected checks "may read as SKIP" while they
    # in fact read as PASS — a sentence that would talk a reader out of
    # investigating. It must now describe the real behaviour.
    cli._print_uart_error({"env": "crowpanel", "uart_error": "cable out"})
    out = capsys.readouterr().out
    assert "UART TAP DIED" in out and "cable out" in out
    assert "SKIP" in out and "FAIL" in out


def test_verdict_json_is_written_on_the_rig_error_path(tmp_path, monkeypatch):
    # README and DESIGN.md both promise every run writes verdict.json, and a rig
    # error is exactly when the partial record is wanted.
    monkeypatch.setattr(cli, "RUNS_DIR", tmp_path)
    d = cli._write_run({"env": "crowpanel", "uart": "boot\n",
                        "uart_error": "cable out"}, [])
    assert (d / "verdict.json").exists()
    assert "cable out" in (d / "verdict.json").read_text()
    assert (d / "uart.log").read_text() == "boot\n"


def test_args_defaults_match_the_documented_cli():
    # The five real flags, as README and DESIGN.md describe them.
    a = cli._args([])
    assert isinstance(a, argparse.Namespace)
    assert (a.env, a.expect_knob, a.soak, a.boot_window, a.allow_skips) == (
        "both", "auto", 300.0, 15.0, False)
