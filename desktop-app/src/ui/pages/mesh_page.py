import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import QGroupBox, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from src.gateway.models import KnownDevice
from src.ui.pages.node_network_page import ONLINE_THRESHOLD_MS, rssi_bar_count

# Bar count (see rssi_bar_count) -> line color. Same 4-strong/1-weak scale
# the Hardware List table already uses, just as a color instead of glyphs -
# a line's color is this node's ESP-NOW link quality straight TO this
# gateway (espnow_rssi, not wifi_rssi - that's this node's unrelated link
# to its own Wi-Fi router, not relevant to a mesh-topology view).
#
# 0 bars is deliberately split into two different colors, not one:
# rssi_bar_count(-88) and rssi_bar_count(None) both return 0, but those
# are very different situations - a real, terrible reading vs. no
# reading at all - and collapsing them into one gray would hide exactly
# the node (e.g. "SHED", confirmed on real hardware to have a
# consistently marginal link) this page most needs to surface. See
# _link_color() below, which branches on the raw rssi (None vs. a real
# value) before ever consulting this bars-keyed table.
_QUALITY_COLORS = {
    4: QColor("#2e7d32"),  # strong - dark green
    3: QColor("#8bc34a"),  # good - light green
    2: QColor("#ff9800"),  # fair - amber
    1: QColor("#e53935"),  # weak - red
    0: QColor("#b71c1c"),  # very weak but a real reading - dark red/maroon
}
_NO_READING_COLOR = QColor("#9e9e9e")  # rssi is None - no data, not just a bad signal
_OFFLINE_COLOR = QColor("#bdbdbd")
_ROUTED_COLOR = QColor("#1565c0")  # distinct blue - "this path is actively relayed", not just a quality reading


def _link_color(rssi: int | None) -> QColor:
    return _NO_READING_COLOR if rssi is None else _QUALITY_COLORS[rssi_bar_count(rssi)]
_GATEWAY_COLOR = QColor("#33415c")
_NODE_BORDER_COLOR = QColor("#222222")
_NODE_FILL_COLOR = QColor("#ffffff")
_TEXT_COLOR = QColor("#111111")
_BACKGROUND_COLOR = QColor("#f5f5f5")


