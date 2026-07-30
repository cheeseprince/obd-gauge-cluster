"""CLI entry point: sequence a run, print a verdict table, write artifacts.

Exit codes exist so "the rig is broken" can never be misread as "the firmware is
good":
    0  every check ran and passed
    1  a check failed          -> firmware problem
    2  rig/environment error   -> no board, port busy, permission denied, flash failed
    3  passed, but checks were SKIPPED (unless --allow-skips)

Code 3 is deliberate. A run where the native port never came back would
otherwise report "all assertions passed" while silently testing a fraction of
what it claims.

Nothing may leak exit 1 by accident: Python's default for an uncaught exception
is also 1, so the runner import below and `_cli()` at the bottom both force any
unexpected failure to 2. The 0/1/3 decision itself lives in `parse.exit_code`,
where it is pure and unit-tested.
"""
from __future__ import annotations

import argparse
import datetime
import json
import sys
import traceback
from pathlib import Path

from . import parse
from .parse import Status, Verdict
from .ports import PortSet

try:
    from .runner import (
        RigError,
        UartTap,
        capture_native,
        discover_ports,
        flash,
        send_key_and_capture,
    )
except ImportError as _import_err:  # pragma: no cover - depends on host state
    # `python3 -m hil` imports this module, which imports runner, which imports
    # pyserial at module scope. On a machine without pyserial — a documented
    # prerequisite, so precisely the failure the docs anticipate — Python's
    # default exit code for an uncaught ImportError is 1: the code this tool
    # reserves for "the firmware is broken". A missing host dependency is a rig
    # error, so it must exit 2, and it must say how to fix itself. This cannot
    # be handled by the wrapper around main() below, because it fires during
    # import, before any of that code exists.
    print(f"RIG ERROR: cannot import the hardware layer: {_import_err}",
          file=sys.stderr)
    if "serial" in str(_import_err):
        print("  pyserial is not installed. Fix: python3 -m pip install pyserial",
              file=sys.stderr)
    raise SystemExit(2) from _import_err

RUNS_DIR = Path(__file__).resolve().parent.parent / "runs"


def _args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(prog="python3 -m hil", description="HIL Phase 1 rig")
    p.add_argument("--env", default="both",
                   choices=["crowpanel", "crowpanel_obd", "both"])
    p.add_argument("--expect-knob", default="auto", choices=["yes", "no", "auto"],
                   help="auto reports [encoder] without asserting on it")
    p.add_argument("--soak", type=float, default=300.0, help="soak seconds")
    p.add_argument("--boot-window", type=float, default=15.0)
    p.add_argument("--allow-skips", action="store_true",
                   help="treat SKIPPED checks as acceptable (exit 0 instead of 3)")
    return p.parse_args(argv)


