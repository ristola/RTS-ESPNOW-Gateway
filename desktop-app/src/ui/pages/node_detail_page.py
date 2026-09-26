from PySide6.QtCore import Signal, Qt
from PySide6.QtWidgets import (
    QButtonGroup,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
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

# Mirrors DeviceSettings.h's kDryerModels/kCrystallizerModels and
# modelTypeCode()'s 0-7 encoding exactly - these are compile-time
# constants on every SPI-IM/RTSNow node, so there's no need to query a
# device for them (unlike its *current* equipment/model/address/baud,
# which do need a live register poll - see show_register_values below).
#
# An "ETHERNET" model button used to always be appended here too,
# regardless of equipment - the one real consumer, RTSNow-SPI-CCP's
# env:node_atoms3_poe (AtomS3 + PoE/W5500 base instead of an RS-485
# tail), is retired/no longer in use as of 2026-09-20 (confirmed by the
# user, not assumed) - see _rebuild_model_buttons for where it was
# removed. MODEL_TYPE_CODES below still has its old code-7 slot, kept
# for decoding compatibility (see that constant's own comment).
EQUIPMENT_TYPES = ["Dryer", "Crystallizer"]
MODELS_BY_EQUIPMENT = {
    "Dryer": ["FC", "FD", "FN", "ADV", "CD"],
    "Crystallizer": ["FC-XTLR", "FN-XTLR"],
}
BAUD_RATES = [1200, 2400, 4800, 9600, 19200]

# PolymerPak's solar-tracker settings (see that project's
# TrackerSettings.h/config.h) - (RTS-NOW setting key, field label). Order
# matches fillRegisterBlock()'s register layout in that project's
# main.cpp, which _update_config_from_registers() below relies on.
POLYMERPAK_FIELDS = [
    ("siteLatitude", "Site Latitude"),
    ("siteLongitude", "Site Longitude"),
    ("sunElevationDeg", "Sun Elevation Threshold"),
    ("trackerStepDeg", "Tracker Step (deg)"),
]

# PolymerPak's /api/status JSON keys (see that project's handleApiStatus())
# -> (field label, format string) for the read-only "Live Status" column -
# mirrors that node's own web dashboard.
POLYMERPAK_STATUS_FIELDS = [
    ("elevationDeg", "Elevation"),
    ("azimuthDeg", "Azimuth"),
    ("elRateDegPerMin", "EL Rate"),
    ("azRateDegPerMin", "AZ Rate"),
    ("moveEl", "Move EL"),
    ("moveAz", "Move AZ"),
    ("usableWindow", "Usable Sunlight"),
    ("localTime", "Local Time"),
]

# modelTypeCode() 0-7 -> (equipmentType, model) - see DeviceSettings.cpp's
# modelTypeCode()/setModelTypeCode() for the canonical mapping this mirrors.
MODEL_TYPE_CODES = [
    ("Dryer", "FC"), ("Dryer", "FD"), ("Dryer", "FN"), ("Dryer", "ADV"), ("Dryer", "CD"),
    ("Crystallizer", "FC-XTLR"), ("Crystallizer", "FN-XTLR"),
    ("Ethernet", "ETHERNET"),
]


class DoubleClickToEditLineEdit(QLineEdit):
    """A QLineEdit that only becomes editable on double-click - a single
    click (e.g. to click into some other widget) doesn't grab it or let
    the cursor land inside it. Pressing Enter or clicking away both
    already fire editingFinished (Qt's own behavior), which callers use
    to commit the value and are expected to call exit_edit_mode()
    afterward to drop back into this locked, double-click-only state."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.exit_edit_mode()

    def enter_edit_mode(self):
        self.setReadOnly(False)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setFocus()
        self.selectAll()

    def exit_edit_mode(self):
        self.setReadOnly(True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.clearFocus()

    def mouseDoubleClickEvent(self, event):
        self.enter_edit_mode()
        super().mouseDoubleClickEvent(event)


class NodeDetailPage(QWidget):
    """Compact identity, this node's live equipment/model/SPI/baud
    configuration, remote control (reboot/poll registers), and an OTA-push
    action - for one selected known device, reached from the Dashboard's
    device table or an RTSNow sidebar child."""

    rename_requested = Signal(str, str)  # mac, new_name
    ota_requested = Signal(str, str)  # ip, file_path
    reboot_requested = Signal(str)  # mac
    poll_registers_requested = Signal(str)  # mac
    config_setting_requested = Signal(str, str, object)  # mac, key, value

    def __init__(self):
        super().__init__()
        self._mac: str | None = None
        self._ip: str | None = None
        self._current_equipment: str | None = None
        self._model_buttons: dict[str, QPushButton] = {}
        self._last_known_name: str = ""
        self._last_known_spi_address: str = ""
        self._build_ui()
        self._show_placeholder()

    def _build_ui(self):
        layout = QVBoxLayout(self)

        self.placeholder_label = QLabel("Select a device from Node Network to see its details.")
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
        self.project_label = QLabel("-")
        self.type_label = QLabel("-")
        self.mac_label = QLabel("-")
        self.ip_label = QLabel("-")
        self.last_seen_label = QLabel("-")
        for caption, value_label in (
            ("Device ID:", self.device_id_label),
            ("Project:", self.project_label),
            ("Type:", self.type_label),
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

        config_box = QGroupBox("Configuration")
        config_layout = QVBoxLayout(config_box)

        equip_row = QHBoxLayout()
        equip_row.addWidget(QLabel("Equipment:"))
        self.equipment_group = QButtonGroup(self)
        self.equipment_group.setExclusive(True)
        self.equipment_buttons: dict[str, QPushButton] = {}
        for name in EQUIPMENT_TYPES:
            btn = QPushButton(name)
            btn.setCheckable(True)
            btn.clicked.connect(lambda checked, n=name: self._on_equipment_clicked(n))
            self.equipment_group.addButton(btn)
            self.equipment_buttons[name] = btn
            equip_row.addWidget(btn)
        equip_row.addStretch(1)
        config_layout.addLayout(equip_row)

        self.model_row = QHBoxLayout()
        self.model_row.addWidget(QLabel("Model:"))
        self.model_group = QButtonGroup(self)
        self.model_group.setExclusive(True)
        self.model_row.addStretch(1)
        config_layout.addLayout(self.model_row)

        addr_row = QHBoxLayout()
        addr_row.addWidget(QLabel("SPI Address:"))
        self.spi_address_edit = DoubleClickToEditLineEdit("32")
        self._last_known_spi_address = "32"
        self.spi_address_edit.setMaximumWidth(60)
        self.spi_address_edit.editingFinished.connect(self._on_spi_address_editing_finished)
        addr_row.addWidget(self.spi_address_edit)
        addr_row.addStretch(1)
        config_layout.addLayout(addr_row)

        baud_row = QHBoxLayout()
        baud_row.addWidget(QLabel("Baud Rate:"))
        self.baud_group = QButtonGroup(self)
        self.baud_group.setExclusive(True)
        self.baud_buttons: dict[int, QPushButton] = {}
        for baud in BAUD_RATES:
            btn = QPushButton(str(baud))
            btn.setCheckable(True)
            btn.clicked.connect(lambda checked, b=baud: self._on_baud_clicked(b))
            self.baud_group.addButton(btn)
            self.baud_buttons[baud] = btn
            baud_row.addWidget(btn)
        baud_row.addStretch(1)
        config_layout.addLayout(baud_row)

        self.config_status_label = QLabel("Poll registers to see this node's current configuration.")
        self.config_status_label.setStyleSheet("color: gray;")
        config_layout.addWidget(self.config_status_label)

        layout.addWidget(config_box)
        self.config_box = config_box

        # A completely different project (solar-tracker controller, see
        # "Customer Projects/PolymerPak") reachable through this same
        # Node Detail page via the Dashboard's device table (see
        # main_window.py's _rebuild_rtsnow_children() docstring) - it has
        # none of SPI-IM's Equipment/Model/SPI Address/Baud Rate concepts,
        # so it gets its own Configuration box instead, shown/hidden by
        # dev.project_name in show_device() rather than sharing config_box.
        polymerpak_config_box = QGroupBox("Configuration (PolymerPak)")
        polymerpak_columns = QHBoxLayout(polymerpak_config_box)

        # Left column: the 4 persisted settings (see TrackerSettings.h) -
        # same double-click-to-edit pattern as SPI-IM's SPI Address.
        settings_column = QVBoxLayout()
        self.polymerpak_edits: dict[str, DoubleClickToEditLineEdit] = {}
        self._polymerpak_last_known: dict[str, str] = {}
        for key, label in POLYMERPAK_FIELDS:
            row = QHBoxLayout()
            row.addWidget(QLabel(label + ":"))
            edit = DoubleClickToEditLineEdit()
            edit.setMaximumWidth(120)
            edit.editingFinished.connect(lambda k=key: self._on_polymerpak_field_editing_finished(k))
            self.polymerpak_edits[key] = edit
            self._polymerpak_last_known[key] = ""
            row.addWidget(edit)
            row.addStretch(1)
            settings_column.addLayout(row)

        self.polymerpak_status_label = QLabel("Poll registers to see this node's current configuration.")
        self.polymerpak_status_label.setStyleSheet("color: gray;")
        settings_column.addWidget(self.polymerpak_status_label)
        settings_column.addStretch(1)
        polymerpak_columns.addLayout(settings_column, stretch=1)

        # Right column: read-only live sun-position telemetry, fetched
        # over plain HTTP from the node's own /api/status (see
        # PolymerPakStatusPoller) - the same numbers as that node's own
        # web dashboard, just alongside the settings that drive them
        # instead of on a separate page.
        live_column = QVBoxLayout()
        live_column.addWidget(QLabel("Live Status:"))
        self.polymerpak_live_labels: dict[str, QLabel] = {}
        for key, label in POLYMERPAK_STATUS_FIELDS:
            row = QHBoxLayout()
            row.addWidget(QLabel(label + ":"))
            value_label = QLabel("-")
            self.polymerpak_live_labels[key] = value_label
            row.addWidget(value_label)
            row.addStretch(1)
            live_column.addLayout(row)
        self.polymerpak_live_status_label = QLabel("")
        self.polymerpak_live_status_label.setStyleSheet("color: gray;")
        live_column.addWidget(self.polymerpak_live_status_label)
        live_column.addStretch(1)
        polymerpak_columns.addLayout(live_column, stretch=1)

        layout.addWidget(polymerpak_config_box)
        self.polymerpak_config_box = polymerpak_config_box
        self._polymerpak_poller: PolymerPakStatusPoller | None = None
        self._rebuild_model_buttons("Dryer")

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

        self.registers_table = QTableWidget(0, 2)
        self.registers_table.setHorizontalHeaderLabels(["Register", "Value"])
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
        self._ota_file_path: str | None = None

        layout.addStretch(1)

    def _rebuild_model_buttons(self, equipment: str, active_model: str | None = None):
        """Swaps the Model row's buttons to match `equipment`'s valid model
        list (see MODELS_BY_EQUIPMENT) - called whenever the Equipment
        selection changes, or fresh register values report a different
        equipment/model than what was previously shown.

        ETHERNET used to always be appended here as an extra choice
        regardless of equipment (see the module comment) - the
        one real consumer, RTSNow-SPI-CCP's env:node_atoms3_poe (AtomS3 +
        PoE/W5500 base instead of an RS-485 tail), is retired/no longer
        in use as of 2026-09-20, confirmed directly by the user rather
        than assumed - so the button is removed here. MODEL_TYPE_CODES
        is left untouched (still has the correct 0-7 positional mapping
        DeviceSettings.cpp's modelTypeCode() defines) since editing it
        would risk breaking decoding for the other, still-real codes;
        if a device ever reports code 7 now, _update_rtsnow_config_from_
        registers still decodes it without erroring, it just won't find
        a matching button to highlight."""
        for btn in list(self._model_buttons.values()):
            self.model_group.removeButton(btn)
            self.model_row.removeWidget(btn)
            btn.deleteLater()
        self._model_buttons.clear()

        # Insert before the trailing stretch (always the last item in the row).
        insert_at = self.model_row.count() - 1
        for name in MODELS_BY_EQUIPMENT.get(equipment, []):
            btn = QPushButton(name)
            btn.setCheckable(True)
            btn.setChecked(name == active_model)
            btn.clicked.connect(lambda checked, n=name: self._on_model_clicked(n))
            self.model_group.addButton(btn)
            self._model_buttons[name] = btn
            self.model_row.insertWidget(insert_at, btn)
            insert_at += 1

    def set_enabled(self, enabled: bool):
        self.reboot_btn.setEnabled(enabled and self._mac is not None)
        self.poll_registers_btn.setEnabled(enabled and self._mac is not None)
        for btn in self.equipment_buttons.values():
            btn.setEnabled(enabled and self._mac is not None)
        for btn in self.baud_buttons.values():
            btn.setEnabled(enabled and self._mac is not None)
        self._update_ota_enabled()

    def _show_placeholder(self):
        self._mac = None
        self._ip = None
        self._current_project_name = None
        self._stop_polymerpak_poller()
        self.placeholder_label.setVisible(True)
        self.identity_box.setVisible(False)
        self.config_box.setVisible(False)
        self.polymerpak_config_box.setVisible(False)
        self.remote_box.setVisible(False)
        self.ota_box.setVisible(False)

    def _stop_polymerpak_poller(self):
        poller = self._polymerpak_poller
        self._polymerpak_poller = None
        if poller is None:
            return
        # Disconnect first so a reply already in flight when stop() is
        # called can't touch the UI after it's moved to a different
        # device. Then block briefly (bounded by STOP_WAIT_MS) so the
        # underlying OS thread has actually exited before this last
        # reference to it is dropped - destroying a QThread object while
        # its thread is still running is undefined behavior in Qt, not
        # just a leak. A short stall here (rare - only on an actual
        # device switch, at most a few seconds) beats that risk.
        poller.status_received.disconnect(self._on_polymerpak_status_received)
        poller.status_error.disconnect(self._on_polymerpak_status_error)
        poller.stop()
        poller.wait(PolymerPakStatusPoller.STOP_WAIT_MS)

    def _start_polymerpak_poller(self, ip: str):
        self._polymerpak_poller = PolymerPakStatusPoller(ip)
        self._polymerpak_poller.status_received.connect(self._on_polymerpak_status_received)
        self._polymerpak_poller.status_error.connect(self._on_polymerpak_status_error)
        self._polymerpak_poller.start()

    # Explicit per-key formatting instead of guessing from the value's
    # type/key name - only elevationDeg gets the "(UP)"/"(DOWN)" suffix
    # (from the JSON's separate "sunUp" bool), matching the webpage's own
    # badge next to its Elevation figure.
    _POLYMERPAK_STATUS_FORMATTERS = {
        "elevationDeg": lambda v: f"{v:.1f}°",
        "azimuthDeg": lambda v: f"{v:.1f}°",
        "elRateDegPerMin": lambda v: f"{v:+.2f} °/min",
        "azRateDegPerMin": lambda v: f"{v:+.2f} °/min",
    }

    def _on_polymerpak_status_received(self, data: dict):
        for key, label_widget in self.polymerpak_live_labels.items():
            value = data.get(key)
            if value is None:
                label_widget.setText("-")
                continue
            formatter = self._POLYMERPAK_STATUS_FORMATTERS.get(key)
            text = formatter(value) if formatter else str(value)
            if key == "elevationDeg" and "sunUp" in data:
                text += " (UP)" if data["sunUp"] else " (DOWN)"
            label_widget.setText(text)
        self.polymerpak_live_status_label.setText("Live - updated just now.")

    def _on_polymerpak_status_error(self, message: str):
        self.polymerpak_live_status_label.setText(f"Could not reach node's HTTP status endpoint: {message}")

    def show_device(self, dev: KnownDevice):
        is_new_selection = dev.mac != self._mac
        if is_new_selection:
            self.rename_status_label.setText("")  # only clear on an actual switch, not a periodic refresh
            self.registers_status_label.setText("")
            self.registers_table.setRowCount(0)
            self.config_status_label.setText("Poll registers to see this node's current configuration.")
        self._mac = dev.mac
        self._ip = dev.ip
        self._current_project_name = dev.project_name
        self.placeholder_label.setVisible(False)
        self.identity_box.setVisible(True)
        # The Equipment/Model/SPI Address/Baud Rate section only makes
        # sense for SPI-IM/RTSNow nodes - a different project reached via
        # the Dashboard's device table (e.g. PolymerPak's solar tracker,
        # see POLYMERPAK_FIELDS) gets its own Configuration box instead.
        # Anything that isn't recognized shows neither, rather than
        # guessing.
        self.config_box.setVisible(dev.project_name == "RTSNow")
        self.polymerpak_config_box.setVisible(dev.project_name == "PolymerPak")
        self.remote_box.setVisible(True)
        self.ota_box.setVisible(True)

        if is_new_selection:
            # Only (re)start the poller on an actual device switch, not
            # every periodic known_devices refresh - restarting it every
            # time would otherwise re-fire "Requested..." status text
            # continuously even when nothing changed.
            self._stop_polymerpak_poller()
            if dev.project_name == "PolymerPak" and dev.ip:
                self._start_polymerpak_poller(dev.ip)

        self.device_id_label.setText(f"0x{dev.device_id:08X}")
        self.project_label.setText(dev.project_name)
        self.type_label.setText(dev.device_type_name)
        if not self.name_edit.hasFocus():
            # Don't clobber an in-progress edit if a periodic known_devices
            # refresh happens to land while the user is mid-typing.
            self.name_edit.setText(dev.friendly_name)
            self._last_known_name = dev.friendly_name
        self.mac_label.setText(dev.mac)
        self.ip_label.setText(dev.ip or "- (mesh-only, no Wi-Fi)")
        self.last_seen_label.setText(f"{dev.age_ms // 1000}s ago")

        self.reboot_btn.setEnabled(True)
        self.poll_registers_btn.setEnabled(True)
        for btn in self.equipment_buttons.values():
            btn.setEnabled(True)
        for btn in self.baud_buttons.values():
            btn.setEnabled(True)
        self._update_ota_enabled()

        if is_new_selection:
            # Auto-poll so Configuration reflects this node's actual current
            # state right away, rather than showing stale/default values
            # (or nothing) until the user remembers to click Poll Registers
            # themselves.
            self.poll_registers_requested.emit(dev.mac)

    def refresh_if_current(self, dev: KnownDevice):
        """Called whenever a fresh known_devices snapshot arrives - updates
        the displayed fields in place if this is the device being shown."""
        if self._mac == dev.mac:
            self.show_device(dev)

    def current_mac(self) -> str | None:
        return self._mac

    def shutdown(self):
        """Call on application exit - destroying a QThread while its
        underlying OS thread is still running is undefined behavior in Qt
        (see _stop_polymerpak_poller's comment), and nothing else stops
        this page's poller once the app is closing, not just navigated
        away from."""
        self._stop_polymerpak_poller()

    def _update_ota_enabled(self):
        self.ota_push_btn.setEnabled(bool(self._ip and self._ota_file_path))

    def _on_name_editing_finished(self):
        # Fires on Enter *or* just losing focus (clicking elsewhere) - only
        # actually send anything if the text is different from what's
        # already known, so merely tabbing through the field doesn't spam
        # a rename request every time. Either way, "finished" means this
        # field locks back to double-click-only until the user double-
        # clicks it again.
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
        # Matches RTSNOW_DeviceIdentity.friendlyName (rtsnow_protocol.h) -
        # the node truncates to this anyway, but fail fast with a clear
        # message instead of silently losing characters on the device.
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
        self.registers_table.setRowCount(len(values))
        for row, value in enumerate(values):
            self.registers_table.setItem(row, 0, QTableWidgetItem(str(start_register + row)))
            self.registers_table.setItem(row, 1, QTableWidgetItem(str(value)))
        self._update_config_from_registers(start_register, values)

    def _update_config_from_registers(self, start_register: int, values: list[int]):
        """Dispatches to the right decode for whichever project owns the
        currently-selected device - SPI-IM/RTSNow's Modbus-style 400xx
        registers and PolymerPak's own scaled-float register layout mean
        completely different things at the same register numbers, so
        this must never try to decode one project's block as the
        other's."""
        if self._current_project_name == "PolymerPak":
            self._update_polymerpak_config_from_registers(start_register, values)
        elif self._current_project_name == "RTSNow":
            self._update_rtsnow_config_from_registers(start_register, values)

    def _update_polymerpak_config_from_registers(self, start_register: int, values: list[int]):
        """Decodes PolymerPak's fillRegisterBlock() layout (see that
        project's main.cpp): 4 values, each spanning 2 registers
        (high word then low word) scaled by 10000, starting at register
        1 - siteLatitude, siteLongitude, sunElevationDeg, trackerStepDeg
        in that order (see POLYMERPAK_FIELDS)."""
        for i, (key, _label) in enumerate(POLYMERPAK_FIELDS):
            hi_index = 1 + i * 2 - start_register
            lo_index = hi_index + 1
            if hi_index < 0 or lo_index >= len(values):
                continue
            scaled = (values[hi_index] << 16) | values[lo_index]
            if scaled >= 1 << 31:  # two's complement sign extension
                scaled -= 1 << 32
            text = f"{scaled / 10000.0:g}"
            self._polymerpak_last_known[key] = text
            if not self.polymerpak_edits[key].hasFocus():
                self.polymerpak_edits[key].setText(text)
        self.polymerpak_status_label.setText("Reflects this node's configuration as of the last register poll.")

    def _update_rtsnow_config_from_registers(self, start_register: int, values: list[int]):
        """Decodes Station ID (40002)/Baud Rate (40003)/Model Type (40004)
        out of a register poll to reflect this node's actual current
        configuration - see MODEL_TYPE_CODES for the 0-7 decoding this
        mirrors from DeviceSettings.cpp."""
        station_id_index = 40002 - start_register
        baud_index = 40003 - start_register
        model_type_index = 40004 - start_register
        if model_type_index < 0 or model_type_index >= len(values):
            return  # this poll didn't cover the registers we need

        code = values[model_type_index]
        if not (0 <= code < len(MODEL_TYPE_CODES)):
            self.config_status_label.setText(f"Unrecognized Model Type code {code} from device - not updating Configuration.")
            return

        equipment, model = MODEL_TYPE_CODES[code]
        if equipment in self.equipment_buttons:
            self._current_equipment = equipment
            self.equipment_buttons[equipment].setChecked(True)
        else:
            # code == 7 (ETHERNET) - a model choice orthogonal to
            # equipment type (see DeviceSettings.h's kEthernetModels
            # comment), not its own equipment-type button. The node
            # doesn't expose a separate register for the "true"
            # equipmentType while ETHERNET is selected, so leave the
            # Equipment buttons exactly as they already are instead of
            # forcing a deselect - fall back to whatever's already known,
            # or Dryer as a last resort on the very first poll.
            equipment = self._current_equipment or EQUIPMENT_TYPES[0]
        self._rebuild_model_buttons(equipment, model)

        if 0 <= station_id_index < len(values):
            self._last_known_spi_address = str(values[station_id_index])
            if not self.spi_address_edit.hasFocus():
                self.spi_address_edit.setText(self._last_known_spi_address)
        if 0 <= baud_index < len(values):
            baud = values[baud_index]
            for rate, btn in self.baud_buttons.items():
                btn.setChecked(rate == baud)

        self.config_status_label.setText("Reflects this node's configuration as of the last register poll.")

    def _on_equipment_clicked(self, name: str):
        if not self._mac:
            return
        self._current_equipment = name
        self._rebuild_model_buttons(name)
        self.config_status_label.setText(f"Sent - awaiting node confirmation ({name})...")
        self.config_setting_requested.emit(self._mac, "equipmentType", name)

    def _on_model_clicked(self, name: str):
        if not self._mac:
            return
        self.config_status_label.setText(f"Sent - awaiting node confirmation ({name})...")
        self.config_setting_requested.emit(self._mac, "model", name)

    def _on_spi_address_editing_finished(self):
        # Same "only send if it actually changed" discipline as
        # _on_name_editing_finished - editingFinished fires on Enter *or*
        # just losing focus. Either way, this field locks back to
        # double-click-only until double-clicked again.
        self.spi_address_edit.exit_edit_mode()
        if not self._mac:
            return
        raw = self.spi_address_edit.text().strip()
        if raw == self._last_known_spi_address:
            return
        try:
            address = int(raw)
        except ValueError:
            QMessageBox.warning(self, "Invalid address", "SPI Address must be a whole number (0-255).")
            self.spi_address_edit.setText(self._last_known_spi_address)
            return
        if not (0 <= address <= 255):
            QMessageBox.warning(self, "Invalid address", "SPI Address must be 0-255.")
            self.spi_address_edit.setText(self._last_known_spi_address)
            return
        self._last_known_spi_address = str(address)
        self.config_status_label.setText(f"Sent - awaiting node confirmation (address {address})...")
        self.config_setting_requested.emit(self._mac, "spiAddress", address)

    def _on_baud_clicked(self, baud: int):
        if not self._mac:
            return
        self.config_status_label.setText(f"Sent - awaiting node confirmation ({baud} baud)...")
        self.config_setting_requested.emit(self._mac, "spiBaudRate", baud)

    # Mirrors main.cpp's onRemoteSetting() range checks in that project -
    # not exhaustive validation, just enough to catch an obvious typo
    # locally instead of waiting on a round-trip rejection.
    _POLYMERPAK_FIELD_RANGES = {
        "siteLatitude": (-90.0, 90.0),
        "siteLongitude": (-180.0, 180.0),
        "sunElevationDeg": (0.0, 90.0),
        "trackerStepDeg": (0.0001, 90.0),
    }

    def _on_polymerpak_field_editing_finished(self, key: str):
        edit = self.polymerpak_edits[key]
        # Same "only send if it actually changed, then lock back to
        # double-click-only" discipline as _on_spi_address_editing_finished.
        edit.exit_edit_mode()
        if not self._mac:
            return
        raw = edit.text().strip()
        if raw == self._polymerpak_last_known.get(key, ""):
            return
        try:
            value = float(raw)
        except ValueError:
            QMessageBox.warning(self, "Invalid value", f"{key} must be a number.")
            edit.setText(self._polymerpak_last_known.get(key, ""))
            return
        low, high = self._POLYMERPAK_FIELD_RANGES[key]
        if not (low <= value <= high):
            QMessageBox.warning(self, "Invalid value", f"{key} must be between {low} and {high}.")
            edit.setText(self._polymerpak_last_known.get(key, ""))
            return
        self._polymerpak_last_known[key] = raw
        self.polymerpak_status_label.setText(f"Sent - awaiting node confirmation ({key}={value})...")
        self.config_setting_requested.emit(self._mac, key, value)

    def set_config_ack_status(self, key: str, accepted: bool):
        label = self.polymerpak_status_label if key in self._POLYMERPAK_FIELD_RANGES else self.config_status_label
        if accepted:
            label.setText(f"\"{key}\" confirmed by node.")
        else:
            label.setText(f"Node rejected \"{key}\" - check the value and try again.")

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