class _MeshCanvas(QWidget):
    """Hand-painted (not QGraphicsView, matching this app's existing
    DryerDetailPage diagrams) star topology: this gateway is the hub every
    node connects directly to - see MeshPage's own docstring for why there
    are deliberately no node-to-node lines. Explicit fixed colors
    throughout, not the system palette/stylesheets - same reasoning as
    DryerDetailPage's diagram: this is a from-scratch painted canvas, so it
    needs to stay legible in both light and dark OS themes on its own."""

    def __init__(self):
        super().__init__()
        self._devices: list[KnownDevice] = []
        self.setMinimumSize(420, 420)

    def set_devices(self, devices: list[KnownDevice]):
        self._devices = devices
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), _BACKGROUND_COLOR)

        cx, cy = self.width() / 2, self.height() / 2
        node_radius = 30
        # Margin leaves room for the two label lines (name above, RSSI
        # below) drawn outside the circle itself - see the label rects
        # below, which is why this is a good deal more than just
        # node_radius plus a small pad.
        margin = node_radius + 60
        ring_radius = max(80.0, min(self.width(), self.height()) / 2 - margin)

        positions = []
        n = len(self._devices)
        for i, dev in enumerate(self._devices):
            angle = (2 * math.pi * i / n) - math.pi / 2 if n else 0
            x = cx + ring_radius * math.cos(angle)
            y = cy + ring_radius * math.sin(angle)
            positions.append((dev, QPointF(x, y)))
        pos_by_device_id = {dev.device_id: pos for dev, pos in positions}
        dev_by_device_id = {dev.device_id: dev for dev, pos in positions}

        # Node-to-node lines first (furthest back), then gateway-hub
        # lines, then circles on top of everything - so the two line
        # kinds never visually compete and circles always paint cleanly
        # over both.
        #
        # This is real, sniffed "can A currently hear B" data (see
        # rtsnow_node.cpp's neighbor-discovery section) - NOT a relay/
        # routing path. No traffic actually flows over these; see this
        # class's own docstring. Drawn thin/dotted specifically so they
        # read as a weaker diagnostic signal than the solid gateway-hub
        # lines, not as an alternate control path.
        drawn_pairs = set()
        for dev, pos in positions:
            if dev.age_ms >= ONLINE_THRESHOLD_MS:
                continue
            for neighbor in dev.neighbors:
                other_pos = pos_by_device_id.get(neighbor.device_id)
                if other_pos is None:
                    continue
                pair = frozenset((dev.device_id, neighbor.device_id))
                if pair in drawn_pairs:
                    continue
                drawn_pairs.add(pair)
                pen = QPen(_link_color(neighbor.rssi), 1, Qt.PenStyle.DotLine)
                painter.setPen(pen)
                painter.drawLine(pos, other_pos)

        # Gateway-hub lines - bent through the chosen relay (drawn in a
        # distinct blue, gateway->relay then relay->target) for a device
        # gateway-firmware's recompute_routing() has decided to route
        # outbound commands through instead of sending direct. This is
        # real, currently-active routing (unlike the dotted lines above),
        # but still only for the gateway->node direction - a relayed
        # command's own reply may not make it back the same way yet, see
        # this class's own docstring.
        for dev, pos in positions:
            online = dev.age_ms < ONLINE_THRESHOLD_MS
            relay_pos = pos_by_device_id.get(dev.route_via_device_id) if online and dev.route_via_device_id else None
            if not online:
                pen = QPen(_OFFLINE_COLOR, 2, Qt.PenStyle.DashLine)
                painter.setPen(pen)
                painter.drawLine(QPointF(cx, cy), pos)
            elif relay_pos is not None:
                pen = QPen(_ROUTED_COLOR, 3, Qt.PenStyle.SolidLine)
                painter.setPen(pen)
                painter.drawLine(QPointF(cx, cy), relay_pos)
                painter.drawLine(relay_pos, pos)
            else:
                color = _link_color(dev.espnow_rssi)
                pen = QPen(color, 3 if dev.espnow_rssi is not None else 2,
                           Qt.PenStyle.SolidLine if dev.espnow_rssi is not None else Qt.PenStyle.DashLine)
                painter.setPen(pen)
                painter.drawLine(QPointF(cx, cy), pos)

        # Gateway hub, center.
        painter.setPen(QPen(_NODE_BORDER_COLOR, 2))
        painter.setBrush(_GATEWAY_COLOR)
        gw_size = 70
        gw_rect = QRectF(cx - gw_size / 2, cy - gw_size / 2, gw_size, gw_size)
        painter.drawRoundedRect(gw_rect, 10, 10)
        painter.setPen(QColor("#ffffff"))
        painter.setFont(QFont(painter.font().family(), 9, QFont.Weight.Bold))
        painter.drawText(gw_rect, Qt.AlignmentFlag.AlignCenter, "GATEWAY")

        # Nodes, around the ring.
        for dev, pos in positions:
            online = dev.age_ms < ONLINE_THRESHOLD_MS
            painter.setPen(QPen(_NODE_BORDER_COLOR, 2))
            painter.setBrush(_NODE_FILL_COLOR if online else QColor("#e0e0e0"))
            painter.drawEllipse(pos, node_radius, node_radius)

            # Name above the circle, RSSI below - both outside the circle's
            # own outline (node_radius) rather than overlapping it.
            painter.setPen(_TEXT_COLOR if online else QColor("#777777"))
            painter.setFont(QFont(painter.font().family(), 8))
            name_rect = QRectF(pos.x() - 70, pos.y() - node_radius - 16, 140, 14)
            painter.drawText(name_rect, Qt.AlignmentFlag.AlignCenter, dev.friendly_name)

            if online:
                rssi_text = f"{dev.espnow_rssi} dBm" if dev.espnow_rssi is not None else "no reading"
                quality_color = _link_color(dev.espnow_rssi)
            else:
                rssi_text = "offline"
                quality_color = _OFFLINE_COLOR
            painter.setPen(quality_color)
            painter.setFont(QFont(painter.font().family(), 7))
            rssi_rect = QRectF(pos.x() - 70, pos.y() + node_radius + 2, 140, 14)
            painter.drawText(rssi_rect, Qt.AlignmentFlag.AlignCenter, rssi_text)

            relay = dev_by_device_id.get(dev.route_via_device_id) if online and dev.route_via_device_id else None
            if relay is not None:
                painter.setPen(_ROUTED_COLOR)
                painter.setFont(QFont(painter.font().family(), 7, QFont.Weight.Bold))
                via_rect = QRectF(pos.x() - 70, pos.y() + node_radius + 16, 140, 14)
                painter.drawText(via_rect, Qt.AlignmentFlag.AlignCenter, f"via {relay.friendly_name}")

        if n == 0:
            painter.setPen(QColor("#777777"))
            painter.setFont(QFont(painter.font().family(), 10))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "No devices known yet.")


