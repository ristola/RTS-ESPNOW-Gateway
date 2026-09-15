from PySide6.QtWidgets import QGroupBox, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from src.ui.pages.node_network_page import NodeNetworkPage


class DashboardPage(QWidget):
    """Read-only summary (connection state + gateway identity, merged into
    one "Gateway Connection" section) plus the live node network - known
    devices/pending requests tables and their actions (provision, reject,
    view detail) live directly in the Nodes section here now, rather than
    on their own separate sidebar page. See NodeNetworkPage for the actual
    table/action implementation - this page just embeds one instance of
    it."""

    def __init__(self):
        super().__init__()
        layout = QVBoxLayout(self)

        connection_box = QGroupBox("Gateway Connection")
        connection_row = QHBoxLayout(connection_box)
        self.connection_label = QLabel("Not connected")
        self.device_id_label = QLabel("-")
        self.mac_label = QLabel("-")
        self.channel_label = QLabel("-")
        self.protocol_label = QLabel("-")
        for caption, value_label in (
            ("Status:", self.connection_label),
            ("Device ID:", self.device_id_label),
            ("MAC:", self.mac_label),
            ("Channel:", self.channel_label),
            ("Protocol version:", self.protocol_label),
        ):
            connection_row.addWidget(QLabel(caption))
            connection_row.addWidget(value_label)
            connection_row.addSpacing(16)
        connection_row.addStretch(1)
        layout.addWidget(connection_box)

        nodes_box = QGroupBox("Nodes")
        nodes_layout = QVBoxLayout(nodes_box)
        self.node_network = NodeNetworkPage()
        nodes_layout.addWidget(self.node_network)
        layout.addWidget(nodes_box, stretch=1)

    def set_connection_status(self, text: str):
        self.connection_label.setText(text)

    def set_gateway_info(self, device_id: int, channel: int, protocol_version: int):
        self.device_id_label.setText(f"0x{device_id:08X}")
        self.channel_label.setText(str(channel))
        self.protocol_label.setText(str(protocol_version))

    def set_mac(self, mac: str | None):
        self.mac_label.setText(mac or "-")

    def set_channel(self, channel: int):
        self.channel_label.setText(str(channel))

    def reset(self):
        self.connection_label.setText("Not connected")
        self.device_id_label.setText("-")
        self.mac_label.setText("-")
        self.channel_label.setText("-")
        self.protocol_label.setText("-")
