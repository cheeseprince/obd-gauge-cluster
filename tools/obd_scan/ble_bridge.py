"""Bridge a BLE ELM327 adapter to a local TCP port, so obd_scan can use it.

WHY A BRIDGE RATHER THAN A SECOND TRANSPORT
`ElmSession` speaks TCP only (socket.create_connection), and it is the piece
that has been validated across four vehicles. Teaching it a second transport
means editing that code and re-earning that confidence; a bridge leaves it
untouched and the scanner runs exactly as it always has:

    python -m tools.obd_scan.ble_bridge --name vlinker     # terminal 1
    python -m tools.obd_scan --host 127.0.0.1 census       # terminal 2

This is the same shape tools/hil already uses, inverted: there a BLE shim fronts
a TCP ELM server; here a TCP server fronts a BLE adapter.

bleak, NOT BlueZ D-Bus, on purpose: scanning happens from a Mac in the field and
bleak is the only one of the two that works on CoreBluetooth. It is an OPTIONAL
dependency -- imported lazily so this module (and its tests) import fine without
it, which is why it is absent from requirements/test.in.

The ranking and profile-binding rules below are deliberately the same ones the
firmware applies in src/ble_rank.cpp and src/ble_obd_source.cpp. Keeping them
pure, in one place, and host-tested is why that file exists; this is the Python
half of the same idea.
"""

import argparse
import asyncio
import sys

