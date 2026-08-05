#!/usr/bin/env python3
"""
ble_elm.py — a fake BLE ELM327 adapter for the HIL rig.

    elm_server.py  --TCP-->  this shim  --BLE GATT-->  the dash (unmodified)
    (protocol)               (transport only)

WHY THIS EXISTS
The dash is BLE-only: the ESP32-S3 has no Bluetooth Classic radio, so every OBD
adapter it will ever meet is a BLE peripheral. Existing ELM327 emulators speak
pty, TCP or classic RFCOMM -- none of them BLE GATT -- so the transport is the
part that had to be written. This shim is transport ONLY; the protocol lives in
elm_server.py beside it.

Point it at anything that speaks ELM327 over TCP. elm_server.py is the default
because it ships here and is MIT. Ircama's ELM327-emulator is more capable and
works fine with --port, but it is CC-BY-NC-SA-4.0 (NON-COMMERCIAL), which is why
it is not a dependency of this repository.

THE GATT PROFILE IT PRESENTS
Captured from the real vLinker MS on the truck (src/ble_obd_source.h:7). The dash
scans, connects, then keeps whichever peer exposes one of three known profiles
(ble_obd_source.cpp:410-412); this shim can present any of them via --profile:

    vlinker  service 0x18f0  notify 0x2af0  write 0x2af1   (default; the real truck adapter)
    fff0     service 0xfff0  notify 0xfff1  write 0xfff2   (common clone)
    ffe0     service 0xffe0  notify 0xffe1  write 0xffe1   (clone; ONE char does both)

Presenting each in turn is most of what backlog item UX-3 wanted -- those two clone
paths were coded from spec and have never been exercised against anything.

MTU / CHUNKING
A BLE notification carries at most MTU-3 bytes and the default MTU is 23, so a long
ELM reply (a multi-frame Mode-09 VIN response is ~60 bytes) MUST be split across
several notifications. A real adapter does this too, so the dash already handles it
-- but if this shim did not chunk, replies would be silently truncated and the
failure would look like a firmware parsing bug. Chunk size follows the negotiated
MTU when BlueZ reports one, and falls back to 20.

RUNNING IT
    python3 elm_server.py --scenario gm_sierra &   # protocol, on TCP 35000
    python3 ble_elm.py                             # transport, vlinker profile
    python3 ble_elm.py --profile ffe0              # exercise a clone GATT path

No root required: an unprivileged process may both RegisterApplication and
RegisterAdvertisement against BlueZ 5.72 under the stock D-Bus policy.
"""

import argparse
import datetime
import socket
import sys
import threading
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib


def log(msg):
    """Every line timestamped. The first Phase-2 run could not be diagnosed
    because the shim log had no clock to correlate against the board's."""
    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"{ts} {msg}", flush=True)


BLUEZ = "org.bluez"
ADAPTER_PATH = "/org/bluez/hci0"
GATT_MANAGER = "org.bluez.GattManager1"
LE_AD_MANAGER = "org.bluez.LEAdvertisingManager1"
GATT_SERVICE = "org.bluez.GattService1"
GATT_CHRC = "org.bluez.GattCharacteristic1"
LE_ADVERT = "org.bluez.LEAdvertisement1"
DBUS_PROPS = "org.freedesktop.DBus.Properties"
DBUS_OM = "org.freedesktop.DBus.ObjectManager"


def uuid128(short):
    """0x18f0 -> the full 128-bit form BlueZ expects."""
    return f"0000{short:04x}-0000-1000-8000-00805f9b34fb"


# The three profiles the firmware will accept. `same` marks the ffe0 clone, where
# one characteristic is BOTH the notify and the write end -- a real quirk of those
# adapters and a case the firmware's bindChars() has to handle.
PROFILES = {
    "vlinker": dict(svc=0x18F0, notify=0x2AF0, write=0x2AF1, same=False),
    "fff0":    dict(svc=0xFFF0, notify=0xFFF1, write=0xFFF2, same=False),
    "ffe0":    dict(svc=0xFFE0, notify=0xFFE1, write=0xFFE1, same=True),
}


