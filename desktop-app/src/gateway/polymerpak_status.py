import json
import time
import urllib.request

from PySide6.QtCore import QThread, Signal


class PolymerPakStatusPoller(QThread):
    """Background poller for PolymerPak's own /api/status HTTP endpoint
    (see that project's handleApiStatus() in main.cpp) - RTS-NOW's
    register-poll mechanism only carries the 4 persisted settings (see
    fillRegisterBlock()), not this live sun-position telemetry, so this
    talks to the node directly over plain HTTP instead - same "no auth,
    LAN-only trust model" as its OTA push (see ota_client.py). Runs
    continuously once started; call stop() from the UI thread when
    navigating away from this device so it doesn't keep polling a node
    that's no longer on screen."""

    status_received = Signal(dict)
    status_error = Signal(str)

    _POLL_INTERVAL_S = 3
    _TIMEOUT_S = 4

    # How long a caller's stop()+wait() should block at most - covers the
    # worst case of stop() landing right as a urlopen() call starts (that
    # blocking call can't be cancelled from outside, only waited out).
    # Public because destroying a QThread object while its underlying OS
    # thread is still running is undefined behavior in Qt, not just a
    # leak - callers must wait() this long before dropping their last
    # reference to a poller they've stop()'d.
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
                with urllib.request.urlopen(f"http://{self._ip}/api/status", timeout=self._TIMEOUT_S) as resp:
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
