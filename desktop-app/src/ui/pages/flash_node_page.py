from pathlib import Path

from PySide6.QtCore import Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from src.gateway.models import KnownDevice
from src.gateway.node_flasher import list_pio_environments

UNPROVISIONED_COLOR = QColor("#ffcc00")

PORT_COLUMNS = ["Port", "Description", "MAC", "Status"]

# desktop-app/src/ui/pages/flash_node_page.py -> repo root -> node-firmware
_DEFAULT_USB_PROJECT_DIR = str(Path(__file__).resolve().parents[4] / "node-firmware")
# -> RTSNow-Gateway -> RTSNow -> RTSNow-SPI-CCP (a sibling repo, not inside
# RTSNow-Gateway itself - the nodes most likely to need mesh flashing -
# ATOM POE, a persistently-weak-link dryer node - live there)
_DEFAULT_MESH_PROJECT_DIR = str(Path(__file__).resolve().parents[5] / "RTSNow-SPI-CCP")


def _flashable_environments(project_dir: str) -> list[str]:
    """list_pio_environments() returns every [env:...] section verbatim,
    including each board's "_ota" twin (same source/board, just
    upload_protocol=espota instead of a serial upload - see that env's own
    platformio.ini comment). Neither tab here has any use for those:
    NodeFlasherClient (USB tab) passes --upload-port <serial-path>, which
    would make PlatformIO try to hand that to espota (expects a hostname/
    IP) and fail outright; PioBuildClient (Mesh tab) only ever builds
    (never uploads), and upload_protocol/upload_flags don't affect
    compilation at all, so an "_ota" env there would just build the exact
    same firmware.bin as its plain counterpart - listing both is pure
    noise. Filtered out here rather than fixed in list_pio_environments()
    itself, since a project with no non-"_ota" environments at all should
    still show them rather than silently offering nothing."""
    envs = list_pio_environments(project_dir)
    non_ota = [env for env in envs if not env.endswith("_ota")]
    return non_ota or envs


