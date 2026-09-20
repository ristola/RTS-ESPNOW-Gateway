import json
import urllib.request

from PySide6.QtCore import QThread, Signal


class DeviceInfoFetcher(QThread):
    """One-shot fetch of an RTSNow-SPI-CCP node's equipmentType/model from
    its own /api/data HTTP endpoint - same direct-by-IP reasoning as
    dryer_status_poller.py, but a single request rather than continuous
    polling: this only feeds the RTSNow page's icon grid (which icon/label
    to show for a device) and MainWindow's device-type routing, neither of
    which needs live updates, just "what kind of thing is this.\""""

    info_received = Signal(str, dict)  # mac, {"equipmentType": ..., "model": ...}
    info_error = Signal(str, str)  # mac, message

    _TIMEOUT_S = 4

    def __init__(self, mac: str, ip: str):
        super().__init__()
        self._mac = mac
        self._ip = ip

    def run(self):
        try:
            with urllib.request.urlopen(f"http://{self._ip}/api/data", timeout=self._TIMEOUT_S) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            self.info_received.emit(self._mac, data)
        except Exception as exc:  # noqa: BLE001 - any of urllib's many failure modes just means "unreachable right now"
            self.info_error.emit(self._mac, str(exc))
