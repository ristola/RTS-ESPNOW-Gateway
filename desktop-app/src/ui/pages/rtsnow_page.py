from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

# rtsnow_page.py -> pages/ -> ui/ -> assets/RTSLOGO.png
_LOGO_PATH = Path(__file__).resolve().parents[1] / "assets" / "RTSLOGO.png"


class RtsNowPage(QWidget):
    """Branding/info page shown when the sidebar's "RTSNow" header item
    itself is clicked - distinct from its per-project-device children
    (added dynamically by MainWindow), which navigate straight to Node
    Detail for that device instead of showing this page."""

    def __init__(self):
        super().__init__()
        layout = QVBoxLayout(self)
        layout.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter)
        layout.setContentsMargins(40, 40, 40, 40)
        layout.setSpacing(12)

        logo_label = QLabel()
        pixmap = QPixmap(str(_LOGO_PATH))
        if not pixmap.isNull():
            logo_label.setPixmap(pixmap.scaledToWidth(200, Qt.TransformationMode.SmoothTransformation))
        logo_label.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        layout.addWidget(logo_label)

        title = QLabel('<span style="color:#1b75bc;">RTS</span><span style="color:#ffffff;">Now</span>')
        title.setStyleSheet("font-size: 28pt; font-weight: bold;")
        title.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        layout.addWidget(title)

        description = QLabel(
            "A project-agnostic ESP-NOW gateway: discovers nodes from any RTS project, "
            "provisions Wi-Fi credentials over the air, and pushes generic settings and "
            "remote-control commands - all from this desktop app.\n\n"
            "See the sidebar's RTSNow entries below for every currently known RTSNow "
            "project device."
        )
        description.setWordWrap(True)
        description.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        description.setStyleSheet("color: gray;")
        description.setMaximumWidth(420)
        layout.addWidget(description)
