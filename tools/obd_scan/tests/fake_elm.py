"""A scripted ELM327 over TCP, for testing without a vehicle."""
import socket
import threading


class WedgedElm:
    """A TCP peer that accepts a connection, reads whatever is sent, and
    NEVER replies -- socket up, sendall() succeeds, nothing ever comes back.

    This is the total-wedge case of a dropped WiFi ELM327 link: the kernel
    buffer accepts the write, so no OSError is ever raised, and the read
    side simply never produces bytes. Used to prove that two consecutive
    promptless reads must be attributed to the LINK, not treated as a clean
    "NO DATA" from the vehicle (CRITICAL 1). Deterministic and fast: the
    caller drives timing via ElmSession's own deadline (probe_timeout), not
    a sleep here.
    """

    def __init__(self):
        self._sock = None
        self._conn = None
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()

    def start(self) -> tuple[str, int]:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(1)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self._sock.getsockname()

    def _serve(self):
        try:
            conn, _ = self._sock.accept()
        except OSError:
            return
        with self._lock:
            if self._stop.is_set():
                conn.close()
                return
            self._conn = conn
        try:
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(256)
                except OSError:
                    return
                if not chunk:
                    return
                # Deliberately never reply -- that is the whole point of
                # this fake.
        finally:
            conn.close()

    def stop(self):
        """Idempotent; mirrors FakeElm.stop() so a wedged server thread
        parked in recv() cannot leak past a test."""
        self._stop.set()
        if self._sock:
            self._sock.close()
            self._sock = None
        with self._lock:
            conn = self._conn
        if conn:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()
        if self._thread:
            self._thread.join(timeout=1.0)


class FakeElm:
    """Serves canned replies keyed by the request text (uppercase, no spaces).

    Unknown requests answer 'NO DATA'. Every command is recorded in
    `requests_seen` so tests can assert on protocol setup order.
    """

    def __init__(self, responses: dict[str, str], prompt: str = ">",
                 drop_prompt_once: set[str] | None = None):
        self.responses = {k.upper().replace(" ", ""): v for k, v in responses.items()}
        self.prompt = prompt
        self.requests_seen: list[str] = []
        self._sock = None
        self._conn = None          # the accepted connection, once one exists
        self._thread = None
        self._stop = threading.Event()
        # Guards the handoff of _conn between _serve() (writer) and stop()
        # (reader) so neither side can act on a half-set/half-closed value.
        self._lock = threading.Lock()
        # Requests in this set get their reply sent WITHOUT the trailing
        # prompt character the first time they are seen (simulates a
        # dropped/never-arriving '>' — deadline expiry or peer hiccup on the
        # real adapter). Removed after firing once, so a retry of the same
        # request gets a normal, complete reply.
        self._drop_once = {k.upper().replace(" ", "") for k in (drop_prompt_once or set())}

    def start(self) -> tuple[str, int]:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(1)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self._sock.getsockname()

    def _serve(self):
        try:
            conn, _ = self._sock.accept()
        except OSError:
            return
        with self._lock:
            if self._stop.is_set():
                # stop() already ran (raced accept()) -- nothing to serve.
                conn.close()
                return
            self._conn = conn
        try:
            buf = ""
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(256)
                except OSError:
                    return
                if not chunk:
                    return
                buf += chunk.decode(errors="ignore")
                while "\r" in buf:
                    line, _, buf = buf.partition("\r")
                    key = line.strip().upper().replace(" ", "")
                    if not key:
                        continue
                    self.requests_seen.append(key)
                    body = self.responses.get(key, "NO DATA")
                    if key in self._drop_once:
                        self._drop_once.discard(key)
                        conn.sendall(body.encode())            # prompt withheld once
                    else:
                        conn.sendall((body + "\r" + self.prompt).encode())
        finally:
            conn.close()

    def stop(self):
        """Idempotent; safe to call even if no client ever connected.

        Closes both the listener AND the accepted connection (if any) so a
        server thread blocked in conn.recv() -- e.g. because a test failed
        before calling session.close() -- cannot leak, then joins the thread
        with a short bounded timeout.
        """
        self._stop.set()
        if self._sock:
            self._sock.close()
            self._sock = None
        with self._lock:
            conn = self._conn
        if conn:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()
        if self._thread:
            self._thread.join(timeout=1.0)
