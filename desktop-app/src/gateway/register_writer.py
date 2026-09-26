import urllib.error
import urllib.parse
import urllib.request

from PySide6.QtCore import QThread, Signal


class RegisterWriter(QThread):
    """One-shot POST to an RTSNow-SPI-CCP node's own /api/writeregister
    (see that project's DryerWebServer.cpp/EquipmentModel.cpp) - same
    direct-by-IP HTTP pattern as DeviceInfoFetcher/DryerStatusPoller, for
    pushing a new value to a single register instead of reading one.

    Deliberately doesn't hardcode which registers are actually writable
    on this app's side - EquipmentModel::writeRegister() only allowlists
    a few so far (40010/40011/40014, confirmed by reading that source as
    of this writing, with "more will be added once tested against real
    hardware" in its own comment) and returns a clear 400 rejection for
    anything else, which write_error surfaces as-is. Duplicating that
    allowlist here would just be a second copy that can silently drift
    out of sync the next time the firmware side changes it.
    """

    write_succeeded = Signal(int, float)  # reg, value
    write_error = Signal(int, str)  # reg, message

    _TIMEOUT_S = 4

    def __init__(self, ip: str, reg: int, value: float):
        super().__init__()
        self._ip = ip
        self._reg = reg
        self._value = value

    def run(self):
        body = urllib.parse.urlencode({"reg": self._reg, "value": self._value}).encode("utf-8")
        req = urllib.request.Request(
            f"http://{self._ip}/api/writeregister",
            data=body,
            method="POST",
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        try:
            with urllib.request.urlopen(req, timeout=self._TIMEOUT_S):
                pass
            self.write_succeeded.emit(self._reg, self._value)
        except urllib.error.HTTPError as exc:
            # handleWriteRegister() (DryerWebServer.cpp) sends a plain-text
            # 400 body explaining why (unsupported register, or the
            # tributary didn't ack the SPI-CCP SELECT) - surface that
            # verbatim rather than just "HTTP 400".
            message = exc.read().decode("utf-8", errors="replace").strip() or f"HTTP {exc.code}"
            self.write_error.emit(self._reg, message)
        except Exception as exc:  # noqa: BLE001 - any of urllib's many failure modes just means "unreachable right now"
            self.write_error.emit(self._reg, str(exc))
