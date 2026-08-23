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
                mtu=23, log=print):
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
    log(f"[tcp] listening on {host}:{port} - now run:")
    log(f"      python -m tools.obd_scan --host {host} census")
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


async def _run(args):
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
            print("[ble] no OBD-looking adapter found. Re-run with --addr, or --name to "
                  "match loosely.", file=sys.stderr)
            for _, _, negr, addr, nm, _ in rank_devices(devices)[:10]:
                print(f"      seen: {addr}  {nm or '(no name)'}  {-negr} dBm", file=sys.stderr)
            return 2
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
            print("[ble] connected, but no known ELM327 GATT profile is present. "
                  "Characteristics seen:", file=sys.stderr)
            for uuid_, props in sorted(chars.items()):
                print(f"      {uuid_}  {','.join(props)}", file=sys.stderr)
            return 3
        label, notify, write, needs_response = bound
        print(f"[ble] profile {label}  write needs response: {needs_response}", flush=True)
        await serve(client, notify, write, needs_response, args.host, args.port, mtu)
    return 0


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
    except KeyboardInterrupt:
        print("\n[bye]")
        return 0


if __name__ == "__main__":
    sys.exit(main())