def _run_env(env: str, ports: PortSet, a: argparse.Namespace, art: dict) -> list[Verdict]:
    """One environment end to end. Returns the verdicts.

    `art` is OWNED BY THE CALLER and mutated in place, never reassigned or
    returned. This matters: when this function raises RigError partway
    through, the caller still needs the pio output and whatever UART text was
    captured before the failure. Returning the dict instead would lose
    exactly the logs you need to diagnose a flash failure — the opposite of
    this rig's purpose.

    Port discipline (see DESIGN.md's "two constraints learned the hard way"):
    `UartTap` below is opened only on `ports.uart`. `flash`, `capture_native`
    and `send_key_and_capture` below are called only with `ports.native`.
    Nothing type-enforces that split — it is deliberate discipline, not a
    guardrail — so it must never drift while editing this function.

    The artifact guarantee holds for EVERY way this block can unwind, not
    just the one explicit `raise` below. `capture_native`'s EACCES path and
    `send_key_and_capture`'s `_open()` can both raise RigError, and a
    mid-soak Ctrl-C (KeyboardInterrupt) is arguably the *most likely* way this
    ever unwinds, since the soak runs 300s by default. `native` and
    `soak_native` are therefore initialised before the `try` (so the
    `finally` can always reference them without an UnboundLocalError of its
    own), and the `art[...]` assignments live in `finally` rather than only
    after a clean fall-through.
    """
    verdicts: list[Verdict] = []

    # UART opens FIRST and stays open: if the native reopen loses the race, boot
    # evidence still lands here. The native port is not touched until AFTER
    # flash() returns, and flash() itself is never handed the UART port.
    with UartTap(ports.uart) as uart:
        native = ""
        soak_native, soak_fail = "", None
        try:
            try:
                ok, pio_out = flash(env, ports.native)
            except RigError as e:
                # flash() raised instead of returning a status: `pio` is missing,
                # or it blew the 600 s budget and was killed. Either way there is
                # no complete log — but the timeout case is precisely when the
                # partial output is the only evidence of what wedged, so record
                # it before letting the error propagate to the exit-2 handler.
                # Without this, pio.log is written EMPTY on the one run where it
                # matters most.
                art["pio"] = e.partial_output
                raise
            art["pio"] = pio_out
            if not ok:
                # Do not assert against a stale binary — that produces confident
                # nonsense. Bail out as a rig error. `art` is the caller's dict, so
                # the pio output recorded above survives this raise. (The `finally`
                # below will re-set these same two keys — harmless, and this
                # explicit pair stays because it documents the flash-failure
                # case is deliberately handled, not merely caught by luck.)
                uart.read_new()
                art["uart"] = uart.text
                art["uart_error"] = uart.error  # None when the tap is healthy
                raise RigError(f"pio upload failed for {env}; see pio.log in the run dir")
            verdicts.append(Verdict("flash", Status.PASS, env))

            native = capture_native(ports.native, a.boot_window)
            uart.read_new()

            # Console probes. Skip them outright if the boot capture is empty —
            # the port never came back, so writing to it would only raise.
            if native:
                native += send_key_and_capture(ports.native, "n", 2.0)
                if env == "crowpanel":
                    native += send_key_and_capture(ports.native, "s", 2.0)

            # Soak with an ACTIVE liveness probe. Silence cannot distinguish
            # "healthy and idle" from "wedged", so poke it and require an answer.
            elapsed = 0.0
            while elapsed < a.soak:
                step = min(30.0, a.soak - elapsed)
                elapsed += step
                if not native:
                    break
                reply = send_key_and_capture(ports.native, "n", step)
                soak_native += reply
                # Drain the UART on every soak step, not just at the ends. The
                # OS tty buffer is finite and the soak is 300 s by default;
                # without this the whole window from the boot-window read to
                # __exit__ has to fit in that buffer, and the overflow is
                # silent. The serial log IS this rig's primary deliverable —
                # truncating it on the long path while draining diligently on
                # every short one is the exact defect shape this branch keeps
                # finding.
                uart.read_new()
                if "[THEME]" not in reply:
                    soak_fail = f"no [THEME] reply at t+{int(elapsed)}s"
                    break
        finally:
            # Runs on the normal path, on RigError raised anywhere above (flash
            # failure, or an EACCES from capture_native/send_key_and_capture),
            # and on KeyboardInterrupt. __exit__ (below, once this `with` body
            # finishes) drains the port into uart.text too, but that is a LOCAL
            # attribute on `uart` — it only becomes evidence once copied into
            # the caller's `art` dict, which is what this block does regardless
            # of how the try body above exited. `native`/`soak_native` hold
            # whatever was captured up to the point of any raise, which is
            # exactly the evidence a rig error or a mid-soak Ctrl-C must not
            # discard.
            art["uart"] = uart.text
            art["uart_error"] = uart.error  # None when the tap is healthy
            art["native_boot"] = native
            art["native_soak"] = soak_native

    # UartTap.__exit__ already drained the port into .text and closed it, so
    # read .text rather than calling read_new() on a closed handle — this
    # picks up any bytes that arrived between the `finally` above and the tap
    # actually closing. Only reached on the no-exception path (an exception
    # skips straight past this to the caller), which is fine: the `finally`
    # above already guaranteed a value lands in `art` on every other path.
    art["uart"] = uart.text
    art["uart_error"] = uart.error
    uart_text = art["uart"]
    all_native = native + soak_native

    verdicts += [
        parse.check_boot_mode(uart_text),
        parse.check_no_panic(uart_text),
        parse.check_no_wdt_spam(uart_text),
        parse.check_app_wdt_clean(all_native),
        parse.check_banner_env(native, env),
        parse.check_encoder(native, a.expect_knob),
    ]
    if native:
        verdicts.append(parse.check_theme_ack(native))
        if env == "crowpanel":
            verdicts.append(parse.check_mock_alarm_ack(native))
        if env == "crowpanel_obd":
            verdicts.append(parse.check_ble_scanning(native))
    else:
        verdicts.append(Verdict("console probes", Status.SKIP,
                                "native port never re-enumerated"))

    if a.soak <= 0:
        # `--soak 0` is the obvious way to say "skip the soak" while iterating,
        # and the loop above correctly runs zero probes for it. What must not
        # happen is reporting PASS for a check that never ran — so this is an
        # honest SKIP, which makes the run exit 3. Deliberately NOT clamped up to
        # some minimum: the operator asked for no soak and should be told they
        # got no soak, not quietly given one.
        verdicts.append(Verdict("soak liveness", Status.SKIP,
                                f"soak disabled (--soak {a.soak:g})"))
    elif not native:
        verdicts.append(Verdict("soak liveness", Status.SKIP, "no native port"))
    elif soak_fail:
        verdicts.append(Verdict("soak liveness", Status.FAIL, soak_fail))
    else:
        verdicts.append(Verdict("soak liveness", Status.PASS, f"{int(a.soak)}s"))

    # Boot heap is reported for a human to read, not compared against a
    # baseline — the firmware emits no periodic heap line, so a single boot
    # reading could only ever catch static-allocation bloat, not a runtime
    # leak. Automated regression detection is deferred (see DESIGN.md).
    b = parse.parse_banner(native)
    if b:
        art["heap"] = b.heap
        art["version"] = b.version
        art["git"] = b.git
        if b.version != "local":
            print(f"  note: {env} reports ver={b.version}, not 'local' — "
                  f"is this a bench build?")

    # A dead UART tap must not leave three green checks behind. `UartTap.text`
    # retains everything captured before the tap died, and every UART check's
    # PASS condition is satisfied by that retained text — so a cable pulled 50 s
    # into a 300 s run would otherwise report "all checks green", exit 0, having
    # observed a sixth of its window. Downgrading here (rather than inside each
    # check) keeps parse.py free of any notion of tap health, which is a
    # hardware-layer fact it has no way to know.
    err = art.get("uart_error")
    if err:
        verdicts = parse.downgrade_uart_verdicts(verdicts, err)

    return verdicts


