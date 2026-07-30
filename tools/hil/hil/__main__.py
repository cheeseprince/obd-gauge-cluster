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
"""
from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

from . import parse
from .parse import Status, Verdict
from .ports import PortSet
from .runner import RigError, UartTap, capture_native, discover_ports, flash, send_key_and_capture

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
            ok, pio_out = flash(env, ports.native)
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

    if not native:
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


def _print_uart_error(art: dict) -> None:
    """A SKIP whose cause is visible is actionable; a SKIP whose cause is
    invisible sends someone hunting the firmware for a cable fault. Print
    loudly whenever the UART tap died mid-run; say nothing when it's healthy.
    """
    err = art.get("uart_error")
    if err:
        print(f"  ** UART TAP DIED: {err} — checks relying on it may read as SKIP **")


def _render_verdict_table(all_verdicts: dict[str, list[Verdict]]) -> tuple[int, int]:
    """Print the per-env verdict table. Returns (fail_count, skip_count)."""
    fails = skips = 0
    print()
    for env, verdicts in all_verdicts.items():
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
    for env in envs:
        print(f"\n=== {env} ===")
        # Owned here, mutated by _run_env, so a mid-run RigError or SIGINT
        # still leaves us holding every log captured before the failure.
        art: dict = {"env": env}
        try:
            verdicts = _run_env(env, ports, a, art)
        except KeyboardInterrupt:
            d = _write_artifacts(art)
            _print_uart_error(art)
            print(f"\nINTERRUPTED — writing partial artifacts for {env}",
                  file=sys.stderr)
            print(f"artifacts: {d}", file=sys.stderr)
            return 2
        except RigError as e:
            d = _write_artifacts(art)
            _print_uart_error(art)
            print(f"RIG ERROR: {e}", file=sys.stderr)
            print(f"artifacts: {d}", file=sys.stderr)
            return 2
        _print_uart_error(art)
        all_verdicts[env] = verdicts
        d = _write_artifacts(art)
        (d / "verdict.json").write_text(json.dumps(
            {"env": env,
             "heap": art.get("heap"),
             "version": art.get("version"),
             "uart_error": art.get("uart_error"),
             "checks": [{"check": v.check, "status": v.status.value,
                         "detail": v.detail} for v in verdicts]},
            indent=2))
        run_dirs.append(d)

    # Verdict table.
    fails, skips = _render_verdict_table(all_verdicts)

    for d in run_dirs:
        print(f"artifacts: {d}")

    if fails:
        print(f"\nFAILED — {fails} check(s) failed, {skips} skipped")
        return 1
    if skips and not a.allow_skips:
        # Not a pass. A run that skipped checks tested less than it claims.
        print(f"\nINCOMPLETE — 0 failed but {skips} skipped "
              f"(pass --allow-skips to accept)")
        return 3
    print(f"\nPASSED — all checks green ({skips} skipped, accepted)"
          if skips else "\nPASSED — all checks green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
