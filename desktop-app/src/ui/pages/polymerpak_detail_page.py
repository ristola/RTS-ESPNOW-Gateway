import math

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPen, QRadialGradient
from PySide6.QtWidgets import (
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from src.gateway.models import KnownDevice
from src.gateway.polymerpak_status import PolymerPakStatusPoller
from src.ui.pages.node_detail_page import (
    POLYMERPAK_FIELDS,
    POLYMERPAK_STATUS_FIELDS,
    POLYMERPAK_TELEMETRY_FIELDS,
    DoubleClickToEditLineEdit,
    decode_polymerpak_register_pair,
)


class SunPositionDiagram(QWidget):
    """Polar sky-dome view, looking straight down from overhead: azimuth
    is compass direction (0=N at top, clockwise - matches solar_position.
    cpp's own atan2 convention), elevation is distance from center
    (zenith/90 deg at the center, horizon/0 deg at the outer ring).

    This is what "current tracking" actually means for this project (see
    TrackerSettings.h/main.cpp) - there is no motorized actuator or
    position-feedback sensor at all, just a periodic human-followed
    recommendation (formatMoveRecommendation()) for when to physically
    re-aim the panel by one more trackerStepDeg. The one position this
    can genuinely show is where the sun itself is right now (plus which
    way it's drifting, via the trailing arrow) - not "the panel is here,
    the sun is there," since the panel's own actual aim point is never
    reported anywhere.
    """

    _LOOKAHEAD_MIN = 15  # trailing-arrow projection window

    def __init__(self):
        super().__init__()
        self.setMinimumSize(260, 260)
        self._elevation_deg = 0.0
        self._azimuth_deg = 0.0
        self._el_rate = 0.0
        self._az_rate = 0.0
        self._sun_up = False
        self._has_data = False

    def set_position(self, elevation_deg: float, azimuth_deg: float, el_rate: float, az_rate: float, sun_up: bool):
        self._elevation_deg = elevation_deg
        self._azimuth_deg = azimuth_deg
        self._el_rate = el_rate
        self._az_rate = az_rate
        self._sun_up = sun_up
        self._has_data = True
        self.update()

    def clear(self):
        self._has_data = False
        self.update()

    def _polar_to_point(self, cx: float, cy: float, radius: float, elevation_deg: float, azimuth_deg: float):
        el_clamped = max(0.0, min(90.0, elevation_deg))
        r = radius * (90.0 - el_clamped) / 90.0
        ang = math.radians(azimuth_deg)
        return cx + r * math.sin(ang), cy - r * math.cos(ang)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        side = min(self.width(), self.height())
        cx, cy = self.width() / 2.0, self.height() / 2.0
        radius = side / 2.0 - 26

        # Explicit card background for the whole widget (not just the sky
        # circle) - painted first so the compass labels just outside the
        # circle always sit on a known, predictable color rather than
        # whatever's behind this widget (the OS/system theme's own
        # background, which varies and can make dark label text
        # unreadable against a dark theme).
        painter.fillRect(self.rect(), QColor("#f2f2f2"))

        sky = QRadialGradient(cx, cy, radius * 1.3)
        if self._has_data and self._sun_up:
            sky.setColorAt(0, QColor("#bfe6f7"))
            sky.setColorAt(1, QColor("#4a90c4"))
        elif self._has_data:
            sky.setColorAt(0, QColor("#2c3e6b"))
            sky.setColorAt(1, QColor("#0d1b3d"))
        else:
            sky.setColorAt(0, QColor("#d8d8d8"))
            sky.setColorAt(1, QColor("#a8a8a8"))
        painter.setPen(QPen(QColor("#1a1a1a"), 2))
        painter.setBrush(sky)
        painter.drawEllipse(QPointF(cx, cy), radius, radius)

        # Elevation guide rings (30/60 deg) - horizon itself is the outer
        # circle just drawn, zenith is the unmarked center point.
        painter.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt.PenStyle.DashLine))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        for el_ring in (30, 60):
            r = radius * (90 - el_ring) / 90.0
            painter.drawEllipse(QPointF(cx, cy), r, r)

        # Compass labels just outside the horizon ring.
        painter.setPen(QPen(QColor("#222")))
        painter.setFont(QFont("", 10, QFont.Weight.Bold))
        for label, az in (("N", 0), ("E", 90), ("S", 180), ("W", 270)):
            ang = math.radians(az)
            lx = cx + (radius + 14) * math.sin(ang)
            ly = cy - (radius + 14) * math.cos(ang)
            painter.drawText(QRectF(lx - 12, ly - 10, 24, 20), Qt.AlignmentFlag.AlignCenter, label)

        if not self._has_data:
            painter.setPen(QColor("#555"))
            painter.setFont(QFont("", 9))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "Poll registers to see sun position")
            return

        sx, sy = self._polar_to_point(cx, cy, radius, self._elevation_deg, self._azimuth_deg)
        below_horizon = self._elevation_deg < 0

        # Trailing arrow toward where the sun will be in _LOOKAHEAD_MIN
        # minutes, from the rates alone - the only "motion" this diagram
        # can show, since there's no actual tracker position to compare
        # against (see the class docstring).
        if abs(self._el_rate) > 0.001 or abs(self._az_rate) > 0.001:
            future_el = self._elevation_deg + self._el_rate * self._LOOKAHEAD_MIN
            future_az = self._azimuth_deg + self._az_rate * self._LOOKAHEAD_MIN
            fx, fy = self._polar_to_point(cx, cy, radius, future_el, future_az)
            painter.setPen(QPen(QColor("#d2691e" if not below_horizon else "#cccccc"), 2))
            painter.drawLine(QPointF(sx, sy), QPointF(fx, fy))
            # Simple arrowhead.
            angle = math.atan2(fy - sy, fx - sx)
            for spread in (2.6, -2.6):
                hx = fx - 8 * math.cos(angle + spread * 0.3)
                hy = fy - 8 * math.sin(angle + spread * 0.3)
                painter.drawLine(QPointF(fx, fy), QPointF(hx, hy))

        sun_color = QColor("#ffcc33") if not below_horizon else QColor("#8a8a8a")
        glow_color = QColor(255, 220, 120, 210) if not below_horizon else QColor(160, 160, 160, 130)
        glow = QRadialGradient(sx, sy, 22)
        glow.setColorAt(0, glow_color)
        glow.setColorAt(1, QColor(glow_color.red(), glow_color.green(), glow_color.blue(), 0))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(glow)
        painter.drawEllipse(QPointF(sx, sy), 22, 22)
        painter.setBrush(sun_color)
        painter.setPen(QPen(QColor("#8a5000" if not below_horizon else "#555555"), 1.5))
        painter.drawEllipse(QPointF(sx, sy), 8, 8)

        if below_horizon:
            painter.setPen(QColor("#ffffff") if self.height() > 0 else QColor("#000"))
            painter.setFont(QFont("", 9))
            painter.drawText(
                QRectF(cx - radius, cy + radius - 24, radius * 2, 20),
                Qt.AlignmentFlag.AlignCenter,
                "Sun below horizon",
            )


