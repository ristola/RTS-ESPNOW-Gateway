from pathlib import Path

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from src.gateway.models import KnownDevice
from src.gateway.node_flasher import list_pio_environments

# espnow_flash_page.py -> pages -> ui -> src -> desktop-app -> RTSNow-Gateway -> RTSNow -> RTSNow-SPI-CCP
# (a sibling repo, not inside RTSNow-Gateway itself - unlike FlashNodePage's
# own node-firmware default, the nodes most likely to need this feature -
# ATOM POE, a persistently-weak-link dryer node - live in RTSNow-SPI-CCP)
_DEFAULT_PROJECT_DIR = str(Path(__file__).resolve().parents[5] / "RTSNow-SPI-CCP")


class EspNowFlashPage(QWidget):
    """Flashes firmware to a node entirely over the ESP-NOW mesh - for a
    device with no usable WiFi-OTA path (no IP at all, e.g. ATOM POE) or
    one where WiFi OTA is just unreliable (e.g. a node with a
    persistently weak link), and no easy physical USB access either.
    Builds the target project/environment fresh (PioBuildClient, no
    upload/port involved), then streams the resulting firmware.bin over
    the mesh chunk by chunk (see EspNowOtaTransfer) - transparently
    routed through a relay if the gateway's own routing has decided one
    is warranted (recompute_routing() in gateway-firmware), exactly like
    any other outbound command to that device.

    This is real firmware being flashed to real equipment over a
    deliberately slow, best-effort link - expect several minutes, not
    seconds, and don't be surprised by a failed attempt on a node with an
    already-known weak connection; retrying is normal here, not a sign
    something is broken.
    """

    flash_requested = Signal(str, str, str)  # mac, project_dir, environment
    abort_requested = Signal()

    def __init__(self):
        super().__init__()
        self._project_dir = _DEFAULT_PROJECT_DIR
        self._devices: dict[str, KnownDevice] = {}
        self._build_ui()
        self._refresh_environments()

    def _build_ui(self):
        layout = QVBoxLayout(self)

        note = QLabel(
            "Flashes a node entirely over the ESP-NOW mesh - for a device with no usable WiFi-OTA path "
            "(no IP at all, or an unreliable link) and no easy physical USB access. Slow (minutes, not "
            "seconds) and transparently routed through a relay if the gateway's own routing has picked one."
        )
        note.setWordWrap(True)
        note.setStyleSheet("color: gray; font-size: 9pt;")
        layout.addWidget(note)

        project_box = QGroupBox("Firmware Project")
        project_row = QHBoxLayout(project_box)
        project_row.addWidget(QLabel("Project:"))
        self.project_edit = QLineEdit(self._project_dir)
        self.project_edit.setReadOnly(True)
        project_row.addWidget(self.project_edit, stretch=1)
        change_project_btn = QPushButton("Change...")
        change_project_btn.clicked.connect(self._on_change_project_clicked)
        project_row.addWidget(change_project_btn)
        project_row.addWidget(QLabel("Environment:"))
        self.environment_combo = QComboBox()
        project_row.addWidget(self.environment_combo)
        layout.addWidget(project_box)

        target_box = QGroupBox("Target Node")
        target_row = QHBoxLayout(target_box)
        target_row.addWidget(QLabel("Device:"))
        self.device_combo = QComboBox()
        target_row.addWidget(self.device_combo, stretch=1)
        self.flash_btn = QPushButton("Build and Flash via Mesh")
        self.flash_btn.clicked.connect(self._on_flash_clicked)
        target_row.addWidget(self.flash_btn)
        self.abort_btn = QPushButton("Abort")
        self.abort_btn.setEnabled(False)
        self.abort_btn.clicked.connect(self.abort_requested.emit)
        target_row.addWidget(self.abort_btn)
        layout.addWidget(target_box)

        progress_box = QGroupBox("Transfer Progress")
        progress_layout = QVBoxLayout(progress_box)
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        progress_layout.addWidget(self.progress_bar)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setStyleSheet("font-family: monospace;")
        self.log.setMaximumBlockCount(2000)
        progress_layout.addWidget(self.log)
        layout.addWidget(progress_box, stretch=1)

    def _refresh_environments(self):
        self.environment_combo.clear()
        self.environment_combo.addItems(list_pio_environments(self._project_dir))

    def _on_change_project_clicked(self):
        directory = QFileDialog.getExistingDirectory(self, "Select firmware project directory", self._project_dir)
        if not directory:
            return
        self._project_dir = directory
        self.project_edit.setText(directory)
        self._refresh_environments()

    def update_devices(self, devices: list[KnownDevice]):
        current_mac = self.device_combo.currentData()
        self._devices = {dev.mac: dev for dev in devices}
        self.device_combo.clear()
        for dev in sorted(devices, key=lambda d: d.friendly_name.lower()):
            self.device_combo.addItem(f"{dev.friendly_name} ({dev.mac})", dev.mac)
        if current_mac:
            idx = self.device_combo.findData(current_mac)
            if idx >= 0:
                self.device_combo.setCurrentIndex(idx)

    def _on_flash_clicked(self):
        mac = self.device_combo.currentData()
        if not mac:
            QMessageBox.information(self, "No device", "Select a target device first.")
            return
        environment = self.environment_combo.currentText()
        if not environment:
            QMessageBox.information(self, "No environment", "Select a PlatformIO environment to build first.")
            return
        dev = self._devices.get(mac)
        name = dev.friendly_name if dev else mac

        if QMessageBox.question(
            self, "Flash via ESP-NOW mesh",
            f"Build {self._project_dir} (env: {environment}) and flash it to \"{name}\" ({mac}) entirely "
            "over the ESP-NOW mesh?\n\nThis can take several minutes and will reboot the device once "
            "complete.",
        ) != QMessageBox.StandardButton.Yes:
            return

        self.progress_bar.setValue(0)
        self.log.clear()
        self.set_busy(True)
        self.flash_requested.emit(mac, self._project_dir, environment)

    def set_busy(self, busy: bool):
        self.flash_btn.setEnabled(not busy)
        self.abort_btn.setEnabled(busy)

    def append_log(self, line: str):
        self.log.appendPlainText(line)

    def set_progress(self, current: int, total: int):
        percent = int(current * 100 / total) if total else 0
        self.progress_bar.setValue(percent)

    def set_result(self, success: bool, message: str):
        self.append_log(("SUCCESS: " if success else "FAILED: ") + message)
        self.set_busy(False)
