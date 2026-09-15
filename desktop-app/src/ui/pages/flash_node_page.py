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
    QVBoxLayout,
    QWidget,
)

from src.gateway.node_flasher import list_pio_environments

UNPROVISIONED_COLOR = QColor("#ffcc00")

PORT_COLUMNS = ["Port", "Description", "MAC", "Status"]

# desktop-app/src/ui/pages/flash_node_page.py -> repo root -> node-firmware
_DEFAULT_PROJECT_DIR = str(Path(__file__).resolve().parents[4] / "node-firmware")


class FlashNodePage(QWidget):
    """USB-based node onboarding: scan connected serial ports, read each
    one's chip MAC directly via esptool (works on a totally blank chip -
    see node_flasher.py), and flag any MAC that isn't already in the
    gateway's known_devices list as unprovisioned. Flash RTS-NOW node
    firmware onto a selected port via `pio run -t upload`.
    """

    scan_requested = Signal()
    flash_requested = Signal(str, str, str)  # port, project_dir, environment

    def __init__(self):
        super().__init__()
        self._project_dir = _DEFAULT_PROJECT_DIR
        self._build_ui()
        self._refresh_environments()

    def _build_ui(self):
        layout = QVBoxLayout(self)

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

        ports_box = QGroupBox("Connected Serial Ports")
        ports_layout = QVBoxLayout(ports_box)
        self.ports_table = QTableWidget(0, len(PORT_COLUMNS))
        self.ports_table.setHorizontalHeaderLabels(PORT_COLUMNS)
        self.ports_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.ports_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.ports_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.ports_table.itemSelectionChanged.connect(self._on_selection_changed)
        ports_layout.addWidget(self.ports_table)

        buttons_row = QHBoxLayout()
        self.scan_btn = QPushButton("Scan for Nodes")
        self.scan_btn.clicked.connect(self.scan_requested.emit)
        self.flash_btn = QPushButton("Flash RTS-NOW Node Firmware")
        self.flash_btn.clicked.connect(self._on_flash_clicked)
        buttons_row.addWidget(self.scan_btn)
        buttons_row.addWidget(self.flash_btn)
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
        self.flash_progress = QProgressBar()
        self.flash_progress.setRange(0, 100)
        progress_layout.addWidget(self.flash_progress)
        self.flash_log = QPlainTextEdit()
        self.flash_log.setReadOnly(True)
        self.flash_log.setStyleSheet("font-family: monospace;")
        self.flash_log.setMaximumBlockCount(2000)
        progress_layout.addWidget(self.flash_log)
        layout.addWidget(progress_box, stretch=1)

    def set_enabled(self, enabled: bool):
        self.scan_btn.setEnabled(enabled)
        if not enabled:
            self.flash_btn.setEnabled(False)
            self.ports_table.setRowCount(0)

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
        self.flash_btn.setEnabled(False)

    def _on_selection_changed(self):
        # currentRow() alone isn't enough - Qt keeps a "current" index even
        # after clearSelection(), so this checks actual selection state.
        self.flash_btn.setEnabled(bool(self.ports_table.selectedItems()))

    def _on_flash_clicked(self):
        row = self.ports_table.currentRow()
        if row < 0:
            QMessageBox.information(self, "No selection", "Select a port to flash first.")
            return
        port = self.ports_table.item(row, 0).text()
        environment = self.environment_combo.currentText() or None

        if QMessageBox.question(
            self, "Flash RTS-NOW node firmware",
            f"Flash {self._project_dir}"
            + (f" (env: {environment})" if environment else "")
            + f"\nto {port}?\n\nThis erases whatever is currently on that device.",
        ) != QMessageBox.StandardButton.Yes:
            return

        self.flash_progress.setValue(0)
        self.flash_log.clear()
        self.flash_requested.emit(port, self._project_dir, environment or "")

    def append_log(self, line: str):
        self.flash_log.appendPlainText(line)

    def set_flash_progress(self, percent: int):
        self.flash_progress.setValue(percent)

    def set_flash_result(self, success: bool, message: str):
        self.append_log(("SUCCESS: " if success else "FAILED: ") + message)
