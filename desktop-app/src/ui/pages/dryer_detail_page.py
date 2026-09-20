from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from src.gateway.dryer_status_poller import DryerStatusPoller
from src.gateway.models import KnownDevice

# reg -> tile label. Matches DryerWebServer.cpp's /api/data "registers"
# array (reg/name/value/present) - see RTSNow-SPI-CCP's DataSheets/SPI-CCP
# Notes - Dryer and Crystallizer Polls.md for which of these a given
# model actually reports (0x20/0x40/0x48 - Process Status/Machine
# Status - are on every dryer model; the rest come from each model's own
# "blanket poll", which not every model implements yet). A register
# missing from the live response (present=False, or absent entirely -
# e.g. an FD/CD dryer, which use different blanket-poll layouts SPI-CCP
# doesn't decode yet) just shows "--" rather than hiding the tile - the
# fixed layout is what makes this "one page, adapts per model" instead of
# a separate hand-built page per dryer model.
_REGISTER_TILES = {
    40010: "Process Setpoint",
    40012: "Process Temp",
    40011: "Process Limit Delta",
    40016: "Dew Point",
    40017: "Dew Point Trigger",
    40015: "Return Temp",
    40018: "Regen Temp",
    40019: "Regen Out Temp",
    40020: "Aux 1 Temp",
    40021: "Aux 2 Temp",
}

# Raw status words every dryer model reports (see DataSheets notes: "Process
# Status" / "Machine Mode" bitfields) - shown as their raw value, not
# decoded into a Running/Alarm state, since no confirmed bit-meaning
# exists yet for either. Misrepresenting an unconfirmed bitfield as
# "Running"/"Alarm" would be worse than just showing the number.
_STATUS_TILES = {
    40013: "Process Status",
    40014: "Machine Status",
}

# Not decoded from any known SPI-CCP command yet (see this project's
# Discovery-tool capture of this same FN dryer's 0xED-prefixed commands,
# still unlabeled) - shown as fixed placeholders so the layout already has
# room for them once they're reverse-engineered, rather than needing a
# layout change later.
_HOPPER_LADDER_LABELS = ["Hopper 1", "Hopper 2", "Hopper 3", "Hopper 4", "Hopper 5", "Hopper 6"]


class _Tile(QFrame):
    """One bordered label+value box - the basic unit of this page's
    layout, echoing the boxed readouts on a real dryer HMI screen."""

    def __init__(self, label: str, value_pt: int = 20):
        super().__init__()
        self.setFrameShape(QFrame.Shape.Box)
        self.setStyleSheet("QFrame { border: 1px solid #888; border-radius: 4px; background: #f5fbfb; }")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(2)

        name = QLabel(label)
        name.setStyleSheet("border: none; background: transparent; color: #333; font-size: 9pt;")
        layout.addWidget(name)

        self.value_label = QLabel("--")
        self.value_label.setStyleSheet(
            f"border: none; background: transparent; color: #111; font-size: {value_pt}pt; font-weight: bold;"
        )
        layout.addWidget(self.value_label)

    def set_value(self, text: str):
        self.value_label.setText(text)


