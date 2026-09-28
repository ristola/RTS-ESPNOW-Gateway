import time
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QSplitter,
    QStackedWidget,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from src.gateway.device_info_client import DeviceInfoFetcher
from src.gateway.espnow_ota_transfer import EspNowOtaTransfer
from src.gateway.models import MODBUS_REG_MODEL_TYPE, KnownDevice, decode_model_type_register
from src.gateway.network_client import NetworkGatewayClient
from src.gateway.node_flasher import ChipMacReader, NodeFlasherClient, PioBuildClient
from src.gateway.ota_client import OtaPushClient
from src.gateway.ports import list_serial_ports
from src.gateway.serial_client import GatewayClient
from src.gateway.settings import get_default_gateway_ip, set_default_gateway_ip
from src.ui.pages.dashboard_page import DashboardPage
from src.ui.pages.dryer_detail_page import DryerDetailPage
from src.ui.pages.flash_node_page import FlashNodePage
from src.ui.pages.mesh_page import MeshPage
from src.ui.pages.node_detail_page import NodeDetailPage
from src.ui.pages.polymerpak_detail_page import PolymerPakDetailPage
from src.ui.pages.rtsnow_page import RtsNowPage

POLL_INTERVAL_MS = 2000
VERIFY_TIMEOUT_MS = 3000
# The gateway's own do_setting_send already resends 3x, but all 3 land
# within ~80ms of each other (see gateway-firmware/src/main.cpp's
# kRemoteControlResendGapMs) - fine for a strong link, not enough time-
# diversity to survive a longer fade on a weaker one. Confirmed on real
# hardware: a node with a noticeably weaker ESP-NOW link than its sibling
# dropped roughly 2 of 5 single-burst rename attempts outright. Resending
# the whole command again here, spaced RENAME_RETRY_INTERVAL_MS apart,
# gives each attempt its own independent burst at a different moment -
# much likelier to land on at least one of RENAME_MAX_ATTEMPTS tries.
RENAME_RETRY_INTERVAL_MS = 1500
RENAME_MAX_ATTEMPTS = 4
RTSNOW_CHANNEL_MIN = 1
RTSNOW_CHANNEL_MAX = 11
# How long to wait between automatic reconnect attempts to the default
# gateway IP after it drops (see _on_client_disconnected's own comment) -
# cheap enough to retry indefinitely in the background (a failed
# socket.create_connection() to an unreachable host returns in well under
# this) without spamming the log every single attempt.
AUTO_RECONNECT_INTERVAL_MS = 5000