class FlashNodePage(QWidget):
    """Two transports for the same underlying job (getting RTS-NOW node
    firmware onto a physical board), merged into one page/nav item since
    they're both reached from the same "flash a node" intent - just
    picking a tab for whichever transport actually applies to the board
    at hand: USB (the board is on a serial port right now, fast, needs
    physical access) or ESP-NOW Mesh (it isn't - no IP at all, e.g. ATOM
    POE, or an unreliable WiFi-OTA link, e.g. a node with a persistently
    weak connection - and no easy physical USB access either). Each tab
    keeps its own Firmware Project/environment picker rather than sharing
    one, since they default to different project directories (node-
    firmware for USB vs. the sibling RTSNow-SPI-CCP repo for mesh) and a
    user picking one transport has no reason to expect it to silently
    change the other's in-progress selection.
    """

    scan_requested = Signal()
    flash_usb_requested = Signal(str, str, str)  # port, project_dir, environment
    flash_mesh_requested = Signal(str, str, str)  # mac, project_dir, environment
    abort_mesh_requested = Signal()

    def __init__(self):
        super().__init__()
        self._usb_project_dir = _DEFAULT_USB_PROJECT_DIR
        self._mesh_project_dir = _DEFAULT_MESH_PROJECT_DIR
        self._devices: dict[str, KnownDevice] = {}
        self._build_ui()
        self._refresh_usb_environments()
        self._refresh_mesh_environments()

    def _build_ui(self):
        layout = QVBoxLayout(self)
        tabs = QTabWidget()
        tabs.addTab(self._build_usb_tab(), "USB")
        tabs.addTab(self._build_mesh_tab(), "ESP-NOW Mesh")
        layout.addWidget(tabs)

    def _build_usb_tab(self) -> QWidget:
        tab = QWidget()
        layout = QVBoxLayout(tab)

        project_box = QGroupBox("Firmware Project")
        project_row = QHBoxLayout(project_box)
        project_row.addWidget(QLabel("Project:"))
        self.usb_project_edit = QLineEdit(self._usb_project_dir)
        self.usb_project_edit.setReadOnly(True)
        project_row.addWidget(self.usb_project_edit, stretch=1)
        change_project_btn = QPushButton("Change...")
        change_project_btn.clicked.connect(self._on_change_usb_project_clicked)
        project_row.addWidget(change_project_btn)
        project_row.addWidget(QLabel("Environment:"))
        self.usb_environment_combo = QComboBox()
        project_row.addWidget(self.usb_environment_combo)
        layout.addWidget(project_box)

        ports_box = QGroupBox("Connected Serial Ports")
        ports_layout = QVBoxLayout(ports_box)
        self.ports_table = QTableWidget(0, len(PORT_COLUMNS))
        self.ports_table.setHorizontalHeaderLabels(PORT_COLUMNS)
        self.ports_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.ports_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.ports_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.ports_table.itemSelectionChanged.connect(self._on_usb_selection_changed)
        ports_layout.addWidget(self.ports_table)

        buttons_row = QHBoxLayout()
        self.scan_btn = QPushButton("Scan for Nodes")
        self.scan_btn.clicked.connect(self.scan_requested.emit)
        self.usb_flash_btn = QPushButton("Flash RTS-NOW Node Firmware")
        self.usb_flash_btn.clicked.connect(self._on_usb_flash_clicked)
        buttons_row.addWidget(self.scan_btn)
        buttons_row.addWidget(self.usb_flash_btn)
        buttons_row.addStretch(1)
        ports_layout.addLayout(buttons_row)
        hint = QLabel(
            "The gateway's own port is never scanned/flashed. Scanning "
            "briefly resets each other connected board to read its chip "
            "MAC (harmless, but expect a short reboot on each)."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color: gray;")
        ports_layout.addWidget(hint)
        layout.addWidget(ports_box)

        progress_box = QGroupBox("Flash Progress")
        progress_layout = QVBoxLayout(progress_box)
        self.usb_progress = QProgressBar()
        self.usb_progress.setRange(0, 100)
        progress_layout.addWidget(self.usb_progress)
        self.usb_log = QPlainTextEdit()
        self.usb_log.setReadOnly(True)
        self.usb_log.setStyleSheet("font-family: monospace;")
        self.usb_log.setMaximumBlockCount(2000)
        progress_layout.addWidget(self.usb_log)
        layout.addWidget(progress_box, stretch=1)

        return tab

    def _build_mesh_tab(self) -> QWidget:
        tab = QWidget()
        layout = QVBoxLayout(tab)

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
        self.mesh_project_edit = QLineEdit(self._mesh_project_dir)
        self.mesh_project_edit.setReadOnly(True)
        project_row.addWidget(self.mesh_project_edit, stretch=1)
        change_project_btn = QPushButton("Change...")
        change_project_btn.clicked.connect(self._on_change_mesh_project_clicked)
        project_row.addWidget(change_project_btn)
        project_row.addWidget(QLabel("Environment:"))
        self.mesh_environment_combo = QComboBox()
        project_row.addWidget(self.mesh_environment_combo)
        layout.addWidget(project_box)

        target_box = QGroupBox("Target Node")
        target_row = QHBoxLayout(target_box)
        target_row.addWidget(QLabel("Device:"))
        self.device_combo = QComboBox()
        target_row.addWidget(self.device_combo, stretch=1)
        self.mesh_flash_btn = QPushButton("Build and Flash via Mesh")
        self.mesh_flash_btn.clicked.connect(self._on_mesh_flash_clicked)
        target_row.addWidget(self.mesh_flash_btn)
        self.abort_btn = QPushButton("Abort")
        self.abort_btn.setEnabled(False)
        self.abort_btn.clicked.connect(self.abort_mesh_requested.emit)
        target_row.addWidget(self.abort_btn)
        layout.addWidget(target_box)

        progress_box = QGroupBox("Transfer Progress")
        progress_layout = QVBoxLayout(progress_box)
        self.mesh_progress = QProgressBar()
        self.mesh_progress.setRange(0, 100)
        progress_layout.addWidget(self.mesh_progress)
        self.mesh_log = QPlainTextEdit()
        self.mesh_log.setReadOnly(True)
        self.mesh_log.setStyleSheet("font-family: monospace;")
        self.mesh_log.setMaximumBlockCount(2000)
        progress_layout.addWidget(self.mesh_log)
        layout.addWidget(progress_box, stretch=1)

        return tab

    # ---- USB tab ----

    def set_enabled(self, enabled: bool):
        self.scan_btn.setEnabled(enabled)
        if not enabled:
            self.usb_flash_btn.setEnabled(False)
            self.ports_table.setRowCount(0)

    def _refresh_usb_environments(self):
        self.usb_environment_combo.clear()
        self.usb_environment_combo.addItems(_flashable_environments(self._usb_project_dir))

    def _on_change_usb_project_clicked(self):
        directory = QFileDialog.getExistingDirectory(self, "Select firmware project directory", self._usb_project_dir)
        if not directory:
            return
        self._usb_project_dir = directory
        self.usb_project_edit.setText(directory)
        self._refresh_usb_environments()

    def set_scanning(self, scanning: bool):
        self.scan_btn.setEnabled(not scanning)
        self.scan_btn.setText("Scanning..." if scanning else "Scan for Nodes")

    def update_rows(self, rows: list[dict]):
        """Each row: {"port", "description", "mac" (str or None), "status"
        ("known"/"unprovisioned"/"unreadable")}. Only actual flash
        candidates are shown - a "known" device is already provisioned
        (nothing to do), and "unreadable" usually just means some unrelated
        serial device (Bluetooth, another USB gadget) that isn't an
        ESP32/S3 esptool could even talk to, not a real candidate node."""
        rows = [row for row in rows if row["status"] == "unprovisioned" and row["mac"]]
        self.ports_table.setRowCount(len(rows))
        for row_idx, row in enumerate(rows):
            values = [row["port"], row["description"], row["mac"], row["status"]]
            for col, value in enumerate(values):
                item = QTableWidgetItem(value)
                item.setForeground(UNPROVISIONED_COLOR)
                self.ports_table.setItem(row_idx, col, item)
        self.usb_flash_btn.setEnabled(False)

    def _on_usb_selection_changed(self):
        # currentRow() alone isn't enough - Qt keeps a "current" index even
        # after clearSelection(), so this checks actual selection state.
        self.usb_flash_btn.setEnabled(bool(self.ports_table.selectedItems()))

    def _on_usb_flash_clicked(self):
        row = self.ports_table.currentRow()
        if row < 0:
            QMessageBox.information(self, "No selection", "Select a port to flash first.")
            return
        port = self.ports_table.item(row, 0).text()
        environment = self.usb_environment_combo.currentText() or None

        if QMessageBox.question(
            self, "Flash RTS-NOW node firmware",
            f"Flash {self._usb_project_dir}"
            + (f" (env: {environment})" if environment else "")
            + f"\nto {port}?\n\nThis erases whatever is currently on that device.",
        ) != QMessageBox.StandardButton.Yes:
            return

        self.usb_progress.setValue(0)
        self.usb_log.clear()
        self.flash_usb_requested.emit(port, self._usb_project_dir, environment or "")

    def append_usb_log(self, line: str):
        self.usb_log.appendPlainText(line)

    def set_usb_flash_progress(self, percent: int):
        self.usb_progress.setValue(percent)

    def set_usb_flash_result(self, success: bool, message: str):
        self.append_usb_log(("SUCCESS: " if success else "FAILED: ") + message)

    # ---- ESP-NOW Mesh tab ----

    def _refresh_mesh_environments(self):
        self.mesh_environment_combo.clear()
        self.mesh_environment_combo.addItems(_flashable_environments(self._mesh_project_dir))

    def _on_change_mesh_project_clicked(self):
        directory = QFileDialog.getExistingDirectory(self, "Select firmware project directory", self._mesh_project_dir)
        if not directory:
            return
        self._mesh_project_dir = directory
        self.mesh_project_edit.setText(directory)
        self._refresh_mesh_environments()

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

    def _on_mesh_flash_clicked(self):
        mac = self.device_combo.currentData()
        if not mac:
            QMessageBox.information(self, "No device", "Select a target device first.")
            return
        environment = self.mesh_environment_combo.currentText()
        if not environment:
            QMessageBox.information(self, "No environment", "Select a PlatformIO environment to build first.")
            return
        dev = self._devices.get(mac)
        name = dev.friendly_name if dev else mac

        if QMessageBox.question(
            self, "Flash via ESP-NOW mesh",
            f"Build {self._mesh_project_dir} (env: {environment}) and flash it to \"{name}\" ({mac}) entirely "
            "over the ESP-NOW mesh?\n\nThis can take several minutes and will reboot the device once "
            "complete.",
        ) != QMessageBox.StandardButton.Yes:
            return

        self.mesh_progress.setValue(0)
        self.mesh_log.clear()
        self.set_mesh_busy(True)
        self.flash_mesh_requested.emit(mac, self._mesh_project_dir, environment)

    def set_mesh_busy(self, busy: bool):
        self.mesh_flash_btn.setEnabled(not busy)
        self.abort_btn.setEnabled(busy)

    def append_mesh_log(self, line: str):
        self.mesh_log.appendPlainText(line)

    def set_mesh_progress(self, current: int, total: int):
        percent = int(current * 100 / total) if total else 0
        self.mesh_progress.setValue(percent)

    def set_mesh_result(self, success: bool, message: str):
        self.append_mesh_log(("SUCCESS: " if success else "FAILED: ") + message)
        self.set_mesh_busy(False)
