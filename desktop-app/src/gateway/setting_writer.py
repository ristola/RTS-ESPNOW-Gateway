import urllib.error
import urllib.parse
import urllib.request

from PySide6.QtCore import QThread, Signal


class SettingWriter(QThread):
    """One-shot POST of a single value to one of an RTSNow-SPI-CCP node's
    own simple settings endpoints - /api/equipment, /api/model, /api/baud,
    /api/writestationid (see DryerWebServer.cpp) - which all take exactly
    one 'value' form field. Same direct-by-IP HTTP pattern and QThread-
    lifecycle discipline as RegisterWriter (see that class's own
    docstring for why cleanup must happen on finished, not on
    write_succeeded/write_error), just for these device-level settings
    instead of a numbered dryer register.
    """

    write_succeeded = Signal(str, str)  # endpoint, value (as sent)
    write_error = Signal(str, str)  # endpoint, message

    _TIMEOUT_S = 4

    def __init__(self, ip: str, endpoint: str, value):
        super().__init__()
        self._ip = ip
        self._endpoint = endpoint
        self._value = value

    def run(self):
        body = urllib.parse.urlencode({"value": self._value}).encode("utf-8")
        req = urllib.request.Request(
            f"http://{self._ip}/{self._endpoint}",
            data=body,
            method="POST",
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        try:
            with urllib.request.urlopen(req, timeout=self._TIMEOUT_S):
                pass
            self.write_succeeded.emit(self._endpoint, str(self._value))
        except urllib.error.HTTPError as exc:
            message = exc.read().decode("utf-8", errors="replace").strip() or f"HTTP {exc.code}"
            self.write_error.emit(self._endpoint, message)
        except Exception as exc:  # noqa: BLE001 - any of urllib's many failure modes just means "unreachable right now"
            self.write_error.emit(self._endpoint, str(exc))