# main_window.py -> src/ui/ -> assets/RTSLOGO.png (Ristola Technical
# Services' own logo, copied in from the sibling SPI-IM project's Assets/
# folder rather than referenced across repos - see this app's own
# assets/ directory, not a path into another project).
_LOGO_PATH = Path(__file__).resolve().parent / "assets" / "RTSLOGO.png"


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("RTS ESP-NOW Gateway")
        self.resize(1200, 750)

        self.client: GatewayClient | NetworkGatewayClient | None = None
        self._connected_port: str | None = None
        # Set while an automatic startup attempt at the remembered default
        # gateway IP is in flight (see _try_default_gateway_on_startup) -
        # distinguishes "no gateway there yet, quietly fall back to manual
        # USB" from a real error worth a dialog, which every other
        # verify-timeout/error path below still wants.
        self._connecting_via_network = False
        # The host from the most recent _connect_network() call, kept set
        # across a verify failure/success and cleared only on a manual
        # Disconnect or a manual USB Connect (see _disconnect()/
        # _on_connect_clicked()) - this is what lets _on_client_disconnected
        # tell "a network connection dropped on its own, keep retrying it"
        # apart from "the user just clicked Disconnect", since
        # NetworkGatewayClient's own `disconnected` signal fires identically
        # either way (see network_client.py's run()).
        self._network_host: str | None = None
        # Set right before self.client.stop() in _disconnect() so the next
        # _on_client_disconnected knows this exact drop was requested, not
        # a real connection loss - see that method's own comment.
        self._manual_disconnect_requested = False
        # True once this session has already logged "reconnecting..." for
        # the current unbroken run of auto-retries, so a gateway that stays
        # down for a while doesn't get a fresh log line every single
        # AUTO_RECONNECT_INTERVAL_MS tick - only the first drop and the
        # eventual reconnect are worth a line.
        self._auto_reconnect_logged = False
        self.known_devices: dict[str, KnownDevice] = {}
        self._pending_acks: dict[int, str] = {}
        self._ota_client: OtaPushClient | None = None
        self._ota_target_page = None  # whichever page's ota_requested started the in-flight push
        self._verified = False  # confirmed the connected port is actually an RTS ESP-NOW Gateway
        self._node_flasher: NodeFlasherClient | None = None
        self._pio_build_client: PioBuildClient | None = None
        self._espnow_ota_transfer: EspNowOtaTransfer | None = None
        self._mac_readers: list[ChipMacReader] = []
        self._scan_results: list[dict] = []
        self._scan_pending_ports: list[tuple[str, str]] = []  # (port, description) still to read
        self._pending_renames: dict[str, dict] = {}  # mac -> {"timer": retry QTimer, "attempts": int}

        # equipmentType/model for RTSNow-project devices, fetched once per
        # mac from the node's own /api/data (see DeviceInfoFetcher) -
        # feeds both the RTSNow page's icon grid and _show_device_page()'s
        # routing decision. Not part of KnownDevice/known_devices itself:
        # that comes from ESP-NOW's device identity broadcast, which has
        # no notion of equipmentType/model at all (see PROTOCOL.md).
        self._device_info: dict[str, dict] = {}
        self._info_fetchers: dict[str, DeviceInfoFetcher] = {}  # mac -> in-flight fetch
        # Fallback for a device with no IP at all (e.g. Dryer SHED's
        # persistently weak link) where DeviceInfoFetcher's direct HTTP
        # fetch can never succeed - asks over the ESP-NOW mesh instead
        # (poll_registers, already relay-capable) and derives equipmentType/
        # model from the resulting register_values once it arrives (see
        # _ensure_device_info/_on_event's own "register_values" handling).
        # mac -> last-requested monotonic time, so this doesn't re-send
        # poll_registers every single known_devices snapshot for a node
        # that just isn't answering right now.
        self._mesh_info_requested_at: dict[str, float] = {}

        self._poll_timer = QTimer(self)
        self._poll_timer.timeout.connect(self._poll_gateway)

        self._verify_timer = QTimer(self)
        self._verify_timer.setSingleShot(True)
        self._verify_timer.timeout.connect(self._on_verify_timeout)

        self._auto_reconnect_timer = QTimer(self)
        self._auto_reconnect_timer.setSingleShot(True)
        self._auto_reconnect_timer.timeout.connect(self._on_auto_reconnect_timeout)

        self._build_ui()
        self._refresh_ports()
        self._set_connected_ui(False)
        self._try_default_gateway_on_startup()

    # ---- UI construction ----

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        root.addLayout(self._build_connection_bar())

        body_splitter = QSplitter(Qt.Orientation.Vertical)

        top_splitter = QSplitter(Qt.Orientation.Horizontal)
        sidebar = self._build_sidebar()
        pages = self._build_pages()
        top_splitter.addWidget(sidebar)
        top_splitter.addWidget(pages)
        top_splitter.setStretchFactor(0, 0)
        top_splitter.setStretchFactor(1, 1)
        body_splitter.addWidget(top_splitter)

        # Wire up navigation only once both the sidebar and the pages it
        # controls exist - connecting/selecting earlier would fire
        # _on_sidebar_item_changed before self.pages exists. self.sidebar is
        # the actual QListWidget - _build_sidebar() returns a container
        # widget (logo + the list) for layout purposes only.
        self.sidebar.currentItemChanged.connect(self._on_sidebar_item_changed)
        self.sidebar.setCurrentRow(0)

        body_splitter.addWidget(self._build_log_panel())
        body_splitter.setStretchFactor(0, 3)
        body_splitter.setStretchFactor(1, 1)
        root.addWidget(body_splitter, stretch=1)

        self.statusBar().showMessage("Not connected")

    def _build_connection_bar(self) -> QHBoxLayout:
        bar = QHBoxLayout()

        bar.addWidget(QLabel("Port:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(280)
        bar.addWidget(self.port_combo)

        refresh_ports_btn = QPushButton("Refresh Ports")
        refresh_ports_btn.clicked.connect(self._refresh_ports)
        bar.addWidget(refresh_ports_btn)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self._on_connect_clicked)
        bar.addWidget(self.connect_btn)

        # Separate from Connect (which always means "USB, from the Port
        # dropdown above") - this is the only way to get back to the
        # network gateway after it drops without restarting the whole app,
        # since the auto-retry loop below only covers a drop that happens
        # while nothing else has been manually requested in the meantime.
        # Enabled/disabled alongside connect_btn in _set_connected_ui().
        self.reconnect_gateway_btn = QPushButton("Reconnect to Gateway")
        self.reconnect_gateway_btn.clicked.connect(self._on_reconnect_gateway_clicked)
        bar.addWidget(self.reconnect_gateway_btn)

        bar.addSpacing(24)
        bar.addWidget(QLabel("Channel:"))
        self.channel_spin = QSpinBox()
        self.channel_spin.setRange(RTSNOW_CHANNEL_MIN, RTSNOW_CHANNEL_MAX)
        bar.addWidget(self.channel_spin)
        self.apply_channel_btn = QPushButton("Apply")
        self.apply_channel_btn.clicked.connect(self._on_apply_channel)
        bar.addWidget(self.apply_channel_btn)

        bar.addSpacing(24)
        self.discover_btn = QPushButton("Discover Now")
        self.discover_btn.clicked.connect(self._on_discover_clicked)
        bar.addWidget(self.discover_btn)

        bar.addStretch(1)
        return bar

    # Sidebar items carry a (role, payload) tuple in Qt.ItemDataRole.UserRole
    # instead of relying on row index == page index - each project
    # header's per-device children (see _rebuild_children_for_project)
    # are inserted/removed dynamically as known_devices updates, so a fixed index
    # mapping would break the moment the device count changed.
    _RTSNOW_CHILD_INDENT = "   "  # 3 spaces, per request: "tab under RTSNow by 2-3 spaces"

    def _add_nav_item(self, sidebar: QListWidget, label: str, role: tuple, row: int | None = None) -> QListWidgetItem:
        item = QListWidgetItem(label)
        item.setData(Qt.ItemDataRole.UserRole, role)
        if row is None:
            sidebar.addItem(item)
        else:
            sidebar.insertItem(row, item)
        return item

    def _build_sidebar(self) -> QWidget:
        container = QWidget()
        container.setMaximumWidth(200)
        layout = QVBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)

        branding = QWidget()
        branding_layout = QVBoxLayout(branding)
        branding_layout.setContentsMargins(8, 12, 8, 12)

        logo_label = QLabel()
        pixmap = QPixmap(str(_LOGO_PATH))
        if not pixmap.isNull():
            logo_label.setPixmap(
                pixmap.scaledToWidth(160, Qt.TransformationMode.SmoothTransformation)
            )
        logo_label.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        branding_layout.addWidget(logo_label)

        layout.addWidget(branding)

        sidebar = QListWidget()
        # Qt's default palette renders a selected-but-unfocused row in a
        # dimmer gray (the "inactive" selection color) vs. the brighter
        # blue "active" one - noticeable specifically because
        # _select_sidebar_device_row() selects a row programmatically
        # without also moving keyboard focus into the sidebar, so a
        # device reached via the RTSNow page/Dashboard table showed gray
        # instead of the blue a direct sidebar click gets. Forcing
        # :selected to the same blue regardless of the (separate,
        # unrelated to "what page is this") focus state keeps the
        # highlight consistent no matter how a page was reached.
        sidebar.setStyleSheet(
            "QListWidget::item:selected { background: #2f6fed; color: white; }"
            "QListWidget::item:selected:!active { background: #2f6fed; color: white; }"
        )
        self._add_nav_item(sidebar, "Hardware List", ("dashboard", None))
        self._add_nav_item(sidebar, self._RTSNOW_CHILD_INDENT + "Flash Node", ("flash_node", None))
        self._add_nav_item(sidebar, self._RTSNOW_CHILD_INDENT + "Mesh", ("mesh", None))
        separator = self._add_nav_item(sidebar, "", ("separator", None))
        separator.setFlags(Qt.ItemFlag.NoItemFlags)  # blank spacer row - not selectable/clickable
        self._add_nav_item(sidebar, "RTSNow", ("rtsnow", None))
        # Each project's per-device children are inserted here dynamically
        # - see _rebuild_children_for_project(), called every time a
        # known_devices snapshot arrives. There's no standalone "Node
        # Detail" item any more - it's only ever reached by clicking a
        # device (one of these children, or a row in the Dashboard's
        # known-devices table).
        pp_separator = self._add_nav_item(sidebar, "", ("separator", None))
        pp_separator.setFlags(Qt.ItemFlag.NoItemFlags)  # blank spacer row - not selectable/clickable
        self._add_nav_item(sidebar, "PolymerPak", ("polymerpak", None))
        self.sidebar = sidebar
        layout.addWidget(sidebar, stretch=1)

        return container

    # Every project section's device children share this same "device"
    # role (see _on_sidebar_item_changed) - a rebuild identifies which
    # rows belong to *its* project by looking up known_devices for each
    # existing "device" row rather than a per-project role string, so
    # rebuilding one project's section never disturbs another's.
    def _rebuild_all_project_children(self):
        self._rebuild_children_for_project("rtsnow", "RTSNow")
        self._rebuild_children_for_project("polymerpak", "PolymerPak")

    def _rebuild_children_for_project(self, header_role: str, project_name: str):
        """Keeps one sidebar child per known device belonging to
        `project_name`, indented under the sidebar item tagged
        `(header_role, None)` (see _build_sidebar) - clicking one jumps
        straight to that device's Node Detail page.

        Skips the teardown/rebuild entirely when the desired (mac, label)
        sequence already matches what's on the sidebar - this runs on
        every known_devices snapshot (every POLL_INTERVAL_MS), and the
        device list is unchanged on almost all of those. Removing the
        currently-selected item from a QListWidget (even to immediately
        reinsert an equivalent one) can make Qt briefly auto-select
        whatever slides into that row before this function's own
        reselection logic runs, firing an extra currentItemChanged for
        that wrong intermediate item - visible as the Node Detail page
        (and whatever it was showing, e.g. just-polled register values)
        randomly flashing away mid-view. Rebuilding only when something
        actually changed avoids that almost entirely."""
        show_online_only = self.node_network_page.is_online_only()
        devices = sorted(
            (
                dev for dev in self.known_devices.values()
                if dev.project_name == project_name
                and (not show_online_only or self.node_network_page.is_online(dev))
            ),
            key=lambda dev: dev.friendly_name.lower(),
        )
        desired = [(dev.mac, self._RTSNOW_CHILD_INDENT + dev.friendly_name) for dev in devices]

        def existing_rows():
            rows = []
            for row in range(self.sidebar.count()):
                role, payload = self.sidebar.item(row).data(Qt.ItemDataRole.UserRole)
                if role != "device":
                    continue
                dev = self.known_devices.get(payload)
                if dev is not None and dev.project_name == project_name:
                    rows.append((row, payload, self.sidebar.item(row).text()))
            return rows

        existing = [(mac, label) for _row, mac, label in existing_rows()]
        if desired == existing:
            return

        current = self.sidebar.currentItem()
        current_role = current.data(Qt.ItemDataRole.UserRole) if current is not None else None

        for row, _mac, _label in reversed(existing_rows()):
            self.sidebar.takeItem(row)

        insert_at = None
        for row in range(self.sidebar.count()):
            role, _ = self.sidebar.item(row).data(Qt.ItemDataRole.UserRole)
            if role == header_role:
                insert_at = row + 1
                break
        if insert_at is None:
            return

        for dev in devices:
            self._add_nav_item(
                self.sidebar,
                self._RTSNOW_CHILD_INDENT + dev.friendly_name,
                ("device", dev.mac),
                row=insert_at,
            )
            insert_at += 1

        # Re-select the equivalent recreated child if that's what was
        # showing before this rebuild - the old QListWidgetItem was just
        # deleted above, so the selection would otherwise silently drop to
        # nothing every time a known_devices snapshot refreshes (every
        # POLL_INTERVAL_MS).
        if current_role is not None and current_role[0] == "device":
            mac = current_role[1]
            for row in range(self.sidebar.count()):
                role, payload = self.sidebar.item(row).data(Qt.ItemDataRole.UserRole)
                if role == "device" and payload == mac:
                    self.sidebar.setCurrentRow(row)
                    break

    def _build_pages(self) -> QStackedWidget:
        self.pages = QStackedWidget()

        self.dashboard_page = DashboardPage()
        self.pages.addWidget(self.dashboard_page)

        self.rtsnow_page = RtsNowPage()
        self.rtsnow_page.device_selected.connect(self._on_rtsnow_device_selected)
        self.pages.addWidget(self.rtsnow_page)

        # Node Network is no longer its own sidebar page - its tables/
        # actions live embedded in the Dashboard's Nodes section (see
        # DashboardPage). Keep this alias so every other call site below
        # that already refers to self.node_network_page doesn't need to
        # change.
        self.node_network_page = self.dashboard_page.node_network
        self.node_network_page.node_selected.connect(self._on_node_selected)
        self.node_network_page.online_only_toggled.connect(lambda _checked: self._rebuild_all_project_children())
        self.node_network_page.forget_requested.connect(self._on_forget_requested)
        self.node_network_page.set_default_gateway_requested.connect(self._on_set_default_gateway_requested)

        self.node_detail_page = NodeDetailPage()
        self.node_detail_page.rename_requested.connect(self._on_rename_requested)
        self.node_detail_page.ota_requested.connect(
            lambda ip, file_path: self._start_ota(ip, file_path, self.node_detail_page)
        )
        self.node_detail_page.reboot_requested.connect(self._on_reboot_requested)
        self.node_detail_page.poll_registers_requested.connect(self._on_poll_registers_requested)
        self.node_detail_page.config_setting_requested.connect(self._on_config_setting_requested)
        self.pages.addWidget(self.node_detail_page)

        self.dryer_detail_page = DryerDetailPage()
        # Reuses the exact same handlers NodeDetailPage's own Reboot/OTA
        # controls already wire up - neither cares which page asked, just
        # who to report progress/results back to (see _start_ota/
        # _on_ota_finished's _ota_target_page tracking). DryerDetailPage
        # no longer has a plain "navigate to NodeDetailPage" menu_
        # requested signal at all - Configuration, Remote Control
        # (reboot+OTA), and Registers all got their own popups directly
        # on that page, so there's nothing left NodeDetailPage offers for
        # a confirmed Dryer that isn't already duplicated there.
        self.dryer_detail_page.reboot_requested.connect(self._on_reboot_requested)
        self.dryer_detail_page.ota_requested.connect(
            lambda ip, file_path: self._start_ota(ip, file_path, self.dryer_detail_page)
        )
        self.dryer_detail_page.rename_requested.connect(self._on_rename_requested)
        self.dryer_detail_page.write_register_requested.connect(self._on_write_register_requested)
        self.pages.addWidget(self.dryer_detail_page)

        self.polymerpak_detail_page = PolymerPakDetailPage()
        # Same "reuses the generic handlers" reasoning as dryer_detail_page
        # above - poll_registers_requested/config_setting_requested are
        # still needed here (unlike DryerDetailPage), since this page's
        # Configuration box is live-editable settings, same mechanism as
        # NodeDetailPage's own SPI Address/Baud Rate.
        self.polymerpak_detail_page.reboot_requested.connect(self._on_reboot_requested)
        self.polymerpak_detail_page.ota_requested.connect(
            lambda ip, file_path: self._start_ota(ip, file_path, self.polymerpak_detail_page)
        )
        self.polymerpak_detail_page.rename_requested.connect(self._on_rename_requested)
        self.polymerpak_detail_page.poll_registers_requested.connect(self._on_poll_registers_requested)
        self.polymerpak_detail_page.config_setting_requested.connect(self._on_config_setting_requested)
        # Wrapped in a QScrollArea rather than added to the stack directly -
        # QStackedWidget/QStackedLayout's own sizeHint() is the LARGEST of
        # every page it holds, not just whichever is currently shown (a
        # documented Qt behavior, so switching pages never triggers a
        # window resize). This page's content (sun diagram + live status +
        # configuration + remote control + OTA, all stacked with no
        # scrolling of its own) is taller than Dryer OFFICE's own diagram
        # page - added directly, it silently inflated the WHOLE stack's
        # minimum height, pushing every other page's bottom controls off-
        # screen too. Confirmed live: Dryer OFFICE's own Alarm/Menus/trend
        # chart became unreachable the moment this page was added, despite
        # nothing on that page changing. A QScrollArea's own sizeHint
        # doesn't grow with its contents, so it no longer drags the shared
        # stack's minimum size along with it - this page scrolls
        # internally instead.
        self.polymerpak_scroll = QScrollArea()
        self.polymerpak_scroll.setWidget(self.polymerpak_detail_page)
        self.polymerpak_scroll.setWidgetResizable(True)
        self.pages.addWidget(self.polymerpak_scroll)

        self.flash_node_page = FlashNodePage()
        self.flash_node_page.scan_requested.connect(self._on_scan_nodes_requested)
        self.flash_node_page.flash_usb_requested.connect(self._on_flash_node_requested)
        self.flash_node_page.flash_mesh_requested.connect(self._on_espnow_flash_requested)
        self.flash_node_page.abort_mesh_requested.connect(self._on_espnow_flash_abort_requested)
        self.pages.addWidget(self.flash_node_page)

        self.mesh_page = MeshPage()
        self.pages.addWidget(self.mesh_page)

        return self.pages

    def _build_log_panel(self) -> QWidget:
        tabs = QTabWidget()

        self.activity_log = QPlainTextEdit()
        self.activity_log.setReadOnly(True)
        tabs.addTab(self.activity_log, "Activity Log")

        self.raw_log = QPlainTextEdit()
        self.raw_log.setReadOnly(True)
        self.raw_log.setStyleSheet("font-family: monospace;")
        tabs.addTab(self.raw_log, "Raw Serial")

        return tabs

    def _on_sidebar_item_changed(self, current: QListWidgetItem, previous: QListWidgetItem):
        if current is None:
            return
        role, payload = current.data(Qt.ItemDataRole.UserRole)
        if role == "dashboard":
            self.pages.setCurrentWidget(self.dashboard_page)
        elif role == "rtsnow":
            self.pages.setCurrentWidget(self.rtsnow_page)
        elif role == "flash_node":
            self.pages.setCurrentWidget(self.flash_node_page)
        elif role == "mesh":
            self.pages.setCurrentWidget(self.mesh_page)
        elif role == "device":
            dev = self.known_devices.get(payload)
            if dev is not None:
                self._show_device_page(dev)

    def _on_node_selected(self, mac: str):
        dev = self.known_devices.get(mac)
        if dev is None:
            return
        # Node Detail/Dryer Detail have no sidebar item of their own - this
        # is reached from the Dashboard's known-devices table or an
        # RTSNow page icon tile, neither of which is itself a sidebar row,
        # so the sidebar's own highlight would otherwise be left wherever
        # it last was (or nothing at all) instead of matching what's
        # actually on screen. _select_sidebar_device_row jumps the
        # highlight to that device's own child row, same one clicking it
        # directly in the sidebar would select.
        self._select_sidebar_device_row(mac)
        self._show_device_page(dev)

    def _on_rtsnow_device_selected(self, mac: str):
        self._on_node_selected(mac)

    def _select_sidebar_device_row(self, mac: str):
        """Highlights `mac`'s per-device sidebar child (under RTSNow or
        PolymerPak - see _rebuild_children_for_project), if it currently
        has one - a device hidden by "Show Online Only" won't. Signals are
        blocked around the actual selection change so this can't loop
        back into _on_sidebar_item_changed and re-run _show_device_page a
        second time for the same click."""
        for row in range(self.sidebar.count()):
            role, payload = self.sidebar.item(row).data(Qt.ItemDataRole.UserRole)
            if role == "device" and payload == mac:
                self.sidebar.blockSignals(True)
                self.sidebar.setCurrentRow(row)
                self.sidebar.blockSignals(False)
                return

    def _show_device_page(self, dev: KnownDevice):
        """Routes to the per-project/per-equipment-type detail page for
        `dev` - Dryer (DryerDetailPage) and PolymerPak (PolymerPakDetailPage)
        each have their own now; everything else (other equipment types,
        or a device whose type isn't known yet - see _device_info's
        comment) falls back to the generic NodeDetailPage, same page
        every RTSNow device used exclusively before either dedicated page
        existed. PolymerPak is routed by project_name directly rather
        than equipmentType (which only ever gets decoded from SPI-CCP's
        own Modbus model-type register - PolymerPak has no such concept
        at all, see its own fillRegisterBlock())."""
        equipment_type = self._device_info.get(dev.mac, {}).get("equipmentType")
        if equipment_type == "Dryer":
            self.dryer_detail_page.show_device(dev)
            self.pages.setCurrentWidget(self.dryer_detail_page)
        elif dev.project_name == "PolymerPak":
            self.polymerpak_detail_page.show_device(dev)
            self.pages.setCurrentWidget(self.polymerpak_scroll)
        else:
            self.node_detail_page.show_device(dev)
            self.pages.setCurrentWidget(self.node_detail_page)

    # ---- Connection management ----

    def _refresh_ports(self):
        self.port_combo.clear()
        for port in list_serial_ports():
            self.port_combo.addItem(port.label, userData=port.device)

    def _set_connected_ui(self, connected: bool):
        self.port_combo.setEnabled(not connected)
        self.connect_btn.setText("Disconnect" if connected else "Connect")
        self.reconnect_gateway_btn.setEnabled(not connected)
        self.apply_channel_btn.setEnabled(connected)
        self.discover_btn.setEnabled(connected)
        self.channel_spin.setEnabled(connected)
        self.node_network_page.set_enabled(connected)
        self.node_detail_page.set_enabled(connected)
        self.flash_node_page.set_enabled(connected)
        if not connected:
            self.dashboard_page.reset()
            # Not clearing _info_fetchers here - any still in flight are
            # one-shot and remove themselves from that dict via their own
            # finished signal once the thread has actually stopped (see
            # _ensure_device_info's comment - deliberately NOT tied to
            # info_received/info_error, which fire from inside run()
            # itself, before the OS thread has finished unwinding).
            # Dropping this dict's references before finished fires would
            # risk destroying a QThread object while it's still running -
            # undefined behavior in Qt (confirmed via a real crash report,
            # not just theoretical - same hazard DryerStatusPoller's
            # STOP_WAIT_MS comment is about), not just a leak.
            self._device_info = {}
            self._mesh_info_requested_at = {}
            self.rtsnow_page.update_devices([], {})

    def _on_connect_clicked(self):
        if self.client is not None:
            self._disconnect()
            return

        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "No port selected", "Select a serial port first.")
            return

        # An explicit choice to use USB instead - stop background network
        # auto-retries (see _on_client_disconnected's own comment) so they
        # don't steal this connection out from under the user the next time
        # the timer fires.
        self._auto_reconnect_timer.stop()
        self._network_host = None

        self._connecting_via_network = False
        self.client = GatewayClient(port)
        self.client.event_received.connect(self._on_event_received)
        self.client.raw_line.connect(self._on_raw_line)
        self.client.error.connect(self._on_client_error)
        self.client.disconnected.connect(self._on_client_disconnected)
        self.client.start()

        self._connected_port = port
        self._verified = False
        self._set_connected_ui(True)

        port_info = next((p for p in list_serial_ports() if p.device == port), None)
        self.dashboard_page.set_mac(port_info.serial_number if port_info else None)

        self.dashboard_page.set_connection_status(f"Connected to {port} - verifying it's a gateway...")
        self.statusBar().showMessage(f"Connected to {port} - verifying...")
        self._log(f"Connecting to {port}...")

        # Any well-formed JSON event proves this is really an RTS ESP-NOW
        # Gateway - node-firmware has no JSON emitter at all, so it can
        # never produce one, no matter what it's sent. `hello` doubles as
        # the verification probe and populates the Dashboard's gateway
        # info immediately, rather than only ever getting it if the app
        # happened to already be connected at the exact moment the gateway
        # itself booted (its only other source). Give up (auto-disconnect)
        # if nothing valid comes back in time - see _on_verify_timeout.
        # This is the real guard; the port dropdown is just a convenience,
        # not something to trust blindly (both boards enumerate
        # identically at the OS level - see TASKS.md).
        self.client.send_command({"cmd": "hello"})
        self._verify_timer.start(VERIFY_TIMEOUT_MS)
        self._poll_timer.start(POLL_INTERVAL_MS)

    def _try_default_gateway_on_startup(self):
        """Called once from __init__, after the UI is built but before the
        user has touched anything - if a "Set As Default Gateway" IP was
        ever remembered (see settings.py), try it automatically rather
        than making the user click Connect and pick a USB port every
        single launch. If nothing answers (or it later drops after
        connecting), this quietly keeps retrying in the background - see
        _on_client_disconnected's own comment - rather than requiring a
        manual Reconnect click or an app restart; manual USB selection via
        Connect still works exactly as before at any point and cancels the
        background retry (see _on_connect_clicked)."""
        default_ip = get_default_gateway_ip()
        if not default_ip:
            self.statusBar().showMessage("No default gateway set - select a USB port to connect manually")
            return
        self._log(f"Trying default gateway {default_ip}...")
        self._connect_network(default_ip)

    def _connect_network(self, host: str):
        self._connecting_via_network = True
        self._network_host = host
        self.client = NetworkGatewayClient(host)
        self.client.event_received.connect(self._on_event_received)
        self.client.raw_line.connect(self._on_raw_line)
        self.client.error.connect(self._on_client_error)
        self.client.disconnected.connect(self._on_client_disconnected)
        self.client.start()

        self._connected_port = host
        self._verified = False
        self._set_connected_ui(True)
        self.dashboard_page.set_mac(None)  # no OS-reported serial number over a network connection

        self.dashboard_page.set_connection_status(f"Connecting to {host} - verifying it's a gateway...")
        self.statusBar().showMessage(f"Connecting to {host}...")

        self.client.send_command({"cmd": "hello"})
        self._verify_timer.start(VERIFY_TIMEOUT_MS)
        self._poll_timer.start(POLL_INTERVAL_MS)

    def _disconnect(self):
        # Distinguishes a deliberate Disconnect click from the connection
        # just dropping on its own - NetworkGatewayClient's `disconnected`
        # signal fires identically either way (see network_client.py's
        # run(), the `finally` block emits it regardless of why the loop
        # exited), so this is the only way _on_client_disconnected can
        # tell them apart and knows NOT to auto-retry a connection the
        # user just asked to close.
        self._manual_disconnect_requested = True
        if self.client is not None:
            self.client.stop()
        self._poll_timer.stop()
        self._verify_timer.stop()
        self._auto_reconnect_timer.stop()

    def _on_client_disconnected(self):
        was_manual = self._manual_disconnect_requested
        self._manual_disconnect_requested = False
        was_connecting_via_network = self._connecting_via_network
        already_verified = self._verified
        network_host = self._network_host
        self.client = None
        self._connected_port = None
        self._verified = False
        self._connecting_via_network = False
        self._set_connected_ui(False)

        if was_manual:
            # The user asked for this - go fully idle, no retry loop.
            self._network_host = None
            self._auto_reconnect_timer.stop()
            self._auto_reconnect_logged = False
            self.statusBar().showMessage("Not connected")
            self._log("Disconnected.")
            return

        if network_host is not None:
            # A network connection (whether to the remembered default IP
            # at startup, via the Reconnect button, or a prior auto-retry)
            # ended on its own - the gateway rebooting (e.g. for an OTA
            # push), a brief Ethernet/WiFi hiccup, or it simply not being
            # up yet are all just as likely as it being gone for good, so
            # this keeps retrying the same host in the background rather
            # than requiring the user to notice and click Reconnect (or
            # worse, restart the whole app) every time. self._network_host
            # is deliberately left set here (not cleared) so the retry
            # below has somewhere to reconnect to - only a manual
            # Disconnect or Connect-via-USB (see _on_connect_clicked)
            # clears it.
            if not self._auto_reconnect_logged:
                self._auto_reconnect_logged = True
                if was_connecting_via_network and not already_verified:
                    self._log(f"No response from {network_host} yet - retrying in the background "
                              f"every {AUTO_RECONNECT_INTERVAL_MS // 1000}s (use a USB port to connect "
                              f"manually instead).")
                else:
                    self._log(f"Lost connection to {network_host} - retrying automatically every "
                              f"{AUTO_RECONNECT_INTERVAL_MS // 1000}s...")
            self.statusBar().showMessage(f"Disconnected from {network_host} - retrying...")
            self._auto_reconnect_timer.start(AUTO_RECONNECT_INTERVAL_MS)
            return

        self.statusBar().showMessage("Not connected")
        self._log("Disconnected.")

    def _on_auto_reconnect_timeout(self):
        if self.client is not None or self._network_host is None:
            return
        self._connect_network(self._network_host)

    def _on_reconnect_gateway_clicked(self):
        if self.client is not None:
            return
        default_ip = get_default_gateway_ip()
        if not default_ip:
            QMessageBox.warning(
                self, "No default gateway set",
                "No default gateway IP has been remembered yet.\n\n"
                "Right-click a discovered device in the Known Devices list "
                "and choose \"Set As Default Gateway\" first.",
            )
            return
        self._auto_reconnect_timer.stop()
        self._auto_reconnect_logged = False
        self._log(f"Reconnecting to {default_ip}...")
        self._connect_network(default_ip)

    def _on_client_error(self, message: str):
        self._log(f"ERROR: {message}")

    def _on_verify_timeout(self):
        if self.client is None or self._verified:
            return
        if self._connecting_via_network:
            self._log(f"No response from {self._connected_port} - not a reachable gateway.")
            self._disconnect()
            return
        port = self.port_combo.currentData()
        self._log(f"No response from {port} that looks like an RTS ESP-NOW Gateway - disconnecting.")
        # _disconnect() stops the client thread, which emits its own
        # `disconnected` signal (already wired to _on_client_disconnected)
        # once it actually exits - don't call that handler again here.
        self._disconnect()
        QMessageBox.warning(
            self, "Not a gateway",
            f"{port} didn't respond like an RTS ESP-NOW Gateway within "
            f"{VERIFY_TIMEOUT_MS // 1000}s (no JSON received).\n\n"
            "This is probably a node's port, not the gateway's - check "
            "Port and try again.",
        )

    # ---- Sending commands ----

    def _send(self, cmd: dict, description: str):
        if self.client is None:
            return
        request_id = self.client.send_command(cmd)
        self._pending_acks[request_id] = description
        self._log(f"-> {description}")

    def _poll_gateway(self):
        if self.client is None:
            return
        self.client.send_command({"cmd": "list"})

    def _on_apply_channel(self):
        value = self.channel_spin.value()
        self._send({"cmd": "channel", "value": value}, f"set channel to {value}")

    def _on_discover_clicked(self):
        self._send({"cmd": "discover"}, "discover")

    def _on_rename_requested(self, mac: str, new_name: str):
        # "friendlyName" is a reserved setting key node-firmware
        # specifically recognizes: it renames itself and persists the
        # name in NVS (see PROTOCOL.md's "Generic settings"). Just a
        # regular `setting` command on the wire - no new gateway command
        # needed for this.
        #
        # The command's own `ack` only means the gateway accepted and
        # forwarded it over ESP-NOW - NOT that the node actually received
        # or applied it (found this out the hard way: a node that had
        # gone silent still got an `ok:true` ack back, since the gateway
        # doesn't wait for the node's actual RTSNOW_SETTING_ACK before
        # answering the JSON command). The real confirmation is the
        # separate, unsolicited `setting_ack` event handled below.
        #
        # Resends the whole command up to RENAME_MAX_ATTEMPTS times,
        # RENAME_RETRY_INTERVAL_MS apart, rather than sending once and just
        # waiting - see that constant's comment for why a single attempt
        # (even with the gateway's own internal 3x resend) wasn't reliable
        # enough on a weaker link.
        old = self._pending_renames.pop(mac, None)
        if old is not None:
            old["timer"].stop()

        state = {"attempts": 0, "timer": QTimer(self)}

        def send_attempt():
            state["attempts"] += 1
            self._send(
                {"cmd": "setting", "mac": mac, "key": "friendlyName", "valueType": "string", "value": new_name},
                f"rename {mac} to \"{new_name}\" (attempt {state['attempts']}/{RENAME_MAX_ATTEMPTS})",
            )
            if state["attempts"] >= RENAME_MAX_ATTEMPTS:
                state["timer"].stop()
                self._on_rename_confirm_timeout(mac)

        state["timer"].timeout.connect(send_attempt)
        self._pending_renames[mac] = state
        send_attempt()
        state["timer"].start(RENAME_RETRY_INTERVAL_MS)

    def _on_rename_confirm_timeout(self, mac: str):
        self._pending_renames.pop(mac, None)
        self._log(f"No confirmation from {mac} for the rename - it may be offline.")
        status = "No confirmation received - node may be offline."
        if self.node_detail_page.current_mac() == mac:
            self.node_detail_page.set_rename_status(status)
        if self.dryer_detail_page.current_mac() == mac:
            self.dryer_detail_page.set_rename_status(status)
        if self.polymerpak_detail_page.current_mac() == mac:
            self.polymerpak_detail_page.set_rename_status(status)

    def _on_reboot_requested(self, mac: str):
        # Same caveat as rename above: this `ack` only confirms the gateway
        # sent the ESP-NOW packet, not that the node received or acted on
        # it. There's no reboot-specific confirmation event (see
        # PROTOCOL.md's "Remote control" section) - a node that actually
        # reboots will show back up via its own next device_announced/
        # heartbeat, same as any node coming back online.
        self._send({"cmd": "reboot", "mac": mac}, f"reboot {mac}")

    def _on_forget_requested(self, mac: str):
        # Unlike reboot/rename above, this is a gateway-local command - it
        # only clears the gateway's own known_devices entry, nothing is
        # sent over ESP-NOW to the node itself. It'll simply reappear here
        # on its next announce/heartbeat, same as gateway-firmware's own
        # `forget` console command says.
        self._send({"cmd": "forget", "mac": mac}, f"forget {mac}")
        # Drop it from the table immediately rather than waiting out the
        # up-to-POLL_INTERVAL_MS gap until the next scheduled snapshot, then
        # force an early re-poll so the gateway's authoritative state (the
        # device reappearing right away if it's still heartbeating) shows
        # up within one round trip instead of up to POLL_INTERVAL_MS later.
        self.known_devices.pop(mac, None)
        self.node_network_page.remove_device(mac)

    def _on_set_default_gateway_requested(self, ip: str):
        set_default_gateway_ip(ip)
        self._log(f"Default gateway set to {ip} - will be tried automatically on the next launch.")
        self._poll_gateway()

    def _on_poll_registers_requested(self, mac: str):
        self._send({"cmd": "poll_registers", "mac": mac}, f"poll_registers {mac}")

    def _on_write_register_requested(self, mac: str, reg: int, value: float):
        self._send({"cmd": "write_register", "mac": mac, "reg": reg, "value": value},
                   f"write_register {reg}={value} {mac}")

    # key -> RTSNOW_SettingPayload.valueType (see SPI-IM's onRemoteSetting()
    # in main.cpp/main_atom_node.cpp for the matching decode).
    _CONFIG_SETTING_TYPES = {
        "equipmentType": "string",
        "model": "string",
        "spiAddress": "int",
        "spiBaudRate": "int",
        # PolymerPak's solar-math settings (see that project's
        # TrackerSettings.h/main.cpp's onRemoteSetting()) - not SPI-IM's.
        "siteLatitude": "float",
        "siteLongitude": "float",
        "sunElevationDeg": "float",
        "trackerStepDeg": "float",
    }

    def _on_config_setting_requested(self, mac: str, key: str, value):
        value_type = self._CONFIG_SETTING_TYPES.get(key, "string")
        self._send(
            {"cmd": "setting", "mac": mac, "key": key, "valueType": value_type, "value": value},
            f"setting {mac} {key}={value}",
        )

    def _start_ota(self, ip: str, file_path: str, target_page):
        # target_page just needs set_ota_progress(int)/set_ota_result(bool,
        # str) - both NodeDetailPage and DryerDetailPage implement them
        # the same way, so this doesn't care which one's asking.
        if self._ota_client is not None:
            QMessageBox.warning(self, "OTA in progress", "Wait for the current OTA update to finish first.")
            return
        self._log(f"-> OTA push to {ip}: {file_path}")
        self._ota_target_page = target_page
        self._ota_client = OtaPushClient(ip, file_path)
        self._ota_client.progress.connect(target_page.set_ota_progress)
        self._ota_client.finished.connect(self._on_ota_finished)
        self._ota_client.start()

    def _on_ota_finished(self, success: bool, message: str):
        self._ota_target_page.set_ota_result(success, message)
        self._log(("OTA succeeded: " if success else "OTA FAILED: ") + message)
        self._ota_client = None
        self._ota_target_page = None

    # ---- Node onboarding (Flash Node page) ----

    def _on_scan_nodes_requested(self):
        if self._mac_readers:
            return  # a scan is already in progress
        self.flash_node_page.set_scanning(True)
        self._scan_results = []
        # Never touch the gateway's own port - esptool's MAC read resets
        # whatever board is on the other end, which would interrupt the
        # live gateway connection.
        self._scan_pending_ports = [
            (p.device, p.description) for p in list_serial_ports() if p.device != self._connected_port
        ]
        self._log(f"Scanning {len(self._scan_pending_ports)} port(s) for RTS-NOW nodes...")
        self._scan_next_port()

    def _scan_next_port(self):
        if not self._scan_pending_ports:
            self.flash_node_page.set_scanning(False)
            self.flash_node_page.update_rows(self._scan_results)
            self._log(f"Scan complete: {len(self._scan_results)} port(s) checked.")
            return

        port, description = self._scan_pending_ports.pop(0)
        self._scan_current_description = description
        reader = ChipMacReader(port)
        reader.result.connect(self._on_mac_read_result)
        self._mac_readers = [reader]
        reader.start()

    def _on_mac_read_result(self, port: str, mac):
        if mac is None:
            status = "unreadable"
        elif mac in self.known_devices:
            status = "known"
        else:
            status = "unprovisioned"
        self._scan_results.append(
            {"port": port, "description": self._scan_current_description, "mac": mac, "status": status}
        )
        self._mac_readers = []
        self._scan_next_port()

    def _on_flash_node_requested(self, port: str, project_dir: str, environment: str):
        if self._node_flasher is not None:
            QMessageBox.warning(self, "Flash in progress", "Wait for the current flash to finish first.")
            return
        self._log(f"-> Flashing {project_dir} to {port}")
        self._node_flasher = NodeFlasherClient(project_dir, port, environment or None)
        self._node_flasher.progress.connect(self.flash_node_page.set_usb_flash_progress)
        self._node_flasher.log_line.connect(self.flash_node_page.append_usb_log)
        self._node_flasher.finished.connect(self._on_node_flash_finished)
        self._node_flasher.start()

    def _on_node_flash_finished(self, success: bool, message: str):
        self.flash_node_page.set_usb_flash_result(success, message)
        self._log(("Node flash succeeded: " if success else "Node flash FAILED: ") + message)
        self._node_flasher = None

    def _on_espnow_flash_requested(self, mac: str, project_dir: str, environment: str):
        if self._pio_build_client is not None or self._espnow_ota_transfer is not None:
            QMessageBox.warning(self, "Flash in progress", "Wait for the current ESP-NOW flash to finish first.")
            return
        self._log(f"-> Building {project_dir} (env: {environment}) for ESP-NOW flash to {mac}")
        self._pio_build_client = PioBuildClient(project_dir, environment)
        self._pio_build_client.log_line.connect(self.flash_node_page.append_mesh_log)
        self._pio_build_client.finished.connect(
            lambda ok, message, bin_path: self._on_espnow_build_finished(mac, ok, message, bin_path)
        )
        self._pio_build_client.start()

    def _on_espnow_build_finished(self, mac: str, ok: bool, message: str, bin_path: str):
        self._pio_build_client = None
        if not ok:
            self.flash_node_page.set_mesh_result(False, message)
            return
        self.flash_node_page.append_mesh_log(f"Build succeeded: {bin_path}")
        self._espnow_ota_transfer = EspNowOtaTransfer(self._send, mac, bin_path)
        self._espnow_ota_transfer.progress.connect(self.flash_node_page.set_mesh_progress)
        self._espnow_ota_transfer.log.connect(self.flash_node_page.append_mesh_log)
        self._espnow_ota_transfer.finished.connect(self._on_espnow_ota_finished)
        self._espnow_ota_transfer.start()

    def _on_espnow_ota_finished(self, success: bool, message: str):
        self.flash_node_page.set_mesh_result(success, message)
        self._log(("ESP-NOW flash succeeded: " if success else "ESP-NOW flash FAILED: ") + message)
        self._espnow_ota_transfer = None

    def _on_espnow_flash_abort_requested(self):
        if self._espnow_ota_transfer is not None:
            self._espnow_ota_transfer.abort()
            self._espnow_ota_transfer = None

    # ---- Incoming events ----

    def _on_event_received(self, obj: dict):
        event = obj.get("event")

        if not self._verified:
            # Any well-formed JSON event at all proves this - see the
            # comment in _on_connect_clicked.
            self._verified = True
            self._verify_timer.stop()
            self._connecting_via_network = False
            # Reset so the NEXT drop (if any) logs its own "lost
            # connection.../retrying..." line instead of staying silent
            # because an earlier, now-resolved drop already set this.
            self._auto_reconnect_logged = False
            # self._connected_port, not the USB port combo's current
            # selection - the latter is meaningless (and possibly stale/
            # wrong) for a NetworkGatewayClient connection, which was
            # never chosen from that dropdown at all.
            self.dashboard_page.set_connection_status(f"Connected to {self._connected_port}")
            self.statusBar().showMessage(f"Connected to {self._connected_port}")
            self._log("Confirmed: this is an RTS ESP-NOW Gateway.")

        if event == "hello":
            channel = obj.get("channel", RTSNOW_CHANNEL_MIN)
            self.channel_spin.blockSignals(True)
            self.channel_spin.setValue(channel)
            self.channel_spin.blockSignals(False)
            self.dashboard_page.set_gateway_info(obj.get("deviceID", 0), channel, obj.get("protocolVersion", 0))
            self._log(
                f"Gateway ready: deviceID=0x{obj.get('deviceID', 0):08X} "
                f"channel={channel} protocolVersion={obj.get('protocolVersion')}"
            )
        elif event == "known_devices":
            self._update_known_devices(obj.get("devices", []))
        elif event == "ack":
            self._handle_ack(obj)
        elif event == "device_announced":
            dev = obj.get("device", {})
            self._log(f"New device announced: {dev.get('friendlyName')} ({dev.get('mac')})")
        elif event == "provision_request":
            req = obj.get("request", {})
            self._log(f"New provisioning request: {req.get('friendlyName')} ({req.get('mac')})")
            self._poll_gateway()
        elif event == "provision_expired":
            self._log(f"Provisioning request from {obj.get('mac')} expired.")
        elif event == "provision_confirmed":
            self._log(f"Provisioning confirmed: {obj.get('mac')} joined SSID \"{obj.get('ssid')}\".")
        elif event == "provision_confirm_timeout":
            self._log(f"No confirmation from {obj.get('mac')} after sending credentials for SSID \"{obj.get('ssid')}\".")
        elif event == "setting_ack":
            mac = obj.get("mac")
            key = obj.get("key")
            accepted = bool(obj.get("accepted"))
            status = "accepted" if accepted else "rejected"
            self._log(f"Setting \"{key}\" {status} by {mac}.")
            if key == "friendlyName" and mac in self._pending_renames:
                self._pending_renames.pop(mac)["timer"].stop()
                rename_status = "Confirmed by node." if accepted else "Node rejected the rename."
                if self.node_detail_page.current_mac() == mac:
                    self.node_detail_page.set_rename_status(rename_status)
                if self.dryer_detail_page.current_mac() == mac:
                    self.dryer_detail_page.set_rename_status(rename_status)
                if self.polymerpak_detail_page.current_mac() == mac:
                    self.polymerpak_detail_page.set_rename_status(rename_status)
            elif key in (
                "equipmentType", "model", "spiAddress", "spiBaudRate",
                "siteLatitude", "siteLongitude", "sunElevationDeg", "trackerStepDeg",
            ):
                if self.node_detail_page.current_mac() == mac:
                    self.node_detail_page.set_config_ack_status(key, accepted)
                if self.polymerpak_detail_page.current_mac() == mac:
                    self.polymerpak_detail_page.set_config_ack_status(key, accepted)
        elif event == "register_values":
            mac = obj.get("mac")
            values = obj.get("values", [])
            start_register = obj.get("startRegister", 0)
            self._log(f"Received {len(values)} register(s) from {mac} starting at {start_register}.")
            if self.node_detail_page.current_mac() == mac:
                self.node_detail_page.show_register_values(start_register, values)
            if self.dryer_detail_page.current_mac() == mac:
                self.dryer_detail_page.show_register_values(start_register, values)
            if self.polymerpak_detail_page.current_mac() == mac:
                self.polymerpak_detail_page.show_register_values(start_register, values)
            # Mesh fallback for a device with no IP (see _ensure_device_
            # info's own comment) - only fills in equipmentType/model if
            # nothing's cached yet, so a device that DOES have a direct-IP
            # answer (a richer /api/data response) never gets overwritten
            # by this narrower, register-only source.
            if mac is not None and mac not in self._device_info:
                model_type_index = MODBUS_REG_MODEL_TYPE - start_register
                if 0 <= model_type_index < len(values):
                    info = decode_model_type_register(values[model_type_index])
                    if info is not None:
                        self._device_info[mac] = info
                        rtsnow_devices = [dev for dev in self.known_devices.values() if dev.project_name == "RTSNow"]
                        self.rtsnow_page.update_devices(rtsnow_devices, self._device_info)
                        # Live-upgrade the currently open page if the user
                        # is already looking at this exact device - e.g.
                        # clicked SHED before this answer arrived, so it
                        # was still showing the generic Node Detail page.
                        dev = self.known_devices.get(mac)
                        if dev is not None and (
                            self.node_detail_page.current_mac() == mac
                            or self.dryer_detail_page.current_mac() == mac
                        ):
                            self._show_device_page(dev)
        elif event == "write_register_ack":
            mac = obj.get("mac")
            if self.dryer_detail_page.current_mac() == mac:
                self.dryer_detail_page.on_write_register_ack(
                    obj.get("reg", 0), bool(obj.get("ok")), obj.get("error", "")
                )
        elif event == "espnow_ota_start_ack":
            if self._espnow_ota_transfer is not None:
                self._espnow_ota_transfer.on_start_ack(obj.get("mac"), bool(obj.get("ok")), obj.get("message", ""))
        elif event == "espnow_ota_chunk_ack":
            if self._espnow_ota_transfer is not None:
                self._espnow_ota_transfer.on_chunk_ack(obj.get("mac"), obj.get("index", -1), bool(obj.get("ok")))
        elif event == "espnow_ota_end_ack":
            if self._espnow_ota_transfer is not None:
                self._espnow_ota_transfer.on_end_ack(obj.get("mac"), bool(obj.get("ok")), obj.get("message", ""))
        elif event == "channel_changed":
            channel = obj.get("channel", RTSNOW_CHANNEL_MIN)
            self.channel_spin.blockSignals(True)
            self.channel_spin.setValue(channel)
            self.channel_spin.blockSignals(False)
            self.dashboard_page.set_channel(channel)
            self._log(f"Gateway channel changed to {channel}.")
        else:
            self._log(f"Unhandled event: {obj}")

    def _handle_ack(self, obj: dict):
        request_id = obj.get("id")
        description = self._pending_acks.pop(request_id, obj.get("cmd", "?"))
        if obj.get("ok"):
            self._log(f"OK: {description}")
        else:
            self._log(f"FAILED: {description}: {obj.get('error')}")

    def _update_known_devices(self, devices_json: list[dict]):
        self.known_devices = {d["mac"]: KnownDevice.from_json(d) for d in devices_json}
        self.node_network_page.update_devices(devices_json)
        self.mesh_page.update_devices(list(self.known_devices.values()))
        self.flash_node_page.update_devices(list(self.known_devices.values()))
        self._rebuild_all_project_children()

        rtsnow_devices = [dev for dev in self.known_devices.values() if dev.project_name == "RTSNow"]
        for dev in rtsnow_devices:
            self._ensure_device_info(dev)
        self.rtsnow_page.update_devices(rtsnow_devices, self._device_info)

        current_mac = self.node_detail_page.current_mac()
        if current_mac and current_mac in self.known_devices:
            self.node_detail_page.refresh_if_current(self.known_devices[current_mac])
        current_dryer_mac = self.dryer_detail_page.current_mac()
        if current_dryer_mac and current_dryer_mac in self.known_devices:
            self.dryer_detail_page.refresh_if_current(self.known_devices[current_dryer_mac])
        current_polymerpak_mac = self.polymerpak_detail_page.current_mac()
        if current_polymerpak_mac and current_polymerpak_mac in self.known_devices:
            self.polymerpak_detail_page.refresh_if_current(self.known_devices[current_polymerpak_mac])

    # Only re-sends a mesh poll_registers fallback request for the same
    # mac this often - it's relayed over ESP-NOW and a persistently weak-
    # link node (the exact case this exists for) may just not answer for a
    # while, so resending every single known_devices snapshot would be
    # needless mesh chatter for no better odds of success.
    _MESH_INFO_RETRY_INTERVAL_S = 20.0

    def _ensure_device_info(self, dev: KnownDevice):
        """Kicks off a one-shot equipmentType/model fetch for `dev` if it
        doesn't have one cached (or in flight) yet - see _device_info's
        comment. Never retries on its own for the direct-IP path: a device
        that's unreachable right now (no ip yet, offline) just keeps the
        default "?" icon/NodeDetailPage routing until a later known_devices
        snapshot calls this again for it (every POLL_INTERVAL_MS) and it
        succeeds. A device with NO ip at all (e.g. Dryer SHED's
        persistently weak link - no WiFi association, so the direct HTTP
        path can never work regardless of retries) instead falls back to
        asking over the ESP-NOW mesh via poll_registers - see this file's
        own "register_values" handling for where that answer gets decoded
        back into equipmentType/model."""
        if dev.mac in self._device_info or dev.mac in self._info_fetchers:
            return
        if not dev.ip:
            last_requested = self._mesh_info_requested_at.get(dev.mac)
            if last_requested is not None and time.monotonic() - last_requested < self._MESH_INFO_RETRY_INTERVAL_S:
                return
            self._mesh_info_requested_at[dev.mac] = time.monotonic()
            self._send({"cmd": "poll_registers", "mac": dev.mac},
                       f"poll_registers {dev.mac} (equipment-type fallback, no IP)")
            return
        fetcher = DeviceInfoFetcher(dev.mac, dev.ip)
        fetcher.info_received.connect(self._on_device_info_received)
        fetcher.info_error.connect(self._on_device_info_error)
        # Cleanup (dropping _info_fetchers' reference, the only thing
        # keeping this QThread alive) deliberately happens on QThread's
        # own finished signal, not from info_received/info_error - those
        # fire from inside run() itself, before the OS thread has
        # actually finished unwinding, so popping the dict there raced
        # with the thread still being marked "running" and crashed
        # (confirmed via an actual macOS crash report: run() -> Python
        # deallocates this object -> ~QThread() fires while isRunning()
        # is still true -> Qt's qFatal() -> SIGABRT). finished only ever
        # fires after the thread has truly stopped, so this can't race.
        fetcher.finished.connect(lambda mac=dev.mac: self._info_fetchers.pop(mac, None))
        self._info_fetchers[dev.mac] = fetcher
        fetcher.start()

    def _on_device_info_received(self, mac: str, data: dict):
        # _info_fetchers' own cleanup happens on the fetcher's finished
        # signal, not here - see _ensure_device_info's comment.
        self._device_info[mac] = data
        rtsnow_devices = [dev for dev in self.known_devices.values() if dev.project_name == "RTSNow"]
        self.rtsnow_page.update_devices(rtsnow_devices, self._device_info)

    def _on_device_info_error(self, mac: str, _message: str):
        # Deliberately doesn't cache a failure into _device_info - leaves
        # this mac eligible for _ensure_device_info() to retry on the next
        # known_devices snapshot instead of getting stuck on the default
        # icon/routing forever after one bad request (e.g. the node was
        # mid-reboot when this fired). _info_fetchers' own cleanup
        # happens on the fetcher's finished signal, not here - see
        # _ensure_device_info's comment.
        pass

    def _on_raw_line(self, line: str):
        self.raw_log.appendPlainText(line)

    def _log(self, message: str):
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.activity_log.appendPlainText(f"[{timestamp}] {message}")

    def closeEvent(self, event):
        self._disconnect()
        # Stop any live-status pollers before exiting - see
        # NodeDetailPage.shutdown()'s comment (destroying a running
        # QThread is undefined behavior in Qt, not just a leak).
        self.node_detail_page.shutdown()
        self.dryer_detail_page.shutdown()
        self.polymerpak_detail_page.shutdown()
        super().closeEvent(event)
