from typing import Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QAction, QFontDatabase
from PySide6.QtWidgets import (
    QCheckBox,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QMenu,
    QMessageBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from src.gateway.models import KnownDevice, device_type_label

DEVICE_COLUMNS = ["Project", "Type", "Firmware Version", "Name", "MAC", "IP", "WiFi", "MESH", "Last Seen"]

# A node's default heartbeat is every 10s (RTSNowNodeConfig::
# heartbeatIntervalMs) - 3x that gives real margin for a missed heartbeat
# or two before "Show Online Only" calls it offline.
ONLINE_THRESHOLD_MS = 30000

# Same rough dBm bands most phone/router UIs use for Wi-Fi signal bars -
# applied to both the WiFi and MESH columns since they're the same 2.4GHz
# radio characteristics, just two different physical links (see
# KnownDevice.wifi_rssi/espnow_rssi's own comment).
_RSSI_BAR_THRESHOLDS = (-50, -60, -70, -80)  # >= each value scores one more bar, up to 4
_RSSI_BAR_FILLED = ["▂", "▄", "▆", "█"]  # ascending-height bars, one per signal step
_RSSI_BAR_EMPTY = "▁"


def rssi_bar_count(rssi: Optional[int]) -> int:
    """0-4, how many of the 4 signal bars a reading scores - shared with
    MeshPage's connection-quality line coloring (see that module) so both
    views agree on what counts as "good"/"weak" for the exact same
    reading, not two independently-tuned thresholds."""
    if rssi is None:
        return 0
    return sum(1 for threshold in _RSSI_BAR_THRESHOLDS if rssi >= threshold)


def format_rssi(rssi: Optional[int]) -> str:
    """"<bars> <value> dBm", or a bare dash when no reading has come in yet
    (older gateway/node firmware pre-dating this field, or - for the MESH
    column specifically - no ESP-NOW frame from this device sniffed yet)."""
    if rssi is None:
        return "–"
    bars = rssi_bar_count(rssi)
    glyph = "".join(_RSSI_BAR_FILLED[i] if i < bars else _RSSI_BAR_EMPTY for i in range(4))
    return f"{glyph} {rssi} dBm"


class NodeNetworkPage(QWidget):
    """Live known-devices table, and drilling into a device's detail page
    from it. Pending Wi-Fi provisioning requests (and the provision/reject
    actions on them) are removed for now - see PROTOCOL.md's Wi-Fi
    provisioning handshake if this comes back later; SPI-IM/RTSNow nodes
    don't use it at all (they self-manage their own Wi-Fi), so it wasn't
    seeing real use here."""

    node_selected = Signal(str)  # mac - user wants to see this device's detail page
    online_only_toggled = Signal(bool)  # lets main_window also filter the RTSNow sidebar children
    forget_requested = Signal(str)  # mac - user confirmed "Forget Node" from the context menu
    set_default_gateway_requested = Signal(str)  # ip - "Set As Default Gateway" from the context menu

    def __init__(self):
        super().__init__()
        self.known_devices: dict[str, KnownDevice] = {}
        self._build_ui()

    def _build_ui(self):
        layout = QVBoxLayout(self)

        devices_box = QGroupBox("Known Devices")
        devices_layout = QVBoxLayout(devices_box)
        self.devices_table = QTableWidget(0, len(DEVICE_COLUMNS))
        self.devices_table.setHorizontalHeaderLabels(DEVICE_COLUMNS)
        # Every column sized to fit its own actual content (MAC/IP/RSSI
        # were getting clipped under a uniform Stretch, e.g. "RTSNow-..."
        # for the Type column) except Name, which absorbs whatever width
        # is left over - names vary a lot more than the fixed-shape
        # columns and benefit most from the extra room.
        header = self.devices_table.horizontalHeader()
        header.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(DEVICE_COLUMNS.index("Name"), QHeaderView.ResizeMode.Stretch)
        # Cramped by default (Qt's stock cell padding is nearly zero) -
        # matches the breathing room the gateway's own HTML status page
        # gives its tables (see gateway-firmware's write_http_style).
        self.devices_table.setStyleSheet("QTableWidget::item { padding: 4px 10px; }")
        self.devices_table.verticalHeader().setDefaultSectionSize(28)
        self.devices_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.devices_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.devices_table.itemDoubleClicked.connect(self._on_device_double_clicked)
        self.devices_table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.devices_table.customContextMenuRequested.connect(self._on_devices_table_context_menu)
        devices_layout.addWidget(self.devices_table)

        devices_footer = QHBoxLayout()
        self.online_only_check = QCheckBox("Show Online Only")
        self.online_only_check.toggled.connect(self._on_online_only_toggled)
        devices_footer.addWidget(self.online_only_check)
        devices_footer.addStretch(1)
        devices_layout.addLayout(devices_footer)
        layout.addWidget(devices_box)

    def set_enabled(self, enabled: bool):
        self.online_only_check.setEnabled(enabled)

    def is_online_only(self) -> bool:
        return self.online_only_check.isChecked()

    def is_online(self, dev: KnownDevice) -> bool:
        return dev.age_ms < ONLINE_THRESHOLD_MS

    def update_devices(self, devices_json: list[dict]):
        self.known_devices = {d["mac"]: KnownDevice.from_json(d) for d in devices_json}
        self._render_devices_table()

    def remove_device(self, mac: str):
        """Optimistically drops `mac` from the table the moment a forget is
        sent, rather than waiting out the up-to-POLL_INTERVAL_MS gap until
        the next known_devices snapshot confirms it - MainWindow follows
        this immediately with its own forced re-poll, which reconciles
        against the gateway's authoritative state (including the device
        reappearing right away if it's still heartbeating)."""
        self.known_devices.pop(mac, None)
        self._render_devices_table()

    def _render_devices_table(self):
        show_online_only = self.online_only_check.isChecked()
        rows = [
            dev for dev in self.known_devices.values()
            if not show_online_only or dev.age_ms < ONLINE_THRESHOLD_MS
        ]
        # The ▂▄▆█▁ bar glyphs format_rssi() builds only line up cleanly at
        # a fixed advance width - the same reason the gateway's own HTML
        # status page needs font-family:monospace on that cell (see
        # gateway-firmware's td.rssi rule); the platform's UI font left
        # them rendering at inconsistent widths, looking garbled/overlapping.
        rssi_font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        rssi_columns = {DEVICE_COLUMNS.index("WiFi"), DEVICE_COLUMNS.index("MESH")}
        self.devices_table.setRowCount(len(rows))
        for row, dev in enumerate(rows):
            values = [
                dev.project_name,
                device_type_label(dev.device_type_name),
                dev.firmware_version,
                dev.friendly_name,
                dev.mac,
                dev.ip or "-",
                format_rssi(dev.wifi_rssi),
                format_rssi(dev.espnow_rssi),
                f"{dev.age_ms // 1000}s ago",
            ]
            for col, value in enumerate(values):
                item = QTableWidgetItem(value)
                if col in rssi_columns:
                    item.setFont(rssi_font)
                self.devices_table.setItem(row, col, item)

    def _on_online_only_toggled(self, checked: bool):
        self._render_devices_table()
        self.online_only_toggled.emit(checked)

    def _on_device_double_clicked(self, item):
        # Column index 4, not 3 - shifted by the "Firmware Version"
        # column inserted at index 2 (see DEVICE_COLUMNS).
        mac = self.devices_table.item(item.row(), 4).text()
        self.node_selected.emit(mac)

    def _on_devices_table_context_menu(self, pos):
        row = self.devices_table.rowAt(pos.y())
        if row < 0:
            return
        self.devices_table.selectRow(row)
        mac = self.devices_table.item(row, DEVICE_COLUMNS.index("MAC")).text()
        name = self.devices_table.item(row, DEVICE_COLUMNS.index("Name")).text()
        ip = self.devices_table.item(row, DEVICE_COLUMNS.index("IP")).text()
        menu = QMenu(self)
        forget_action = QAction("Forget Node", self)
        forget_action.triggered.connect(lambda: self._on_forget_node(mac, name))
        menu.addAction(forget_action)
        # Only offered for a device with a real IP - a mesh-only node (IP
        # column showing "-") has no address MainWindow could ever dial
        # into on startup anyway.
        if ip and ip != "-":
            set_gateway_action = QAction("Set As Default Gateway", self)
            set_gateway_action.triggered.connect(lambda: self._on_set_default_gateway(ip, name))
            menu.addAction(set_gateway_action)
        menu.exec(self.devices_table.viewport().mapToGlobal(pos))

    def _on_forget_node(self, mac: str, name: str):
        if QMessageBox.question(
            self, "Forget node",
            f"Forget {name} ({mac})? It'll reappear here if it announces or "
            "heartbeats again - this only clears the gateway's known-devices entry."
        ) != QMessageBox.StandardButton.Yes:
            return
        self.forget_requested.emit(mac)

    def _on_set_default_gateway(self, ip: str, name: str):
        if QMessageBox.question(
            self, "Set as default gateway",
            f"Try connecting to {name} ({ip}) automatically every time the app starts, "
            "before falling back to a manual USB port selection?"
        ) != QMessageBox.StandardButton.Yes:
            return
        self.set_default_gateway_requested.emit(ip)