class DryerDetailPage(QWidget):
    """Live status view for one Dryer-equipmentType RTSNow-SPI-CCP node -
    the first of what's meant to become a per-equipment-type/model family
    of custom pages (see MainWindow's routing), starting here with dryers
    since that's the model with real hardware to verify against. Polls
    the node's own /api/data directly by IP (see DryerStatusPoller), same
    "reach the node directly, not through the ESP-NOW gateway" pattern as
    PolymerPak's live status in NodeDetailPage - RTS-NOW's register-poll
    mechanism only carries the fixed Device Settings block, not this
    project's full live register set.

    Cam Cell, Cam Time, and per-hopper temperatures aren't wired to real
    data yet - none of those are decoded from any known SPI-CCP command
    on this dryer as of this page's first version (see _HOPPER_LADDER_LABELS'
    comment). They're placeholders so the layout doesn't need to change
    shape once that reverse-engineering happens.
    """

    def __init__(self):
        super().__init__()
        self._mac: str | None = None
        self._ip: str | None = None
        self._poller: DryerStatusPoller | None = None
        self._register_tiles: dict[int, _Tile] = {}
        self._status_tiles: dict[int, _Tile] = {}
        self._build_ui()
        self._show_placeholder()

    def _build_ui(self):
        layout = QVBoxLayout(self)

        self.placeholder_label = QLabel("Select a Dryer from the RTSNow page or sidebar to see its live status.")
        self.placeholder_label.setStyleSheet("color: gray;")
        layout.addWidget(self.placeholder_label)

        self.content = QWidget()
        content_layout = QVBoxLayout(self.content)

        header = QHBoxLayout()
        self.name_label = QLabel()
        self.name_label.setStyleSheet("font-size: 16pt; font-weight: bold;")
        header.addWidget(self.name_label)
        self.model_label = QLabel()
        self.model_label.setStyleSheet("color: gray;")
        header.addWidget(self.model_label)
        header.addStretch(1)
        self.comms_label = QLabel("Comms unknown")
        header.addWidget(self.comms_label)
        content_layout.addLayout(header)

        # Setpoint/Temp - the two figures an operator glances at first,
        # given the most visual weight (largest tiles), same as the top
        # bar of a real dryer HMI screen.
        top_row = QHBoxLayout()
        self._register_tiles[40010] = _Tile(_REGISTER_TILES[40010], value_pt=32)
        self._register_tiles[40012] = _Tile(_REGISTER_TILES[40012], value_pt=32)
        top_row.addWidget(self._register_tiles[40010])
        top_row.addWidget(self._register_tiles[40012])
        content_layout.addLayout(top_row)

        # Everything else this model reports - one tile per register in
        # _REGISTER_TILES not already placed above.
        process_box = QGroupBox("Process")
        process_grid = QGridLayout(process_box)
        remaining = [reg for reg in _REGISTER_TILES if reg not in (40010, 40012)]
        for i, reg in enumerate(remaining):
            tile = _Tile(_REGISTER_TILES[reg])
            self._register_tiles[reg] = tile
            process_grid.addWidget(tile, i // 4, i % 4)
        content_layout.addWidget(process_box)

        hopper_box = QGroupBox("Hopper Temps (not yet decoded)")
        hopper_grid = QGridLayout(hopper_box)
        self._hopper_tiles = []
        for i, label in enumerate(_HOPPER_LADDER_LABELS):
            tile = _Tile(label)
            self._hopper_tiles.append(tile)
            hopper_grid.addWidget(tile, 0, i)
        content_layout.addWidget(hopper_box)

        cam_box = QGroupBox("Cam (not yet decoded)")
        cam_row = QHBoxLayout(cam_box)
        self.cam_cell_tile = _Tile("Cam Cell")
        self.cam_time_tile = _Tile("Cam Time")
        cam_row.addWidget(self.cam_cell_tile)
        cam_row.addWidget(self.cam_time_tile)
        cam_row.addStretch(1)
        content_layout.addWidget(cam_box)

        status_box = QGroupBox("Raw Status (bit meanings not yet confirmed)")
        status_row = QHBoxLayout(status_box)
        for reg, label in _STATUS_TILES.items():
            tile = _Tile(label)
            self._status_tiles[reg] = tile
            status_row.addWidget(tile)
        status_row.addStretch(1)
        content_layout.addWidget(status_box)

        content_layout.addStretch(1)
        layout.addWidget(self.content)

    def _show_placeholder(self):
        self._mac = None
        self._ip = None
        self._stop_poller()
        self.placeholder_label.setVisible(True)
        self.content.setVisible(False)

    def _stop_poller(self):
        poller = self._poller
        self._poller = None
        if poller is None:
            return
        # See DryerStatusPoller.STOP_WAIT_MS's comment - must wait() after
        # stop() before dropping the last reference, not just disconnect.
        poller.status_received.disconnect(self._on_status_received)
        poller.status_error.disconnect(self._on_status_error)
        poller.stop()
        poller.wait(DryerStatusPoller.STOP_WAIT_MS)

    def _start_poller(self, ip: str):
        self._poller = DryerStatusPoller(ip)
        self._poller.status_received.connect(self._on_status_received)
        self._poller.status_error.connect(self._on_status_error)
        self._poller.start()

    def show_device(self, dev: KnownDevice):
        is_new_selection = dev.mac != self._mac
        self._mac = dev.mac
        self._ip = dev.ip
        self.placeholder_label.setVisible(False)
        self.content.setVisible(True)
        self.name_label.setText(dev.friendly_name)

        if is_new_selection:
            self._stop_poller()
            self.comms_label.setText("Comms unknown")
            self.model_label.setText("")
            if dev.ip:
                self._start_poller(dev.ip)
            else:
                self.comms_label.setText("No IP known (mesh-only node)")

    def refresh_if_current(self, dev: KnownDevice):
        if self._mac == dev.mac:
            self.show_device(dev)

    def current_mac(self) -> str | None:
        return self._mac

    def shutdown(self):
        """Call on application exit - see NodeDetailPage.shutdown()'s
        identical reasoning."""
        self._stop_poller()

    def _on_status_received(self, data: dict):
        self.model_label.setText(f"{data.get('equipmentType', '?')} / {data.get('model', '?')}")
        self.comms_label.setText("Comms OK")
        self.comms_label.setStyleSheet("color: green;")

        live = {r["reg"]: r for r in data.get("registers", []) if r.get("present")}
        for reg, tile in self._register_tiles.items():
            reg_data = live.get(reg)
            tile.set_value(str(reg_data["value"]) if reg_data else "--")
        for reg, tile in self._status_tiles.items():
            reg_data = live.get(reg)
            tile.set_value(str(reg_data["value"]) if reg_data else "--")

    def _on_status_error(self, message: str):
        self.comms_label.setText(f"Comms lost: {message}")
        self.comms_label.setStyleSheet("color: red;")
