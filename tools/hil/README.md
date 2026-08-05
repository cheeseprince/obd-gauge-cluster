# HIL test rig — Phase 1

Flashes a real CrowPanel Advance 3.5 over USB and asserts on what it prints.
Design and rationale: [DESIGN.md](DESIGN.md).

## What it catches

- boot-loop crashes (panic/backtrace on UART0)
- the **v0.1.1** regression: `task_wdt` error spam saturating the UART
- flashing the wrong environment
- a knob that stopped answering on I²C
- an LVGL repaint or alarm-overlay hang
- a silent late hang, via an active liveness probe during the soak

## What it does NOT catch

**Anything needing a BLE peer** — including the **v0.1.0** failure (a 4 ms
connect timeout). With nothing to connect to, "connect failed because the
timeout is wrong" is indistinguishable from "connect failed because nothing is
there." That is Phase 2 (`docs/OBD-BACKLOG.md` §12, private).

If an OBD adapter **is** in range, `--expect-peer yes` narrows that gap: check 9
then requires a completed link rather than accepting a scan, so "never found the
peer" becomes a failure instead of a pass. It still asserts nothing about what
the adapter *answers* — scripted OBD responses remain Phase 2.

Passing this rig does **not** make a release safe to tag.

## Requirements

- the board attached by USB, **both** USB-C ports passed through
- the user in the `dialout` group
- PlatformIO on `PATH`, `pyserial` installed

## Usage

```bash
cd tools/hil
python3 -m hil                          # both environments, 300 s soak each
python3 -m hil --env crowpanel --soak 60
python3 -m hil --expect-knob yes        # assert a Modulino knob is attached
python3 -m hil --expect-peer yes        # a BLE OBD peer is present: require a
                                        # completed link, so merely scanning fails
python3 -m hil --boot-window 25         # longer post-flash capture (default 15 s)
python3 -m hil --allow-skips            # accept SKIPped checks (exit 0, not 3)
```

`--soak 0` skips the soak; the soak check then reports **SKIP**, not PASS, so the
run exits 3. A check that never ran is never green here.

Ports are found by VID:PID (`303a:1001` native, `1a86:7522` UART bridge).
Override with `HIL_PORT_NATIVE` / `HIL_PORT_UART`.

## Exit codes

| Code | Meaning |
| :--- | :--- |
| 0 | every check ran and passed |
| 1 | a check failed — firmware problem |
| 2 | rig error — no board, port busy, permission denied, flash failed |
| 3 | passed, but checks were skipped (`--allow-skips` to accept) |

Code 3 is not a pass. A run that skipped checks tested less than it claims.

## Artifacts

Every run writes `runs/<timestamp>-<env>/{uart.log,native.log,pio.log,verdict.json}`
(gitignored). Keep them: on a field-only failure, get the serial log **before**
shipping a fix.

## Tests

```bash
cd tools/hil && python3 -m pytest tests -q
```

Pure logic only — no board needed, which is why CI runs it.
