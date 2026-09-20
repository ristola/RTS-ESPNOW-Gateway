from pathlib import Path

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPixmap
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QLabel,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from src.gateway.models import KnownDevice

# rtsnow_page.py -> pages/ -> ui/ -> assets/RTSLOGO.png
_LOGO_PATH = Path(__file__).resolve().parents[1] / "assets" / "RTSLOGO.png"

_TILE_COLUMNS = 5

# equipmentType (from a device's own /api/data, see DeviceInfoFetcher) ->
# (2-3 letter badge, background color) for the placeholder icon drawn by
# _make_icon(). Real artwork can replace _make_icon() entirely later
# without touching anything that calls it - every caller just wants a
# QPixmap back for a given equipment type.
_ICON_STYLE = {
    "Dryer": ("DRY", QColor("#c0392b")),
    "Crystallizer": ("XTL", QColor("#2980b9")),
}
_ICON_STYLE_DEFAULT = ("?", QColor("#7f8c8d"))


def _make_icon(equipment_type: str | None, size: int = 64) -> QPixmap:
    badge, color = _ICON_STYLE.get(equipment_type, _ICON_STYLE_DEFAULT)
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setBrush(color)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.drawRoundedRect(2, 2, size - 4, size - 4, 10, 10)
    painter.setPen(QColor("white"))
    font = QFont()
    font.setBold(True)
    font.setPointSize(max(8, size // 6))
    painter.setFont(font)
    painter.drawText(pixmap.rect(), Qt.AlignmentFlag.AlignCenter, badge)
    painter.end()
    return pixmap


class _DeviceTile(QFrame):
    """One clickable icon+label card in the RTSNow page's device grid -
    the icon comes from _make_icon() (equipment-type badge until real
    artwork exists), the label is the device's friendly name, same
    identity a sidebar child or the Dashboard's device table already show
    for this same mac."""

    clicked = Signal(str)  # mac

    def __init__(self, dev: KnownDevice, equipment_type: str | None):
        super().__init__()
        self._mac = dev.mac
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFrameShape(QFrame.Shape.Box)
        self.setStyleSheet(
            "_DeviceTile { border: 1px solid #555; border-radius: 6px; } "
            "_DeviceTile:hover { border: 1px solid #1b75bc; }"
        )
        self.setFixedSize(120, 110)

        layout = QVBoxLayout(self)
        layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        icon_label = QLabel()
        icon_label.setPixmap(_make_icon(equipment_type))
        icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(icon_label)

        name_label = QLabel(dev.friendly_name)
        name_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        name_label.setWordWrap(True)
        layout.addWidget(name_label)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self._mac)
        super().mousePressEvent(event)


class RtsNowPage(QWidget):
    """Branding/info page shown when the sidebar's "RTSNow" header item
    itself is clicked, plus an icon grid of every currently known RTSNow
    project device below it - clicking a tile jumps straight to that
    device's detail page (see MainWindow._show_device_page, which picks
    the per-equipment-type page - DryerDetailPage for Dryer, generic
    NodeDetailPage for everything else so far). Distinct from the
    sidebar's own per-project-device children (added dynamically by
    MainWindow), which reach the same detail pages a different way."""

    device_selected = Signal(str)  # mac

    def __init__(self):
        super().__init__()
        self._tiles: dict[str, _DeviceTile] = {}
        self._tile_keys: dict[str, tuple] = {}  # mac -> (friendly_name, equipmentType) last rendered

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)

        header = QWidget()
        header_layout = QVBoxLayout(header)
        header_layout.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter)
        header_layout.setContentsMargins(40, 40, 40, 20)
        header_layout.setSpacing(12)

        logo_label = QLabel()
        pixmap = QPixmap(str(_LOGO_PATH))
        if not pixmap.isNull():
            logo_label.setPixmap(pixmap.scaledToWidth(200, Qt.TransformationMode.SmoothTransformation))
        logo_label.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        header_layout.addWidget(logo_label)

        title = QLabel('<span style="color:#1b75bc;">RTS</span><span style="color:#ffffff;">Now</span>')
        title.setStyleSheet("font-size: 28pt; font-weight: bold;")
        title.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        header_layout.addWidget(title)

        description = QLabel(
            "A project-agnostic ESP-NOW gateway: discovers nodes from any RTS project, "
            "provisions Wi-Fi credentials over the air, and pushes generic settings and "
            "remote-control commands - all from this desktop app.\n\n"
            "Click a device below (or use the sidebar) to see its live status."
        )
        description.setWordWrap(True)
        description.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        description.setStyleSheet("color: gray;")
        description.setMaximumWidth(420)
        header_layout.addWidget(description)

        outer.addWidget(header)

        self._grid_container = QWidget()
        self._grid = QGridLayout(self._grid_container)
        self._grid.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter)

        self._empty_label = QLabel("No RTSNow devices known yet.")
        self._empty_label.setStyleSheet("color: gray;")
        self._empty_label.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        self._grid.addWidget(self._empty_label, 0, 0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self._grid_container)
        outer.addWidget(scroll, stretch=1)

    def update_devices(self, devices: list[KnownDevice], device_info: dict[str, dict]):
        """Rebuilds the grid to match `devices` (already filtered to
        project_name == "RTSNow" by the caller) - `device_info` maps mac
        -> that device's last-fetched /api/data JSON (see
        MainWindow._device_info), used only for equipmentType here to
        pick each tile's icon; missing entries (not yet fetched, or
        unreachable) just get the default "?" icon rather than blocking
        the grid on it."""
        devices_sorted = sorted(devices, key=lambda d: d.friendly_name.lower())
        desired_macs = {dev.mac for dev in devices_sorted}

        for mac in list(self._tiles):
            if mac not in desired_macs:
                tile = self._tiles.pop(mac)
                self._tile_keys.pop(mac, None)
                self._grid.removeWidget(tile)
                tile.deleteLater()

        self._empty_label.setVisible(not devices_sorted)

        # This runs on every known_devices poll (~2s) - recreating every
        # tile every time would flicker (lost hover state, brief blank
        # gap) even when nothing about a given device actually changed.
        # Only rebuild a tile whose (name, equipment type) key changed.
        for dev in devices_sorted:
            equipment_type = device_info.get(dev.mac, {}).get("equipmentType")
            key = (dev.friendly_name, equipment_type)
            if self._tile_keys.get(dev.mac) == key:
                continue
            existing = self._tiles.pop(dev.mac, None)
            if existing is not None:
                self._grid.removeWidget(existing)
                existing.deleteLater()
            tile = _DeviceTile(dev, equipment_type)
            tile.clicked.connect(self.device_selected.emit)
            self._tiles[dev.mac] = tile
            self._tile_keys[dev.mac] = key

        for i, dev in enumerate(devices_sorted):
            row, col = divmod(i, _TILE_COLUMNS)
            self._grid.addWidget(self._tiles[dev.mac], row, col)
