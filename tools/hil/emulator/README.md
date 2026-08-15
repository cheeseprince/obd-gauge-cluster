# HIL Phase 2 — a fake BLE OBD adapter

Gives the rig something to connect to, so the whole `BleObdSource` path becomes
testable: scan, connect, GATT profile bind, ELM327 handshake, query, parse,
decode, and VIN-driven profile selection.

**This is the phase that would have caught v0.1.0 and v0.1.1.** Neither was
visible without a peer — with nothing to connect to, "connect failed because the
timeout is 4 ms" and "connect failed because nothing is there" look identical.

```
elm_server.py  --TCP-->  ble_elm.py  --BLE GATT-->  the dash (unmodified)
 (protocol)              (transport)
```

The two halves are split so the protocol layer is testable with no Bluetooth
hardware at all — `tests/` runs in CI on a machine with no board and no adapter.

## Hardware

| Item | Notes |
| :--- | :--- |
| CrowPanel Advance 3.5" ESP32-S3 | the board under test |
| **Two** USB-C cables | one per port — see below, this is not optional |
| USB Bluetooth adapter | any BlueZ-supported BLE dongle. Verified: TP-Link UB500 (`2357:0604`, RTL8761BU) |
| Modulino knob | optional; only needed for `--expect-knob yes` |

**Both USB-C ports must be connected.** They are not interchangeable and neither
alone is sufficient:

| Port | Carries | On reset |
| :--- | :--- | :--- |
| native USB (`303a:1001`) | the application log — `[BOOT]`, `[BLE]`, `[VIN]` | **re-enumerates**; the device node disappears and returns |
| CH340 bridge (`1a86:7522`) | ROM bootloader, IDF `ESP_LOG`, panics/backtraces | stays enumerated |

**If you run the rig in a VM, pass through the whole USB *controller*, not the
individual devices.** Per-device passthrough breaks on every flash: the ESP32-S3
re-enumerates when reset, the passthrough rule stops matching, and the port
vanishes mid-upload. It presents as random flakiness whose symptom points
nowhere near the cause.

## Setup

```bash
sudo apt install bluez            # BlueZ userspace; the kernel side is already there
sudo usermod -aG dialout $USER    # then log out and back in
pip install pyserial
```

Check the adapter came up:

```bash
bluetoothctl list                 # expect: Controller <MAC> ... [default]
```

No root is needed to run the shim: registering a GATT application and an
advertisement works unprivileged under the stock D-Bus policy.

## Running it

```bash
./hilctl.sh start      # brings up both halves in the right order
./hilctl.sh health     # is the dash actually talking?
./hilctl.sh stop
```

`hilctl.sh` exists because `python3 elm_server.py & python3 ble_elm.py &` leaves you
with two orphans, no PIDs and a held port as soon as you want to stop. It also gets
three orderings right that are easy to miss by hand:

- **waits for the emulator's port** before starting the shim, which is a TCP client of
  it and exits if it loses that race;
- **waits for `hci0` to be `UP RUNNING`**, because a cold boot enumerates the BLE
  dongle well after userspace and the shim exits advertising nothing;
- **verifies the shim survived** two seconds in. It exits almost immediately if
  something else already owns the adapter, and reporting success over a dead rig is
  the failure this whole script is meant to avoid.

It stops processes **by PID from a pidfile, never by pattern**. `pkill -f ble_elm.py`
matches the shell running it, so it kills the caller — that has happened here.

`./hilctl.sh health` is the one to run when the dash looks wrong. It checks that the
shim log is *growing* (an LE connection alone proves nothing — the dash can hold a
stale link and send nothing), that `bluetoothd` has not ballooned, and that the shim
is not spinning.

Overridable via the environment: `HIL_SCENARIO`, `HIL_PORT`, `HIL_PROFILE`,
`HIL_NAME`, `HIL_ELM_LOG`, `HIL_SHIM_LOG`.

Prefer to run them by hand? The two commands are still just:

```bash
python3 elm_server.py --scenario gm_sierra --port 35000   # protocol
python3 ble_elm.py                                        # transport
```

### Autostart (optional)

If you want the rig up after a reboot, install systemd **user** units for the two
halves and enable lingering so they start without a login:

```bash
sudo loginctl enable-linger "$USER"
systemctl --user enable --now hil-elm hil-shim
```

