from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QLabel,
    QLineEdit,
    QVBoxLayout,
)

# Matches RTSNOW_WifiCredentials (gateway-firmware/src/rtsnow_protocol.h) -
# the gateway rejects anything longer, so validate here too for immediate
# feedback instead of a round-trip error.
MAX_SSID_LEN = 32 - 1
MAX_PASSWORD_LEN = 64 - 1


class ProvisionDialog(QDialog):
    """Collects Wi-Fi SSID/password to send to one pending device.

    Unlike the gateway's human-typeable text command interface (`provision
    <mac> <ssid> <password>`, whitespace-tokenized), this dialog sends the
    credentials as JSON string fields - spaces are fine.
    """

    def __init__(self, device_label: str, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Provision Wi-Fi Credentials")

        self.ssid_edit = QLineEdit()
        self.ssid_edit.setMaxLength(MAX_SSID_LEN)
        self.password_edit = QLineEdit()
        self.password_edit.setMaxLength(MAX_PASSWORD_LEN)
        self.password_edit.setEchoMode(QLineEdit.EchoMode.Password)

        form = QFormLayout()
        form.addRow("SSID:", self.ssid_edit)
        form.addRow("Password:", self.password_edit)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)

        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(f"Provisioning: {device_label}"))
        layout.addLayout(form)
        layout.addWidget(buttons)

    def _on_accept(self):
        if not self.ssid_edit.text():
            self.ssid_edit.setFocus()
            return
        self.accept()

    def credentials(self) -> tuple[str, str]:
        return self.ssid_edit.text(), self.password_edit.text()