class ElmLink:
    """TCP client to the Ircama emulator, with a reconnect loop.

    Ircama's TCP server has a listen backlog of 1 and accepts exactly ONE client,
    so this must be the only thing connecting to it. A stray readiness probe eats
    the slot and the real client then sees nothing but timeouts -- sleep and retry,
    never poll with a throwaway connection.
    """

    def __init__(self, host, port, on_data):
        self.host, self.port, self.on_data = host, port, on_data
        self.sock = None
        self.lock = threading.Lock()
        self.stop = False

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stop:
            try:
                s = socket.create_connection((self.host, self.port), timeout=5)
                s.settimeout(None)
                with self.lock:
                    self.sock = s
                log(f"[link] connected to emulator {self.host}:{self.port}")
                while not self.stop:
                    data = s.recv(4096)
                    if not data:
                        break
                    # DRAIN PROMPTLY. Do NOT sleep here: sleeping in the reader
                    # throttles recv() itself, the emulator keeps answering at
                    # full speed, its output piles up in the socket buffer, and
                    # each recv() then returns HUNDREDS of replies concatenated
                    # into one blob. The dash sees a blob where it expects one
                    # reply and desyncs completely. Latency is applied per
                    # DELIVERY instead (see Application.from_emulator).
                    self.on_data(data)
            except Exception as e:
                log(f"[link] {type(e).__name__}: {e} — retrying in 2s")
            with self.lock:
                self.sock = None
            time.sleep(2)

    def send(self, data: bytes):
        with self.lock:
            if self.sock:
                try:
                    self.sock.sendall(data)
                    return True
                except Exception as e:
                    log(f"[link] send failed: {e}")
        return False