# Profile table, in the same order as PROFILES[] in src/ble_obd_source.cpp.
# (label, service, notify, write). ffe0 clones use ONE characteristic for both
# directions, which is why notify and write can be equal.
PROFILES = [
    ("vlinker 18f0", "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0x18F0),
     "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0x2AF0),
     "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0x2AF1)),
    ("clone fff0", "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0xFFF0),
     "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0xFFF1),
     "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0xFFF2)),
    ("clone ffe0", "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0xFFE0),
     "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0xFFE1),
     "0000{:04x}-0000-1000-8000-00805f9b34fb".format(0xFFE1)),
    ("nordic-uart", "6e400001-b5a3-f393-e0a9-e50e24dcca9e",
     "6e400003-b5a3-f393-e0a9-e50e24dcca9e",
     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"),
]

# Same hint list as bleNameLooksLikeObd() in src/ble_rank.cpp.
NAME_HINTS = ("obd", "vlink", "elm", "icar", "veepeak", "konnwei", "carista", "obdlink")

# BLE default ATT MTU is 23; 3 bytes are header, leaving 20 of payload. A long
# AT string would otherwise be silently truncated by the adapter.
MIN_CHUNK = 20


class BleBridgeError(Exception):
    """A bring-up failure the caller can report as a diagnosis.

    The standalone CLI turns these back into the exit codes it always used, so
    running the bridge in its own terminal is unchanged. They exist so that
    obd_scan's in-process `--ble` path can fail the same way `--host` does --
    with a sentence about the adapter -- instead of a traceback or a bare code.
    """


class NoAdapterFound(BleBridgeError):
    """Scanned, and nothing OBD-looking answered."""


class NoKnownProfile(BleBridgeError):
    """Connected, but the GATT characteristics match no ELM327 profile."""


def looks_like_obd(name):
    """True when an advertised name hints at an OBD adapter (case-insensitive)."""
    return bool(name) and any(h in name.lower() for h in NAME_HINTS)


def rank_devices(devices):
    """Order scanned devices best-first. Pure, so it is host-tested.

    `devices` is an iterable of (address, name, service_uuids, rssi).

    A service-UUID match outranks any name, and a name outranks any signal --
    the same tiering as rankKey() in src/ble_rank.cpp, and for the same reason:
    a service UUID is what the bridge must actually find after connecting,
    whereas a name is chosen by whoever built the clone.
    """
    known = {p[1].lower() for p in PROFILES}
    ranked = []
    for address, name, svc_uuids, rssi in devices:
        svc_hit = bool(known & {str(s).lower() for s in (svc_uuids or [])})
        ranked.append((0 if svc_hit else 1,
                       0 if looks_like_obd(name) else 1,
                       -(rssi if rssi is not None else -999),
                       address, name, svc_hit))
    ranked.sort()
    return ranked


def bind_profile(characteristics):
    """Pick the ELM327 profile an adapter exposes, or None.

    `characteristics` maps a lowercase characteristic UUID to its property list.
    Returns (label, notify_uuid, write_uuid, needs_response).

    Write-without-response is preferred when the adapter offers it: it is what
    ELM327 clones expect and it saves a round trip per command. The decision
    comes from the characteristic's own properties rather than a guess, because
    clones differ and guessing wrong stalls every write.
    """
    have = {k.lower(): v for k, v in characteristics.items()}
    for label, _svc, notify, write in PROFILES:
        if notify.lower() in have and write.lower() in have:
            props = have[write.lower()]
            needs_response = "write-without-response" not in props
            return label, notify, write, needs_response
    return None


def chunk(data, size):
    """Split a payload into MTU-sized writes. Never yields an empty chunk."""
    size = max(1, size)
    return [data[i:i + size] for i in range(0, len(data), size)] or [b""]


async def serve(client, notify_uuid, write_uuid, needs_response, host, port,
                mtu=23, log=print, ready=None):
    """Run the TCP server, forwarding bytes both ways over an open BLE client.

    `client` is duck-typed (anything with is_connected / write_gatt_char /
    start_notify), so the whole path is testable with a stub and the only part
    that needs real hardware is the GATT discovery in main().
    """
    size = max(MIN_CHUNK, mtu - 3)
    state = {}

    def on_notify(_handle, data):
        writer = state.get("writer")
        if writer:
            writer.write(bytes(data))

    await client.start_notify(notify_uuid, on_notify)

    async def handle(reader, writer):
        peer = writer.get_extra_info("peername")
        # ONE client at a time. The scanner is the only intended consumer, and a
        # stray second connection would interleave commands on a link that has
        # no way to tell the two apart.
        if state.get("writer"):
            log(f"[tcp] refusing second client {peer}")
            writer.close()
            return
        log(f"[tcp] client {peer}")
        state["writer"] = writer
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                if not client.is_connected:
                    # Close the TCP side rather than absorb the write. Otherwise
                    # the scanner waits out its full timeout on every command
                    # with nothing to say why.
                    log("[ble] link dropped - closing TCP so the scanner fails fast")
                    break
                for piece in chunk(data, size):
                    await client.write_gatt_char(write_uuid, piece, response=needs_response)
        except (OSError, asyncio.IncompleteReadError) as e:
            log(f"[tcp] {e}")
        finally:
            state.pop("writer", None)
            writer.close()
            log("[tcp] client gone")

    server = await asyncio.start_server(handle, host, port)
    # Read the port back rather than echoing the argument: `port=0` asks the OS
    # for a free one, which is what the in-process path uses so it can never
    # collide with the HIL emulator on 35000 or with a second scanner run.
    bound = server.sockets[0].getsockname()[1] if server.sockets else port
    log(f"[tcp] listening on {host}:{bound}")
    if ready is not None:
        ready(host, bound)               # unblocks a caller waiting to connect
    else:
        log(f"      python -m obd_scan --host {host} --port {bound} census")
    async with server:
        await server.serve_forever()


def _bleak():
    """Import bleak with an actionable message when it is missing."""
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as e:
        raise SystemExit(
            "This bridge needs the 'bleak' package, which is an optional extra:\n"
            "    pip install bleak\n"
            "Everything else in obd_scan works without it."
        ) from e
    return BleakScanner, BleakClient


async def _run(args, ready=None):
    BleakScanner, BleakClient = _bleak()

    address = args.addr
    if not address:
        print(f"[ble] scanning {args.scan_timeout:.0f}s ...", flush=True)
        found = await BleakScanner.discover(timeout=args.scan_timeout, return_adv=True)
        devices = [(d.address, (adv.local_name or d.name or ""), adv.service_uuids, adv.rssi)
                   for d, adv in found.values()]
        if args.name:
            devices = [x for x in devices if args.name.lower() in (x[1] or "").lower()]
        ranked = [r for r in rank_devices(devices) if r[0] == 0 or r[1] == 0 or args.name]
        if not ranked:
            seen = "".join(f"\n      seen: {addr}  {nm or '(no name)'}  {-negr} dBm"
                           for _, _, negr, addr, nm, _ in rank_devices(devices)[:10])
            raise NoAdapterFound(
                "no OBD-looking adapter answered the scan. Re-run with --ble-addr, or "
                "widen the name match." + seen)
        _, _, negr, address, nm, svc_hit = ranked[0]
        print(f"[ble] chose {address}  '{nm}'  {-negr} dBm  "
              f"({'advertises our service' if svc_hit else 'matched by name'})", flush=True)

    async with BleakClient(address) as client:
        mtu = getattr(client, "mtu_size", 23)
        print(f"[ble] connected, mtu={mtu}", flush=True)
        chars = {}
        for svc in client.services:
            for ch in svc.characteristics:
                chars[ch.uuid.lower()] = list(ch.properties)
        bound = bind_profile(chars)
        if not bound:
            seen = "".join(f"\n      {uuid_}  {','.join(props)}"
                           for uuid_, props in sorted(chars.items()))
            raise NoKnownProfile(
                "connected, but no known ELM327 GATT profile is present. "
                "Characteristics seen:" + seen)
        label, notify, write, needs_response = bound
        print(f"[ble] profile {label}  write needs response: {needs_response}", flush=True)
        await serve(client, notify, write, needs_response, args.host, args.port,
                    mtu, ready=ready)
    return 0


def start(name=None, addr=None, scan_timeout=10.0, host="127.0.0.1",
          connect_timeout=45.0, log=print):
    """Bring a BLE adapter up and serve it on a local TCP port, in this process.

    Returns (host, port) once the bridge is LISTENING, which is only after the
    BLE link is connected and a GATT profile is bound -- so a caller that gets a
    port back can connect immediately without racing the radio.

    Why a thread rather than a second transport in ElmSession: `ElmSession` is
    the piece validated across four vehicles, and it stays a plain TCP socket.
    The BLE specifics live here, exactly as they do for the standalone bridge,
    and the scanner talks to 127.0.0.1 either way. One code path, one place to
    fix a GATT quirk.

    The thread is a daemon and the loop is never stopped: the bridge lives for
    the length of the scan and dies with the process. That is deliberate -- a
    scan stage that finished should not tear the radio down while a later stage
    in the same run still wants it, and the OS reclaims both on exit.
    """
    import threading

    box: "dict[str, object]" = {}
    done = threading.Event()

    def _ready(h, p_):
        box["addr"] = (h, p_)
        done.set()

    def _thread():
        args = argparse.Namespace(addr=addr, name=name, scan_timeout=scan_timeout,
                                  host=host, port=0)      # 0 = OS picks a free port
        try:
            asyncio.run(_run(args, ready=_ready))
        except BaseException as e:                          # noqa: BLE001 - reported to the caller
            box.setdefault("error", e)
        finally:
            done.set()

    threading.Thread(target=_thread, daemon=True, name="ble-bridge").start()
    if not done.wait(timeout=connect_timeout):
        raise BleBridgeError(
            f"the BLE adapter did not come up within {connect_timeout:.0f}s. "
            "Is it powered and in range? A phone already connected to it holds "
            "its single client slot.")
    if "addr" not in box:
        err = box.get("error")
        raise err if isinstance(err, BaseException) else BleBridgeError("bridge exited during startup")
    log(f"[ble] bridged to {box['addr'][0]}:{box['addr'][1]}")
    return box["addr"]


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="obd_scan.ble_bridge",
        description="Bridge a BLE ELM327 to a local TCP port for obd_scan.")
    p.add_argument("--addr", help="BLE address; skips the scan")
    p.add_argument("--name", help="substring of the advertised name, e.g. vlinker")
    p.add_argument("--scan-timeout", type=float, default=10.0)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=35000)
    args = p.parse_args(argv)
    try:
        return asyncio.run(_run(args))
    except NoAdapterFound as e:
        print(f"[ble] {e}", file=sys.stderr)
        return 2
    except NoKnownProfile as e:
        print(f"[ble] {e}", file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        print("\n[bye]")
        return 0


if __name__ == "__main__":
    sys.exit(main())