`hilctl.sh` detects those units and delegates `start`/`stop`/`restart` to `systemctl`,
so the two mechanisms cannot fight over the same processes. Two things learned the
hard way if you write your own units: give the shim the same `hci0` and port waits as
above, and do **not** use `BindsTo=` on an umbrella unit — it propagates a stop but
does not re-arm when the child restarts, so one supervised restart leaves the umbrella
reading `inactive` over a perfectly healthy rig.

> ⚠️ **After any shim restart the dash needs a hard reset.** It keeps the old link and
> goes quiet — LE connected, zero GATT traffic. Pulse EN:
> ```bash
> python3 -c "import serial,time; s=serial.Serial('/dev/ttyUSB0',115200); \
>             s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False); s.close()"
> ```
> Merely *opening* the port does not reset it if `stty -hupcl` was set earlier.

Then, in another shell, run the rig and assert that the link completes:

```bash
cd .. && python3 -m hil --env crowpanel_obd --expect-peer yes
```

`--expect-peer yes` turns "found the adapter" into a real assertion: scanning
without linking becomes a failure instead of a pass.

### Scenarios

| Name | What it is |
| :--- | :--- |
| `gm_sierra` | Sierra 1500 LZ0 diesel. VIN matches the LZ0 discriminator, so a correct dash auto-selects `gm_sierra_lz0` and polls the enhanced set — including `221940` on the **transmission** ECU at `7E2` |
| `generic` | Standard PIDs only, and a VIN whose WMI is unknown, so the dash must fall back to Generic |
| `ford_sd_67` | F-350 Super Duty 6.7L Power Stroke. **The inverse of the Sierra, which is why it earns a scenario:** 11-bit only, and almost everything it exposes is a standard Mode-01 PID rather than an enhanced DID. Only ATF temperature (`221E1C`) and current gear (`221E60`) are enhanced, and both answer on the transmission module at **`7E1`** — not GM's `7E2`. A dash that hard-codes one vehicle's header map reads nothing here |

Every value is a synthetic fixture. The VINs are synthetic and allowlisted in
`scripts/check_no_pii.py`; a real VIN pasted anywhere in the tree fails CI.

### GATT profiles

`ble_obd_source.cpp` accepts three profiles. The shim can present any of them
with `--profile`, which is the only way to exercise the two clone paths — they
were written from spec and have never met real hardware:

| `--profile` | Service | Notify | Write |
| :--- | :--- | :--- | :--- |
| `vlinker` (default) | `0x18f0` | `0x2af0` | `0x2af1` |
| `fff0` | `0xfff0` | `0xfff1` | `0xfff2` |
| `ffe0` | `0xffe0` | `0xffe1` | `0xffe1` (one characteristic does both) |

## Traps

**`cat /dev/ttyACM0` returns nothing.** The S3's native USB CDC only emits once
DTR is asserted, and `cat` never touches modem-control lines. Every capture comes
back empty and it looks exactly like a dead board. Use `readport.py`, which sets
DTR via pyserial. The rig itself is unaffected — it already uses pyserial.

**Run one instance of each.** A second `hil` process fails with
`RIG ERROR: pio upload failed`, which reads like a firmware fault but is two
processes fighting over the serial port. Likewise two `elm_server.py` instances
will fight over TCP 35000.

**Do not use `pkill -f ble_elm.py`.** The pattern matches the shell running the
command, so it kills itself. The `[b]le_elm` bracket trick does not help — the
literal text is still in the invoking command line.

**Restarting the shim can strand the dash.** It caches the peer address and
reconnects straight to it, but re-registering the GATT application hands out new
handles. Symptom: `bound GATT profile` then silence. Reset the board after
restarting the shim.

## Why not an off-the-shelf emulator

Ircama's **ELM327-emulator** is more capable than `elm_server.py` and was used to
prove this approach works. It is licensed **CC-BY-NC-SA-4.0** — non-commercial,
share-alike, and classified on PyPI as proprietary. This repository is MIT, which
permits commercial use, and NC restricts the *user* rather than only the
redistributor. Depending on it would quietly revoke that permission for anyone
commercial.

`elm_server.py` covers the dialogue this firmware actually performs and nothing
more. If you want full UDS depth — flow control, stateful diagnostic sessions,
DTCs — and the NC terms suit your use, run Ircama instead and point `ble_elm.py`
at it with `--port`. The shim does not care what is on the other end.

## Tests

```bash
python3 -m pytest tests -q
```

Protocol logic only; no board, no adapter, no Bluetooth. Byte-exactness is
enforced deliberately — the dash parses these strings with the same code that
talks to a real vLinker, so a reply that is nearly right just moves the bug from
the firmware into the fixture.
