import json
import time
import urllib.request

from PySide6.QtCore import QThread, Signal


class DryerStatusPoller(QThread):
    """Background poller for an RTSNow-SPI-CCP node's own /api/data HTTP
    endpoint (see that project's DryerWebServer.cpp) - same "reach the
    node directly by IP over plain LAN HTTP, no auth" pattern as
    polymerpak_status.py's poller and ota_client.py's OTA push. Runs
    continuously once started; call stop() from the UI thread when
    navigating away from this device so it doesn't keep polling a node
    that's no longer on screen."""

    status_received = Signal(dict)
    status_error = Signal(str)

    _POLL_INTERVAL_S = 3
    _TIMEOUT_S = 4

    # See polymerpak_status.py's STOP_WAIT_MS comment - same reasoning:
    # destroying a QThread while its underlying OS thread is still running
    # is undefined behavior in Qt, not just a leak, so callers must wait()
    # this long after stop() before dropping their last reference.
    STOP_WAIT_MS = (_TIMEOUT_S + 1) * 1000

    def __init__(self, ip: str):
        super().__init__()
        self._ip = ip
        self._stop = False

    def stop(self):
        self._stop = True

    def run(self):
        while not self._stop:
            try:
                with urllib.request.urlopen(f"http://{self._ip}/api/data", timeout=self._TIMEOUT_S) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                if not self._stop:
                    self.status_received.emit(data)
            except Exception as exc:  # noqa: BLE001 - any of urllib's many failure modes just means "offline right now"
                if not self._stop:
                    self.status_error.emit(str(exc))
            for _ in range(self._POLL_INTERVAL_S * 10):
                if self._stop:
                    break
                time.sleep(0.1)
