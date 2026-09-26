import hashlib

from PySide6.QtCore import QObject, QTimer, Signal


class EspNowOtaTransfer(QObject):
    """Drives one firmware-over-ESP-NOW transfer to one node, chunk by
    chunk, entirely from Qt's own event loop - a QTimer-based
    send-and-retry controller, not a QThread. Matches this app's own
    established pattern for exactly this kind of flow (see
    MainWindow's rename-retry logic in _on_rename_requested) and
    deliberately avoids owning any serial connection of its own: every
    actual send still goes out through MainWindow's single shared
    GatewayClient (passed in as `send_command`), and every ack is fed
    into this object from MainWindow's own event dispatch - there is
    exactly one process talking to the gateway's serial port, ever, no
    matter how many of these are (theoretically) in flight.

    One chunk in flight at a time (strict stop-and-wait, matching
    RTSNOW_OtaChunk's own wire-format comment - the receiving node can
    only write flash sequentially) - this class's whole state machine is
    built around that assumption, not a general-purpose windowed
    transfer.
    """

    progress = Signal(int, int)  # chunks_sent, total_chunks
    log = Signal(str)
    finished = Signal(bool, str)  # ok, message

    # Must match RTSNOW_OtaChunk.data's size in rtsnow_protocol.h exactly -
    # a mismatch here would silently truncate every chunk on the wire.
    CHUNK_SIZE = 220
    _RETRY_INTERVAL_MS = 1500
    _MAX_RETRIES = 6

    def __init__(self, send_command, mac: str, firmware_path: str):
        super().__init__()
        self._send_command = send_command  # callable(dict, str) - MainWindow's own _send
        self._mac = mac
        self._firmware_path = firmware_path
        self._chunks: list[bytes] = []
        self._next_index = 0
        self._attempts = 0
        self._state = "idle"  # idle -> starting -> sending -> ending -> done
        self._pending_size = 0
        self._pending_total_chunks = 0
        self._pending_md5 = ""
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._on_retry_timeout)

    @property
    def mac(self) -> str:
        return self._mac

    def start(self):
        try:
            with open(self._firmware_path, "rb") as f:
                data = f.read()
        except OSError as exc:
            self.finished.emit(False, f"Could not read firmware file: {exc}")
            return
        if not data:
            self.finished.emit(False, "Firmware file is empty")
            return

        self._chunks = [data[i:i + self.CHUNK_SIZE] for i in range(0, len(data), self.CHUNK_SIZE)]
        md5 = hashlib.md5(data).hexdigest()
        self._pending_size = len(data)
        self._pending_total_chunks = len(self._chunks)
        self._pending_md5 = md5
        self._state = "starting"
        self._attempts = 0
        self.log.emit(f"Starting ESP-NOW OTA to {self._mac}: {len(data)} bytes, {len(self._chunks)} chunks")
        self._send_start()
        self._arm_retry()

    def abort(self):
        self._timer.stop()
        self._send_command({"cmd": "espnow_ota_abort", "mac": self._mac}, f"ESP-NOW OTA abort to {self._mac}")
        self._state = "done"
        self.finished.emit(False, "Aborted")

    # ---- internal send helpers ----

    def _send_start(self):
        self._send_command(
            {
                "cmd": "espnow_ota_start",
                "mac": self._mac,
                "size": self._pending_size,
                "totalChunks": self._pending_total_chunks,
                "md5": self._pending_md5,
            },
            f"ESP-NOW OTA start to {self._mac}",
        )

    def _send_chunk(self, index: int):
        self._send_command(
            {"cmd": "espnow_ota_chunk", "mac": self._mac, "index": index, "dataHex": self._chunks[index].hex()},
            f"ESP-NOW OTA chunk {index + 1}/{len(self._chunks)} to {self._mac}",
        )

    def _send_end(self):
        self.log.emit("All chunks sent - requesting verify + apply")
        self._send_command({"cmd": "espnow_ota_end", "mac": self._mac}, f"ESP-NOW OTA end to {self._mac}")

    def _arm_retry(self):
        self._attempts += 1
        if self._attempts > self._MAX_RETRIES:
            timed_out_state = self._state
            self._timer.stop()
            self._state = "done"
            self.finished.emit(False, f"No response after {self._MAX_RETRIES} attempts (was {timed_out_state})")
            return
        self._timer.start(self._RETRY_INTERVAL_MS)

    def _on_retry_timeout(self):
        # No ack arrived in time - resend whatever this transfer is
        # currently waiting on. _arm_retry (called again right after)
        # both re-arms the timer and counts this as one more attempt
        # against _MAX_RETRIES.
        if self._state == "starting":
            self._send_start()
        elif self._state == "sending":
            self._send_chunk(self._next_index)
        elif self._state == "ending":
            self._send_end()
        self._arm_retry()

    # ---- fed by MainWindow's own event dispatch ----

    def on_start_ack(self, mac: str, ok: bool, message: str):
        if mac != self._mac or self._state != "starting":
            return
        self._timer.stop()
        if not ok:
            self._state = "done"
            self.finished.emit(False, f"Node rejected OTA start: {message}")
            return
        self._state = "sending"
        self._next_index = 0
        self._attempts = 0
        self.progress.emit(0, len(self._chunks))
        self._send_chunk(0)
        self._arm_retry()

    def on_chunk_ack(self, mac: str, index: int, ok: bool):
        if mac != self._mac or self._state != "sending" or index != self._next_index:
            return
        self._timer.stop()
        if not ok:
            # The node rejected this exact chunk (e.g. it expected a
            # different sequence index) - resend it right away rather
            # than waiting out a full retry interval for nothing.
            self._attempts = 0
            self._send_chunk(self._next_index)
            self._arm_retry()
            return
        self._next_index += 1
        self.progress.emit(self._next_index, len(self._chunks))
        self._attempts = 0
        if self._next_index >= len(self._chunks):
            self._state = "ending"
            self._send_end()
        else:
            self._send_chunk(self._next_index)
        self._arm_retry()

    def on_end_ack(self, mac: str, ok: bool, message: str):
        if mac != self._mac or self._state != "ending":
            return
        self._timer.stop()
        self._state = "done"
        if ok:
            self.finished.emit(True, "Flashed successfully - node is rebooting into the new firmware")
        else:
            self.finished.emit(False, f"Verification failed: {message}")
