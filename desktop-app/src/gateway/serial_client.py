import json
import queue
import time
from itertools import count
from typing import Optional

import serial
from PySide6.QtCore import QThread, Signal


class GatewayClient(QThread):
    """Owns the serial connection to one RTS ESP-NOW Gateway.

    Runs its own read/write loop on a background thread (pyserial is
    blocking-only) and talks to the rest of the app purely through Qt
    signals - never touch widgets from event_received/raw_line/error
    handlers except via signal/slot connections, since they fire on this
    thread, not the GUI thread.

    Speaks the gateway's JSON-lines protocol (see ../../../SERIAL_PROTOCOL.md)
    - one JSON object per line, both directions. Plain-text lines (the
    gateway's human banner/help output) are forwarded as-is via raw_line
    but never parsed.
    """

    event_received = Signal(dict)
    raw_line = Signal(str)
    error = Signal(str)
    disconnected = Signal()

    def __init__(self, port: str, baud: int = 115200):
        super().__init__()
        self._port = port
        self._baud = baud
        self._outgoing: "queue.Queue[str]" = queue.Queue()
        self._stop_requested = False
        self._id_counter = count(1)

    def send_command(self, cmd: dict) -> int:
        """Queues a command for sending; returns the request id assigned so
        the caller can correlate the eventual response/ack."""
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
            ser = serial.Serial(self._port, self._baud, timeout=0.1)
        except serial.SerialException as exc:
            self.error.emit(f"Could not open {self._port}: {exc}")
            return

        buf = b""
        try:
            while not self._stop_requested:
                while not self._outgoing.empty():
                    line = self._outgoing.get_nowait()
                    ser.write((line + "\n").encode("utf-8"))
                    ser.flush()

                n = ser.in_waiting
                if n:
                    buf += ser.read(n)
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        self._handle_line(raw.decode(errors="replace").rstrip("\r"))
                else:
                    time.sleep(0.02)
        except serial.SerialException as exc:
            self.error.emit(f"Lost connection to {self._port}: {exc}")
        finally:
            ser.close()
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