class MeshPage(QWidget):
    """XBee-style network view of this gateway's ESP-NOW mesh - BUT see
    the gap this deliberately does NOT paper over: this is still
    single-hop relay, not a general multi-hop mesh like a real XBee
    network. Solid, quality-colored lines are a node's direct link to the
    gateway (espnow_rssi, same reading as the Hardware List table's MESH
    column). Dotted lines are real node-to-node "can A currently hear B"
    data (see rtsnow_node.cpp's neighbor-discovery section), sniffed by
    each node itself - purely diagnostic, no traffic flows over these.

    Solid BLUE lines are different: real, currently-active routing -
    gateway-firmware's recompute_routing() decided this node's direct
    link is bad enough, and some other known device's combined path to it
    good enough, that outbound commands (settings/reboot/register-request)
    now go gateway -> that relay -> this node instead of direct. This is
    outbound-only by design (see recompute_routing()'s own comment) - a
    relayed command's own reply may not make it all the way back yet, so
    a routed node's "no confirmation received" on a rename etc. doesn't
    necessarily mean the command failed."""

    def __init__(self):
        super().__init__()
        self._build_ui()

    def _build_ui(self):
        layout = QVBoxLayout(self)

        box = QGroupBox("Mesh (Gateway ↔ Nodes)")
        box_layout = QVBoxLayout(box)

        note = QLabel(
            "Quality-colored solid lines: direct ESP-NOW link to this gateway (MESH RSSI). Dotted lines: "
            "real node-to-node \"can hear\" data - diagnostic only. Solid BLUE lines: this node's outbound "
            "commands are actively routed through another node with a better path - see this page's own "
            "tooltip/docstring for why the reply may not come back the same way yet."
        )
        note.setWordWrap(True)
        note.setStyleSheet("color: gray; font-size: 9pt;")
        box_layout.addWidget(note)

        self._canvas = _MeshCanvas()
        box_layout.addWidget(self._canvas, stretch=1)

        legend = QHBoxLayout()
        for label, color in (
            ("Strong", _QUALITY_COLORS[4]),
            ("Good", _QUALITY_COLORS[3]),
            ("Fair", _QUALITY_COLORS[2]),
            ("Weak", _QUALITY_COLORS[1]),
            ("Very Weak", _QUALITY_COLORS[0]),
            ("No reading", _NO_READING_COLOR),
            ("Offline", _OFFLINE_COLOR),
            ("Routed", _ROUTED_COLOR),
        ):
            swatch = QLabel("⬤")  # filled circle glyph, colored per-entry below
            swatch.setStyleSheet(f"color: {color.name()};")
            legend.addWidget(swatch)
            legend.addWidget(QLabel(label))
            legend.addSpacing(10)
        legend.addStretch(1)
        box_layout.addLayout(legend)

        layout.addWidget(box)

    def update_devices(self, devices: list[KnownDevice]):
        self._canvas.set_devices(devices)