class Characteristic(dbus.service.Object):
    def __init__(self, bus, path, uuid, flags, service_path, owner):
        self.path, self.uuid, self.flags = path, uuid, flags
        self.service_path, self.owner = service_path, owner
        self.notifying = False
        self.value = []
        super().__init__(bus, path)

    def props(self):
        return {
            "UUID": self.uuid,
            "Service": dbus.ObjectPath(self.service_path),
            "Flags": dbus.Array(self.flags, signature="s"),
            "Notifying": dbus.Boolean(self.notifying),
        }

    @dbus.service.method(DBUS_PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props()

    @dbus.service.method(GATT_CHRC, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(self.value, signature="y")

    @dbus.service.method(GATT_CHRC, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        # The dash just wrote an ELM command. Forward it verbatim; do not
        # normalise. The firmware emits "AT SH 7E0\r" WITH spaces, and Ircama
        # tolerates that (its regex strips whitespace first) -- rewriting here
        # would test a command the firmware never actually sends.
        data = bytes(bytearray(value))
        mtu = options.get("mtu")
        if mtu:
            self.owner.note_mtu(int(mtu))
        try:
            printable = data.decode("ascii", "replace").replace("\r", "\\r")
        except Exception:
            printable = repr(data)
        log(f"[dash->emu] {printable}")
        self.owner.to_emulator(data)

    @dbus.service.method(GATT_CHRC)
    def StartNotify(self):
        self.notifying = True
        log("[gatt] dash subscribed to notifications")

    @dbus.service.method(GATT_CHRC)
    def StopNotify(self):
        self.notifying = False
        log("[gatt] dash unsubscribed")

    @dbus.service.signal(DBUS_PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    def notify(self, chunk: bytes):
        if not self.notifying:
            return False
        self.PropertiesChanged(
            GATT_CHRC, {"Value": dbus.Array(chunk, signature="y")}, []
        )
        return True


class Service(dbus.service.Object):
    def __init__(self, bus, path, uuid):
        self.path, self.uuid = path, uuid
        super().__init__(bus, path)

    def props(self):
        return {"UUID": self.uuid, "Primary": dbus.Boolean(True)}

    @dbus.service.method(DBUS_PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return self.props()


class Application(dbus.service.Object):
    def __init__(self, bus, profile, link):
        self.path = "/hilelm"
        self.link = link
        self.mtu = 0
        super().__init__(bus, self.path)

        p = PROFILES[profile]
        svc_path = f"{self.path}/service0"
        self.service = Service(bus, svc_path, uuid128(p["svc"]))

        if p["same"]:
            # ffe0 clones: a single characteristic is both endpoints.
            self.notify_chrc = Characteristic(
                bus, f"{svc_path}/char0", uuid128(p["notify"]),
                ["notify", "write", "write-without-response"], svc_path, self)
            self.write_chrc = self.notify_chrc
            self.chrcs = [self.notify_chrc]
        else:
            self.notify_chrc = Characteristic(
                bus, f"{svc_path}/char0", uuid128(p["notify"]),
                ["notify"], svc_path, self)
            self.write_chrc = Characteristic(
                bus, f"{svc_path}/char1", uuid128(p["write"]),
                ["write", "write-without-response"], svc_path, self)
            self.chrcs = [self.notify_chrc, self.write_chrc]

    def note_mtu(self, mtu):
        if mtu and mtu != self.mtu:
            self.mtu = mtu
            log(f"[gatt] negotiated MTU {mtu} -> {max(20, mtu - 3)}-byte chunks")

    def chunk_size(self):
        return max(20, self.mtu - 3) if self.mtu else 20

    def to_emulator(self, data: bytes):
        if not self.link.send(data):
            log("[warn] emulator link down; command dropped")

    def from_emulator(self, data: bytes):
        """Emulator replied. Fan out over notifications, chunked to the MTU."""
        try:
            printable = data.decode("ascii", "replace").replace("\r", "\\r")
        except Exception:
            printable = repr(data)
        log(f"[emu->dash] {printable}")
        n = self.chunk_size()
        pieces = [data[i:i + n] for i in range(0, len(data), n)]
        # Notifications must be emitted on the GLib main loop thread; this is
        # called from the TCP reader thread. The delay is scheduled here rather
        # than slept in the reader, so the socket keeps draining and replies are
        # never batched together.
        delay_ms = int(getattr(self, "reply_delay", 0.0) * 1000)
        for piece in pieces:
            if delay_ms:
                GLib.timeout_add(delay_ms, self.notify_chrc.notify, piece)
            else:
                GLib.idle_add(self.notify_chrc.notify, piece)

    @dbus.service.method(DBUS_OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        out = {dbus.ObjectPath(self.service.path): {GATT_SERVICE: self.service.props()}}
        for c in self.chrcs:
            out[dbus.ObjectPath(c.path)] = {GATT_CHRC: c.props()}
        return out


class Advertisement(dbus.service.Object):
    def __init__(self, bus, path, name, svc_uuid):
        self.path, self.name, self.svc_uuid = path, name, svc_uuid
        super().__init__(bus, path)

    @dbus.service.method(DBUS_PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return {
            "Type": "peripheral",
            "LocalName": dbus.String(self.name),
            "ServiceUUIDs": dbus.Array([self.svc_uuid], signature="s"),
            "Includes": dbus.Array([], signature="s"),
        }

    @dbus.service.method(LE_ADVERT)
    def Release(self):
        log("[adv] released by BlueZ")


def main():
    ap = argparse.ArgumentParser(description="Fake BLE ELM327 adapter for the HIL rig")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="vlinker",
                    help="which GATT profile to present (default: vlinker, the truck's)")
    ap.add_argument("--name", default="vLinker MS-B", help="advertised BLE name")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=35000, help="Ircama emulator TCP port")
    ap.add_argument("--adapter", default=ADAPTER_PATH)
    ap.add_argument("--reply-delay", type=float, default=0.035,
                    help="seconds to wait before delivering each reply (default 0.035). "
                         "A real ELM327 over BLE answers in tens of ms; this shim can "
                         "answer in 1-2ms, which is FASTER THAN REAL HARDWARE and lets "
                         "the dash's next command overlap the previous reply. Set 0 to "
                         "reproduce that overlap deliberately.")
    args = ap.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    app_holder = {}

    def on_data(data):
        app = app_holder.get("app")
        if app:
            app.from_emulator(data)

    link = ElmLink(args.host, args.port, on_data)
    app = Application(bus, args.profile, link)
    app.reply_delay = args.reply_delay
    app_holder["app"] = app

    p = PROFILES[args.profile]
    adv = Advertisement(bus, "/hilelm/adv0", args.name, uuid128(p["svc"]))

    gm = dbus.Interface(bus.get_object(BLUEZ, args.adapter), GATT_MANAGER)
    am = dbus.Interface(bus.get_object(BLUEZ, args.adapter), LE_AD_MANAGER)

    loop = GLib.MainLoop()
    state = {"gatt": None, "adv": None}

    def fail(what, err):
        log(f"[fatal] {what} failed: {err}")
        state[what] = False
        loop.quit()

    gm.RegisterApplication(
        app.path, {},
        reply_handler=lambda: (state.update(gatt=True),
                               log(f"[gatt] registered profile '{args.profile}' "
                                     f"svc={p['svc']:#06x} notify={p['notify']:#06x} "
                                     f"write={p['write']:#06x}")),
        error_handler=lambda e: fail("gatt", e))
    am.RegisterAdvertisement(
        adv.path, {},
        reply_handler=lambda: (state.update(adv=True),
                               log(f"[adv] advertising as '{args.name}'")),
        error_handler=lambda e: fail("adv", e))

    # RE-ADVERTISE WATCHDOG.
    # BlueZ stops advertising the moment a central connects, and does NOT resume
    # when that central goes away. A real ELM327 adapter advertises whenever it
    # is not connected, so without this the shim becomes invisible to any board
    # that has to SCAN -- only a cached-address direct connect still works. That
    # made reconnects look randomly broken for a whole evening, and it is also
    # why restarting the shim appeared to "break" the dash.
    # Poll ActiveInstances rather than chasing Device1 property signals: it is
    # the property that actually decides whether we are findable, and one cheap
    # D-Bus read every few seconds is simpler than getting signal bookkeeping
    # right across connect/disconnect races.
    ad_props = dbus.Interface(bus.get_object(BLUEZ, args.adapter), DBUS_PROPS)

    def ensure_advertising():
        try:
            active = int(ad_props.Get(LE_AD_MANAGER, "ActiveInstances"))
        except Exception:
            return True                      # adapter busy; try again next tick
        if active > 0:
            return True
        # Not advertising. If a central is connected that is expected and
        # correct; only re-arm once nothing is connected.
        try:
            om = dbus.Interface(bus.get_object(BLUEZ, "/"), DBUS_OM)
            for _path, ifaces in om.GetManagedObjects().items():
                dev = ifaces.get("org.bluez.Device1")
                if dev and bool(dev.get("Connected", False)):
                    return True              # connected: silence is correct
        except Exception:
            return True
        try:
            am.UnregisterAdvertisement(adv.path)
        except Exception:
            pass                             # already gone; that is the normal case
        try:
            am.RegisterAdvertisement(adv.path, {})
            log("[adv] re-armed after disconnect")
        except Exception as e:
            log(f"[adv] re-arm failed: {e}")
        return True

    GLib.timeout_add_seconds(5, ensure_advertising)

    link.start()
    log("[ready] waiting for the dash to scan and connect — Ctrl-C to stop")
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        link.stop = True
        try:
            am.UnregisterAdvertisement(adv.path)
            gm.UnregisterApplication(app.path)
            log("\n[bye] unregistered cleanly")
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
