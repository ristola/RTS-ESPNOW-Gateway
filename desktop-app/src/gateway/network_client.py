import json
import queue
import socket
import time
from itertools import count

from PySide6.QtCore import QThread, Signal


class NetworkGatewayClient(QThread):
    """Owns a TCP connection to one RTS ESP-NOW Gateway reachable over the
    network, instead of GatewayClient's USB serial port. Deliberately
    mirrors GatewayClient's exact public shape (same four signals, same
    send_command(dict) -> int) so MainWindow can hold either in
    self.client interchangeably - every other call site (the _send()
    choke point, event dispatch, etc.) doesn't need to know or care which
    concrete transport is in use.

    Speaks the identical JSON-lines protocol as the USB path (see
    SERIAL_PROTOCOL.md) - the target device just needs to be listening for
    it on a TCP port instead of a serial port. Uses raw `socket` (blocking,
    own thread) rather than QTcpSocket, structurally parallel to
    GatewayClient.run()'s own buffer-accumulate-and-split-on-"\\n" loop.

    Port defaults to 80, not a dedicated JSON-only port anymore - the
    gateway firmware now shares one TCP listener between this JSON
    protocol and its own HTML status site, telling them apart by each
    connection's first line ("GET " vs "{") - see gateway-firmware's
    kHttpPort for the full reasoning behind the merge.
    """

    event_received = Signal(dict)
    raw_line = Signal(str)
    error = Signal(str)
    disconnected = Signal()

    def __init__(self, host: str, port: int = 80, connect_timeout: float = 3.0):
        super().__init__()
        self._host = host
        self._port = port
        self._connect_timeout = connect_timeout
        self._outgoing: "queue.Queue[str]" = queue.Queue()
        self._stop_requested = False
        self._id_counter = count(1)

    def send_command(self, cmd: dict) -> int:
        request_id = next(self._id_counter)
        cmd_with_id = dict(cmd)
        cmd_with_id["id"] = request_id
        self._outgoing.put(json.dumps(cmd_with_id))
        return request_id

    def stop(self):
        self._stop_requested = True
        self.wait(2000)

    def run(self):
        try:
            sock = socket.create_connection((self._host, self._port), timeout=self._connect_timeout)
            sock.settimeout(0.1)
        except OSError as exc:
            self.error.emit(f"Could not connect to {self._host}:{self._port}: {exc}")
            self.disconnected.emit()
            return

        buf = b""
        try:
            while not self._stop_requested:
                while not self._outgoing.empty():
                    line = self._outgoing.get_nowait()
                    sock.sendall((line + "\n").encode("utf-8"))

                try:
                    chunk = sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError as exc:
                    self.error.emit(f"Lost connection to {self._host}:{self._port}: {exc}")
                    break

                if chunk == b"":
                    self.error.emit(f"Connection to {self._host}:{self._port} closed by gateway")
                    break

                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    self._handle_line(raw.decode(errors="replace").rstrip("\r"))
        finally:
            sock.close()
            self.disconnected.emit()

    def _handle_line(self, line: str):
        if not line:
            return
        self.raw_line.emit(line)
        stripped = line.lstrip()
        if not stripped.startswith("{"):
            return
        try:
            obj = json.loads(stripped)
        except json.JSONDecodeError:
            self.error.emit(f"Malformed JSON from gateway: {line!r}")
            return
        self.event_received.emit(obj)