def _write_artifacts(art: dict) -> Path:
    stamp = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
    d = RUNS_DIR / f"{stamp}-{art['env']}"
    d.mkdir(parents=True, exist_ok=True)
    (d / "uart.log").write_text(art.get("uart", ""))
    (d / "native.log").write_text(
        art.get("native_boot", "") + art.get("native_soak", ""))
    (d / "pio.log").write_text(art.get("pio", ""))
    return d


def _write_run(art: dict, verdicts: list[Verdict]) -> Path:
    """Write every artifact for one env, verdict.json included.

    Called on the rig-error and Ctrl-C paths too, with whatever verdicts exist
    (usually none). README and DESIGN.md both promise every run writes
    verdict.json, and a rig error is exactly when someone wants the partial
    record — `uart_error` and the check list up to the failure are the whole
    point of keeping it.
    """
    d = _write_artifacts(art)
    (d / "verdict.json").write_text(json.dumps(
        {"env": art.get("env"),
         "heap": art.get("heap"),
         "version": art.get("version"),
         "uart_error": art.get("uart_error"),
         "checks": [{"check": v.check, "status": v.status.value,
                     "detail": v.detail} for v in verdicts]},
        indent=2))
    return d


def _print_uart_error(art: dict) -> None:
    """A SKIP whose cause is visible is actionable; a SKIP whose cause is
    invisible sends someone hunting the firmware for a cable fault. Print
    loudly whenever the UART tap died mid-run; say nothing when it's healthy.

    Printed BELOW the verdict table by `_print_run_tail`, not above it: the
    loudest warning of the run belongs next to the result it explains, not
    scrolled off the top of it.
    """
    err = art.get("uart_error")
    if not err:
        return
    env = art.get("env", "?")
    # Say what actually happened. An earlier version of this message claimed the
    # affected checks "may read as SKIP" while they in fact read as PASS off
    # retained text — a sentence that would talk a reader out of investigating.
    print(f"  ** UART TAP DIED ({env}): {err}")
    print(f"     {', '.join(parse.UART_DERIVED_CHECKS)} were downgraded "
          f"PASS -> SKIP; an observed FAIL is kept as a FAIL.")
    print("     uart.log holds only what arrived before the tap died. **")


def _print_run_tail(run_dirs: list[Path], arts: list[dict]) -> None:
    """Everything that belongs after the verdict table: artifact paths, then any
    dead-tap warnings. Shared by the success and rig-error paths so the two can
    never drift into printing different things."""
    for d in run_dirs:
        print(f"artifacts: {d}")
    for art in arts:
        _print_uart_error(art)