class PolymerPakDetailPage(QWidget):
    """Dedicated detail page for a PolymerPak solar-tracker node (parallel
    to DryerDetailPage for SPI-CCP dryers) - identity, a live sun-position
    diagram, the 4 persisted settings, live telemetry, remote control
    (reboot/poll registers), and OTA push. Reached from the Dashboard's
    device table or a PolymerPak sidebar child whenever dev.project_name
    == "PolymerPak" (see main_window.py's _show_device_page)."""

    rename_requested = Signal(str, str)  # mac, new_name
    ota_requested = Signal(str, str)  # ip, file_path
    reboot_requested = Signal(str)  # mac
    poll_registers_requested = Signal(str)  # mac
    config_setting_requested = Signal(str, str, object)  # mac, key, value

    _FIELD_RANGES = {
        "siteLatitude": (-90.0, 90.0),
        "siteLongitude": (-180.0, 180.0),
        "sunElevationDeg": (0.0, 90.0),
        "trackerStepDeg": (0.0001, 90.0),
    }

    _STATUS_FORMATTERS = {
        "elevationDeg": lambda v: f"{v:.1f}°",
        "azimuthDeg": lambda v: f"{v:.1f}°",
        "elRateDegPerMin": lambda v: f"{v:+.2f} °/min",
        "azRateDegPerMin": lambda v: f"{v:+.2f} °/min",
    }

    def __init__(self):
        super().__init__()
        self._mac: str | None = None
        self._ip: str | None = None
        self._last_known_name = ""
        self._field_last_known: dict[str, str] = {}
        self._poller: PolymerPakStatusPoller | None = None
        self._ota_file_path: str | None = None
        self._build_ui()
        self._show_placeholder()

    def _build_ui(self):
        layout = QVBoxLayout(self)

        self.placeholder_label = QLabel("Select a PolymerPak device to see its details.")
        self.placeholder_label.setStyleSheet("color: gray;")
        layout.addWidget(self.placeholder_label)

        identity_box = QGroupBox("Identity")
        identity_layout = QVBoxLayout(identity_box)
        name_row = QHBoxLayout()
        name_row.addWidget(QLabel("Name:"))
        self.name_edit = DoubleClickToEditLineEdit()
        self.name_edit.editingFinished.connect(self._on_name_editing_finished)
        self.rename_status_label = QLabel("")
        self.rename_status_label.setStyleSheet("color: gray;")
        name_row.addWidget(self.name_edit)
        name_row.addWidget(self.rename_status_label)
        name_row.addStretch(1)
        identity_layout.addLayout(name_row)

        details_row = QHBoxLayout()
        self.device_id_label = QLabel("-")
        self.mac_label = QLabel("-")
        self.ip_label = QLabel("-")
        self.last_seen_label = QLabel("-")
        for caption, value_label in (
            ("Device ID:", self.device_id_label),
            ("MAC:", self.mac_label),
            ("IP:", self.ip_label),
            ("Last seen:", self.last_seen_label),
        ):
            details_row.addWidget(QLabel(caption))
            details_row.addWidget(value_label)
            details_row.addSpacing(12)
        details_row.addStretch(1)
        identity_layout.addLayout(details_row)
        layout.addWidget(identity_box)
        self.identity_box = identity_box

        diagram_box = QGroupBox("Sun Position")
        diagram_layout = QVBoxLayout(diagram_box)
        self.diagram = SunPositionDiagram()
        diagram_layout.addWidget(self.diagram)
        layout.addWidget(diagram_box)
        self.diagram_box = diagram_box

        live_box = QGroupBox("Live Status")
        live_layout = QHBoxLayout(live_box)
        self.live_labels: dict[str, QLabel] = {}
        for key, label in POLYMERPAK_STATUS_FIELDS:
            col = QVBoxLayout()
            caption = QLabel(label)
            caption.setStyleSheet("color: gray; font-size: 9pt;")
            value = QLabel("-")
            value.setStyleSheet("font-size: 12pt; font-weight: bold;")
            col.addWidget(caption)
            col.addWidget(value)
            self.live_labels[key] = value
            live_layout.addLayout(col)
        self.live_status_label = QLabel("")
        self.live_status_label.setStyleSheet("color: gray;")
        layout.addWidget(live_box)
        layout.addWidget(self.live_status_label)
        self.live_box = live_box

        config_box = QGroupBox("Configuration")
        config_layout = QVBoxLayout(config_box)
        self.field_edits: dict[str, DoubleClickToEditLineEdit] = {}
        for key, label in POLYMERPAK_FIELDS:
            row = QHBoxLayout()
            row.addWidget(QLabel(label + ":"))
            edit = DoubleClickToEditLineEdit()
            edit.setMaximumWidth(120)
            edit.editingFinished.connect(lambda k=key: self._on_field_editing_finished(k))
            self.field_edits[key] = edit
            self._field_last_known[key] = ""
            row.addWidget(edit)
            row.addStretch(1)
            config_layout.addLayout(row)
        self.config_status_label = QLabel("Poll registers to see this node's current configuration.")
        self.config_status_label.setStyleSheet("color: gray;")
        config_layout.addWidget(self.config_status_label)
        layout.addWidget(config_box)
        self.config_box = config_box

        remote_box = QGroupBox("Remote Control")
        remote_layout = QVBoxLayout(remote_box)
        remote_buttons_row = QHBoxLayout()
        self.reboot_btn = QPushButton("Reboot Node")
        self.reboot_btn.clicked.connect(self._on_reboot_clicked)
        self.poll_registers_btn = QPushButton("Poll Registers")
        self.poll_registers_btn.clicked.connect(self._on_poll_registers_clicked)
        remote_buttons_row.addWidget(self.reboot_btn)
        remote_buttons_row.addWidget(self.poll_registers_btn)
        remote_buttons_row.addStretch(1)
        remote_layout.addLayout(remote_buttons_row)
        self.registers_status_label = QLabel("")
        self.registers_status_label.setStyleSheet("color: gray;")
        remote_layout.addWidget(self.registers_status_label)
        self.registers_table = QTableWidget(0, 3)
        self.registers_table.setHorizontalHeaderLabels(["Register", "Name", "Value"])
        self.registers_table.verticalHeader().setVisible(False)
        self.registers_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.registers_table.setMaximumHeight(220)
        remote_layout.addWidget(self.registers_table)
        layout.addWidget(remote_box)
        self.remote_box = remote_box

        ota_box = QGroupBox("OTA Update (Wi-Fi only)")
        ota_layout = QVBoxLayout(ota_box)
        ota_row = QHBoxLayout()
        self.ota_file_label = QLabel("No file selected")
        self.ota_browse_btn = QPushButton("Browse for .bin...")
        self.ota_browse_btn.clicked.connect(self._on_browse_clicked)
        self.ota_push_btn = QPushButton("Push Update")
        self.ota_push_btn.clicked.connect(self._on_push_clicked)
        self.ota_push_btn.setEnabled(False)
        ota_row.addWidget(self.ota_browse_btn)
        ota_row.addWidget(self.ota_file_label, stretch=1)
        ota_row.addWidget(self.ota_push_btn)
        ota_layout.addLayout(ota_row)
        self.ota_progress = QProgressBar()
        self.ota_progress.setRange(0, 100)
        ota_layout.addWidget(self.ota_progress)
        self.ota_status_label = QLabel("")
        self.ota_status_label.setStyleSheet("color: gray;")
        ota_layout.addWidget(self.ota_status_label)
        layout.addWidget(ota_box)
        self.ota_box = ota_box

        layout.addStretch(1)

    def _show_placeholder(self):
        self._mac = None
        self._ip = None
        self._stop_poller()
        self.placeholder_label.setVisible(True)
        for box in (self.identity_box, self.diagram_box, self.live_box, self.config_box, self.remote_box, self.ota_box):
            box.setVisible(False)
        self.diagram.clear()

    def _stop_poller(self):
        poller = self._poller
        self._poller = None
        if poller is None:
            return
        # Same disconnect-then-bounded-wait discipline as NodeDetailPage's
        # own _stop_polymerpak_poller - destroying a QThread while its
        # underlying OS thread is still running is undefined behavior in
        # Qt, not just a leak.
        poller.status_received.disconnect(self._on_status_received)
        poller.status_error.disconnect(self._on_status_error)
        poller.stop()
        poller.wait(PolymerPakStatusPoller.STOP_WAIT_MS)

    def _start_poller(self, ip: str):
        self._poller = PolymerPakStatusPoller(ip)
        self._poller.status_received.connect(self._on_status_received)
        self._poller.status_error.connect(self._on_status_error)
        self._poller.start()

    def _on_status_received(self, data: dict):
        for key, label_widget in self.live_labels.items():
            value = data.get(key)
            if value is None:
                label_widget.setText("-")
                continue
            formatter = self._STATUS_FORMATTERS.get(key)
            text = formatter(value) if formatter else str(value)
            if key == "elevationDeg" and "sunUp" in data:
                text += " (UP)" if data["sunUp"] else " (DOWN)"
            label_widget.setText(text)
        self.live_status_label.setText("Live - updated just now.")

        elevation = data.get("elevationDeg")
        azimuth = data.get("azimuthDeg")
        if elevation is not None and azimuth is not None:
            self.diagram.set_position(
                elevation, azimuth,
                data.get("elRateDegPerMin", 0.0), data.get("azRateDegPerMin", 0.0),
                bool(data.get("sunUp", elevation > 0)),
            )

    def _on_status_error(self, message: str):
        self.live_status_label.setText(f"Could not reach node's HTTP status endpoint: {message}")

    def show_device(self, dev: KnownDevice):
        is_new_selection = dev.mac != self._mac
        if is_new_selection:
            self.rename_status_label.setText("")
            self.registers_status_label.setText("")
            self.registers_table.setRowCount(0)
            self.config_status_label.setText("Poll registers to see this node's current configuration.")
        self._mac = dev.mac
        self._ip = dev.ip
        self.placeholder_label.setVisible(False)
        for box in (self.identity_box, self.diagram_box, self.live_box, self.config_box, self.remote_box, self.ota_box):
            box.setVisible(True)

        if is_new_selection:
            self._stop_poller()
            if dev.ip:
                self._start_poller(dev.ip)
            else:
                self.diagram.clear()
                self.live_status_label.setText("No IP - mesh-only, live telemetry needs Wi-Fi.")

        self.device_id_label.setText(f"0x{dev.device_id:08X}")
        if not self.name_edit.hasFocus():
            self.name_edit.setText(dev.friendly_name)
            self._last_known_name = dev.friendly_name
        self.mac_label.setText(dev.mac)
        self.ip_label.setText(dev.ip or "- (mesh-only, no Wi-Fi)")
        self.last_seen_label.setText(f"{dev.age_ms // 1000}s ago")

        self.reboot_btn.setEnabled(True)
        self.poll_registers_btn.setEnabled(True)
        self._update_ota_enabled()

        if is_new_selection:
            # Auto-poll so Configuration/Registers reflect this node's
            # actual current state right away, matching NodeDetailPage/
            # DryerDetailPage's own behavior on first selecting a device.
            self.poll_registers_requested.emit(dev.mac)

    def refresh_if_current(self, dev: KnownDevice):
        if self._mac == dev.mac:
            self.show_device(dev)

    def current_mac(self) -> str | None:
        return self._mac

    def shutdown(self):
        """Call on application exit - see _stop_poller's own comment."""
        self._stop_poller()

    def _update_ota_enabled(self):
        self.ota_push_btn.setEnabled(bool(self._ip and self._ota_file_path))

    def _on_name_editing_finished(self):
        self.name_edit.exit_edit_mode()
        if not self._mac:
            return
        new_name = self.name_edit.text().strip()
        if new_name == self._last_known_name:
            return
        if not new_name:
            QMessageBox.warning(self, "Missing name", "Enter a name first.")
            self.name_edit.setText(self._last_known_name)
            return
        if len(new_name.encode("utf-8")) > 23:
            QMessageBox.warning(self, "Name too long", "Names are limited to 23 characters.")
            self.name_edit.setText(self._last_known_name)
            return
        self._last_known_name = new_name
        self.set_rename_status("Sent - awaiting node confirmation...")
        self.rename_requested.emit(self._mac, new_name)

    def set_rename_status(self, text: str):
        self.rename_status_label.setText(text)

    def _on_reboot_clicked(self):
        if not self._mac:
            return
        if QMessageBox.question(
            self, "Reboot node",
            f"Reboot {self.name_edit.text()} ({self._mac})? It will briefly go offline."
        ) != QMessageBox.StandardButton.Yes:
            return
        self.reboot_requested.emit(self._mac)

    def _on_poll_registers_clicked(self):
        if not self._mac:
            return
        self.registers_status_label.setText("Requested - awaiting reply...")
        self.registers_table.setRowCount(0)
        self.poll_registers_requested.emit(self._mac)

    def show_register_values(self, start_register: int, values: list[int]):
        self.registers_status_label.setText(f"{len(values)} register(s), received just now.")
        all_fields = POLYMERPAK_FIELDS + POLYMERPAK_TELEMETRY_FIELDS
        self.registers_table.setRowCount(len(all_fields))
        for i, (_key, label) in enumerate(all_fields):
            reg_lo = 1 + i * 2
            reg_hi = reg_lo + 1
            self.registers_table.setItem(i, 0, QTableWidgetItem(f"{reg_lo}-{reg_hi}"))
            self.registers_table.setItem(i, 1, QTableWidgetItem(label))
            text = decode_polymerpak_register_pair(values, start_register, i)
            self.registers_table.setItem(i, 2, QTableWidgetItem(text if text is not None else "(not in this poll)"))

        for i, (key, _label) in enumerate(POLYMERPAK_FIELDS):
            text = decode_polymerpak_register_pair(values, start_register, i)
            if text is None:
                continue
            self._field_last_known[key] = text
            if not self.field_edits[key].hasFocus():
                self.field_edits[key].setText(text)
        self.config_status_label.setText("Reflects this node's configuration as of the last register poll.")

    def _on_field_editing_finished(self, key: str):
        edit = self.field_edits[key]
        edit.exit_edit_mode()
        if not self._mac:
            return
        raw = edit.text().strip()
        if raw == self._field_last_known.get(key, ""):
            return
        try:
            value = float(raw)
        except ValueError:
            QMessageBox.warning(self, "Invalid value", f"{key} must be a number.")
            edit.setText(self._field_last_known.get(key, ""))
            return
        low, high = self._FIELD_RANGES[key]
        if not (low <= value <= high):
            QMessageBox.warning(self, "Invalid value", f"{key} must be between {low} and {high}.")
            edit.setText(self._field_last_known.get(key, ""))
            return
        self._field_last_known[key] = raw
        self.config_status_label.setText(f"Sent - awaiting node confirmation ({key}={value})...")
        self.config_setting_requested.emit(self._mac, key, value)

    def set_config_ack_status(self, key: str, accepted: bool):
        if accepted:
            self.config_status_label.setText(f"\"{key}\" confirmed by node.")
        else:
            self.config_status_label.setText(f"Node rejected \"{key}\" - check the value and try again.")

    def _on_browse_clicked(self):
        file_path, _ = QFileDialog.getOpenFileName(self, "Select firmware image", "", "Firmware images (*.bin)")
        if not file_path:
            return
        self._ota_file_path = file_path
        self.ota_file_label.setText(file_path)
        self._update_ota_enabled()

    def _on_push_clicked(self):
        if not self._ip or not self._ota_file_path:
            return
        if QMessageBox.question(
            self, "Push OTA update",
            f"Push {self._ota_file_path} to {self.name_edit.text()} ({self._ip})? The node will reboot when done."
        ) != QMessageBox.StandardButton.Yes:
            return
        self.ota_progress.setValue(0)
        self.ota_status_label.setText("Starting...")
        self.ota_push_btn.setEnabled(False)
        self.ota_requested.emit(self._ip, self._ota_file_path)

    def set_ota_progress(self, percent: int):
        self.ota_progress.setValue(percent)
        self.ota_status_label.setText(f"Uploading... {percent}%")

    def set_ota_result(self, success: bool, message: str):
        self.ota_status_label.setText(("Success: " if success else "Failed: ") + message)
        self._update_ota_enabled()
