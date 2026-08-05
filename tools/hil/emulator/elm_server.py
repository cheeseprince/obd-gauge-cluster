#!/usr/bin/env python3
"""TCP front end for the ELM327 responder.

    python3 elm_server.py --scenario gm_sierra --port 35000

`ble_elm.py` connects to this and bridges it onto BLE. They are split so the
protocol layer stays testable without any Bluetooth hardware — the responder
tests run in CI, on a machine with no board and no adapter.

The split also means you can point `ble_elm.py` at something else entirely
(Ircama's ELM327-emulator, a recorded log replayer, a real adapter on a serial
bridge) without touching the BLE code.
"""

import argparse
import socket
import sys
import threading

from elm_responder import ElmResponder
from scenarios import SCENARIOS


def serve_client(conn: socket.socket, responder: ElmResponder, verbose: bool) -> None:
    """One client, one responder instance.

    A fresh ElmResponder per connection is deliberate: echo/spaces/header are
    per-session state on a real adapter, and carrying them across reconnects
    would let one test contaminate the next.
    """
    buf = b""
    with conn:
        while True:
            try:
                chunk = conn.recv(1024)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            # Commands are CR-terminated. Split on CR and keep any partial tail:
            # a BLE write can be fragmented, so treating each recv() as a whole
            # command would corrupt anything split across packets.
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                cmd = line.decode("ascii", "replace").strip()
                if not cmd:
                    continue
                reply = responder.handle(cmd)
                if verbose:
                    print(f"  <- {cmd}\n  -> {reply!r}", flush=True)
                try:
                    conn.sendall(reply.encode("ascii"))
                except OSError:
                    return


def main() -> int:
    ap = argparse.ArgumentParser(description="ELM327 responder over TCP")
    ap.add_argument("--scenario", default="gm_sierra", choices=sorted(SCENARIOS),
                    help="which fake vehicle to be (default: gm_sierra)")
    ap.add_argument("--port", type=int, default=35000)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="log every command and reply")
    a = ap.parse_args()

    scenario = SCENARIOS[a.scenario]
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    # Backlog of 8, and each client gets its own thread. A single-client server
    # is a trap: any stray probe (a readiness check, a leftover connection)
    # takes the only slot and the real client then sees nothing but timeouts.
    srv.listen(8)
    print(f"[elm] scenario={a.scenario} vin={scenario.vin or '(none)'} "
          f"listening on {a.host}:{a.port}", flush=True)

    try:
        while True:
            conn, addr = srv.accept()
            print(f"[elm] client {addr[0]}:{addr[1]} connected", flush=True)
            threading.Thread(
                target=serve_client,
                args=(conn, ElmResponder(scenario), a.verbose),
                daemon=True,
            ).start()
    except KeyboardInterrupt:
        print("\n[elm] stopping", flush=True)
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