def _render_verdict_table(all_verdicts: dict[str, list[Verdict]]) -> tuple[int, int]:
    """Print the per-env verdict table. Returns (fail_count, skip_count).

    Tolerates an env with no verdicts (and an empty table outright) because it
    is also called on the rig-error path, where the env that failed contributed
    nothing — the completed envs' verdicts must still be shown.
    """
    fails = skips = 0
    print()
    for env, verdicts in all_verdicts.items():
        if not verdicts:
            continue
        print(f"{env}:")
        width = max(len(v.check) for v in verdicts)
        for v in verdicts:
            print(f"  {v.status.value:<5} {v.check:<{width}}  {v.detail}")
            fails += v.status is Status.FAIL
            skips += v.status is Status.SKIP
    return fails, skips


def main(argv: list[str] | None = None) -> int:
    a = _args(argv)
    envs = ["crowpanel", "crowpanel_obd"] if a.env == "both" else [a.env]

    try:
        ports = discover_ports()
    except RigError as e:
        print(f"RIG ERROR: {e}", file=sys.stderr)
        return 2
    print(f"native={ports.native}  uart={ports.uart}")

    all_verdicts: dict[str, list[Verdict]] = {}
    run_dirs: list[Path] = []
    arts: list[dict] = []
    for env in envs:
        print(f"\n=== {env} ===")
        # Owned here, mutated by _run_env, so a mid-run RigError or SIGINT
        # still leaves us holding every log captured before the failure.
        art: dict = {"env": env}
        arts.append(art)
        try:
            verdicts = _run_env(env, ports, a, art)
        except (KeyboardInterrupt, RigError) as e:
            run_dirs.append(_write_run(art, []))
            # Exit 2 outranks everything, but it must not SUPPRESS what the run
            # already learned. With --env both, a genuine FAIL in `crowpanel`
            # would otherwise become invisible the moment `crowpanel_obd` hit a
            # rig error, because we returned before rendering the table.
            _render_verdict_table(all_verdicts)
            _print_run_tail(run_dirs, arts)
            if isinstance(e, KeyboardInterrupt):
                print(f"\nINTERRUPTED — partial artifacts written for {env}",
                      file=sys.stderr)
            else:
                print(f"\nRIG ERROR: {e}", file=sys.stderr)
            return 2
        all_verdicts[env] = verdicts
        run_dirs.append(_write_run(art, verdicts))

    # Verdict table, then everything that belongs under it.
    fails, skips = _render_verdict_table(all_verdicts)
    _print_run_tail(run_dirs, arts)

    # The 0/1/3 decision itself lives in parse.py, where it is pure and tested:
    # miscounting a SKIP as a PASS would silently turn every exit 3 into an
    # exit 0, which is this rig's worst possible failure. Only the wording is
    # decided here.
    flat = [v for vs in all_verdicts.values() for v in vs]
    code = parse.exit_code(flat, a.allow_skips)
    if code == 1:
        print(f"\nFAILED — {fails} check(s) failed, {skips} skipped")
    elif code == 3:
        # Not a pass. A run that skipped checks tested less than it claims.
        print(f"\nINCOMPLETE — 0 failed but {skips} skipped "
              f"(pass --allow-skips to accept)")
    else:
        print(f"\nPASSED — all checks green ({skips} skipped, accepted)"
              if skips else "\nPASSED — all checks green")
    return code


def _cli() -> int:
    """Run main(), guaranteeing no unhandled exception can exit 1.

    Exit 1 means "the firmware is broken" and nothing else. Python's default
    exit code for an uncaught exception is also 1, so without this wrapper an
    OSError from _write_artifacts (read-only tree, full disk) or a Ctrl-C
    outside the guarded _run_env call would be reported as a firmware failure.
    Both are rig errors: exit 2.

    The traceback is NOT swallowed — an unexpected exception here is a bug in
    the rig, and diagnosing it needs more than a one-line summary.
    """
    try:
        return main()
    except KeyboardInterrupt:
        print("\nINTERRUPTED", file=sys.stderr)
        return 2
    except Exception:
        traceback.print_exc()
        print("\nRIG ERROR: unhandled exception in the rig (see traceback above)"
              " — this is a bug in tools/hil, not a firmware failure",
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(_cli())
