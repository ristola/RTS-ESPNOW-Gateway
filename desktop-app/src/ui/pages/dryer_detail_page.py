import math
import time
from collections import deque

from PySide6.QtCore import Qt, QPointF, QTimer, Signal
from PySide6.QtGui import QColor, QLinearGradient, QPainter, QPainterPath, QPen, QRadialGradient
from PySide6.QtWidgets import (
    QAbstractItemView,
    QDialog,
    QFileDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSizePolicy,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from src.gateway.dryer_status_poller import DryerStatusPoller
from src.gateway.models import MODBUS_REG_MODEL_TYPE, KnownDevice, decode_model_type_register
from src.gateway.register_writer import RegisterWriter
from src.gateway.setting_writer import SettingWriter
from src.ui.pages.node_detail_page import BAUD_RATES, EQUIPMENT_TYPES, MODELS_BY_EQUIPMENT
from src.ui.pages.node_network_page import format_rssi

# Every register ever passed through EquipmentModel::setFloatRegister()
# (base class pollProcessSetpoint/pollProcessDelta/pollDewTrigger/
# pollProcessTemp/pollReturnTemp/pollDewPoint, plus each dryer model's own
# .cpp overrides for its extra temp channels - Regen/Aux/Bed temps etc.)
# gets marked signed_=true by firmware and stored as a raw two's-
# complement int16 bit pattern, e.g. -25 -> 0xFFE7 -> 65511 unsigned. The
# direct-IP /api/data path already reinterprets these correctly (see
# EquipmentModel::isSignedRegister()/DryerWebServer.cpp's own comment on
# it) since the node itself does the conversion - this project-side
# mirror is only needed for the mesh path (show_register_values() below),
# where the raw RTSNOW_RegisterBlock carries plain uint16 words with no
# signed-ness metadata at all. This set is every register number any
# model's own setFloatRegister() call site uses (grep the firmware repo
# for "setFloatRegister(" to re-verify) - deliberately matching firmware's
# per-register signed_ flag exactly rather than guessing from register
# names, since e.g. Process Setpoint (40010)/Process Delta (40011) go
# through the same signed path even though a human wouldn't necessarily
# guess a setpoint could ever be negative.
_SIGNED_MESH_REGISTERS = frozenset(
    {40010, 40011, 40012, 40015, 40016, 40017, 40018, 40019, 40020, 40021, 40022, 40023}
)


def _reinterpret_signed16(value: int) -> int:
    return value - 0x10000 if value > 0x7FFF else value


# Mirrors DryerWebServer.cpp's kDeviceRegNames (40001-40009, the XBEE
# SETUP block - identical across every model) - needed here because the
# mesh path (show_register_values() below) has no equivalent of the
# node's own direct-IP /api/data response, which already carries names
# per-register (see DryerWebServer.cpp's ActiveModel->registerName()/
# kDeviceRegNames). Without this, a mesh-only device's Registers dialog
# showed a completely blank Name column and a single uniform age for
# every row (the whole mesh snapshot's one receivedAt) - confirmed live,
# looking so different from a direct-IP device's own (named, individually-
# aged) Registers dialog that it read as two unrelated features rather
# than the same view of two different transports.
_XBEE_REGISTER_NAMES = {
    40001: "Software Version",
    40002: "SPI Station ID",
    40003: "SPI Baud Rate",
    40004: "Model Type",
    40005: "RSSI",
    40006: "Write Enable",
    40007: "Board Temp",
    40008: "Xbee Radio Voltage",
    40009: "SPI CRC Error",
}

# Mirrors EquipmentModel::registerName() (40010-40017) - the "STANDARD
# COMMON SPI DATA" block, worded identically across every dryer/
# crystallizer model.
_COMMON_REGISTER_NAMES = {
    40010: "Process Set Point",
    40011: "Process Limit Delta",
    40012: "Process Temp",
    40013: "Process Status",
    40014: "Machine Status",
    40015: "Return Temp",
    40016: "Dew Point",
    40017: "Dew Point Alarm Trigger",
}

# Mirrors each model's own registerName() override for its extended block
# (40018+) - see DryerFC.cpp/DryerFN.cpp/DryerADV.cpp/DryerFD.cpp. DryerCD
# and both Crystallizer models don't override registerName() at all (no
# entry here), so they fall back to _COMMON_REGISTER_NAMES only, same as
# the firmware itself does via EquipmentModel::registerName()'s default.
_MODEL_REGISTER_NAMES = {
    "FC": {40018: "Regen Temp", 40019: "Regen Out Temp", 40020: "Aux 1 Temp", 40021: "Aux 2 Temp"},
    "FN": {40018: "Regen Temp", 40019: "Regen Out Temp", 40020: "Aux 1 Temp", 40021: "Aux 2 Temp"},
    "ADV": {
        40018: "Left Bed Heater", 40019: "Left Bed Outlet",
        40020: "Right Bed Heater", 40021: "Right Bed Outlet",
        40022: "Process 2 Temp", 40023: "Return 2 Temp",
    },
    "FD": {
        40018: "Regen Temp", 40019: "Regen Out Temp",
        40020: "Dryer Inlet Temp", 40021: "Dryer Outlet Temp",
        40022: "Process 2 Temp", 40023: "Return 2 Temp",
        40024: "Inlet Temp", 40025: "Throat Temp",
        40026: "Left Bed Temp", 40027: "Right Bed Temp",
        40028: "Hopper 1 Temp", 40029: "Hopper 2 Temp", 40030: "Hopper 3 Temp",
        40031: "Hopper 4 Temp", 40032: "Hopper 5 Temp", 40033: "Hopper 6 Temp",
        40034: "Process Dewpoint", 40035: "Return Dewpoint",
    },
}


def _mesh_register_name(reg: int, model: str) -> str:
    if reg in _XBEE_REGISTER_NAMES:
        return _XBEE_REGISTER_NAMES[reg]
    if reg in _COMMON_REGISTER_NAMES:
        return _COMMON_REGISTER_NAMES[reg]
    return _MODEL_REGISTER_NAMES.get(model, {}).get(reg, "")


# Machine Status (40014) raw values -> operator-facing text. Only 0/1 are
# confirmed meanings so far (see DataSheets/SPI-CCP Notes - Dryer and
# Crystallizer Polls.md and this project's own real-hardware testing) -
# anything else falls back to showing the raw number rather than
# asserting a meaning that isn't confirmed yet, same caution this file's
# Process Status handling takes too (see _PROCESS_STATUS_ALARM_BITS).
_MACHINE_STATUS_TEXT = {
    "0": "Machine Stopped",
    "1": "Machine Running",
}

# Machine Status box background color - confirmed Running/Stopped get a
# real color (matches the reference photo's bright green "Machine
# Running" box); a toggle waiting on poll confirmation (see
# _on_machine_status_clicked) or a not-yet-confirmed "--" both get the
# same neutral gray, since neither is an actual known run state.
_MACHINE_STATUS_COLOR_RUNNING = "#4caf50"
_MACHINE_STATUS_COLOR_STOPPED = "#e53935"
_MACHINE_STATUS_COLOR_UNKNOWN = "#9e9e9e"

# Process Status (40013) bit -> alarm name, per the official SPI-CCP
# "SPI Process Status Poll" spec (CMD1=0x20, CMD2=0x40, 2-byte bitmapped
# data) the user supplied directly. Bit numbers here are positions in
# the assembled 16-bit register value, not the spec's own per-byte
# numbering - the spec's "Byte #0" occupies bits 0-7 (the LOW byte) and
# "Byte #1" occupies bits 8-15 (the HIGH byte) of that value.
#
# This is the OPPOSITE of an earlier, wrong first guess (Byte #0 = high
# byte, matching EquipmentModel::pollProcessStatus()'s wire-level
# bigEndianToU16(data) = (data[0]<<8)|data[1]) - that assumed the spec's
# byte numbering followed wire transmission order. A real value read
# from the dryer (270) proved it doesn't: under the wire-order guess, 270
# decoded to Low Flow Alarm=1 (spec says that bit "is not implemented,
# always 0") and a Reserved bit=1, while System Alarm read 0 despite an
# alarm apparently posted - internally inconsistent. Under this
# corrected LSB-first-numbering interpretation, 270 decodes cleanly to
# just Clogged Filter Alarm posted, correctly also tripping System/
# Process/Machine Alarm above it, with Low Flow and every Reserved bit
# staying 0 exactly as documented - the spec numbers bytes by
# significance within the resulting value, independent of the separate
# "transmitted MSB-first" wire-framing rule.
#
# Deliberately only the 4 bits that name a specific, standalone fault
# condition - excluded: Bit 0 "Processing" (an operating-state flag, not
# an alarm), Bits 1-3 "System/Process/Machine Alarm" (category-summary
# bits meaning "at least one alarm of this kind is posted", redundant
# once the specific alarm below is already listed), Bit 9 "Low Flow
# Alarm" (the spec itself says "not implemented, always 0", so it can
# never actually fire), and every Reserved bit.
_PROCESS_STATUS_ALARM_BITS = [
    (4, "High Temp Alarm"),        # spec Byte #0 bit 4 (process/regen heater overtemp)
    (5, "Low Temp Alarm"),         # spec Byte #0 bit 5 (process/regen heater undertemp)
    (8, "Clogged Filter Alarm"),   # spec Byte #1 bit 0
    (10, "High Dewpoint Alarm"),   # spec Byte #1 bit 2
]


class _Tile(QFrame):
    """One bordered label+value box - used for the top Setpoint/Temp
    readouts and the Additional Readings strip, echoing the boxed
    readouts on a real dryer HMI screen."""

    def __init__(self, label: str, value_pt: int = 20):
        super().__init__()
        self.setStyleSheet("QFrame { border: 1px solid #888; border-radius: 4px; background: #f5fbfb; }")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(2)

        name = QLabel(label)
        name.setStyleSheet("border: none; background: transparent; color: #333; font-size: 10pt; font-weight: bold;")
        layout.addWidget(name)

        self.value_label = QLabel("--")
        self.value_label.setStyleSheet(
            f"border: none; background: transparent; color: #111; font-size: {value_pt}pt; font-weight: bold;"
        )
        layout.addWidget(self.value_label)

    def set_value(self, text: str):
        self.value_label.setText(text)


class _ValueBox(QFrame):
    """Small plain white bordered box showing one value - optionally
    captioned with a small label above it, once a diagram readout has
    been mapped to a real register (see _BedClusterDiagram's box_128/
    box_481/box_556/box_outlet_r). Still-unmapped duct/sensor positions
    just pass no label and show a bare "--"."""

    def __init__(self, width: int = 55, height: int = 28, label: str | None = None):
        super().__init__()
        self.setStyleSheet("QFrame { background: white; border: 1px solid #333; }")
        _LABEL_STYLE = "background: transparent; border: none; color: #555; font-size: 8pt; font-weight: bold;"
        if label:
            # `width` is a floor, not a fixed value - a caption wider
            # than it (at this font) grows the box instead of getting
            # silently center-clipped on both ends by Qt (confirmed by
            # actually rendering this and looking: "Regen Outlet" at a
            # guessed-too-narrow width came out as "egen Outle[t]", not
            # a wrapped or elided line - QLabel doesn't clip its own
            # text, but the parent QFrame clips ITS children to its
            # fixedSize rect, so overflow just vanishes past the edge).
            probe = QLabel(label)
            probe.setStyleSheet(_LABEL_STYLE)
            width = max(width, probe.sizeHint().width() + 8)
        # Grows downward only (top-left position is unaffected) so a
        # caller that already positioned/connected-via-pipe an unlabeled
        # instance can add a label later without recalculating layout.
        self.setFixedSize(width, height + 16 if label else height)
        layout = QVBoxLayout(self) if label else QHBoxLayout(self)
        layout.setContentsMargins(4, 2, 4, 2)
        layout.setSpacing(0)
        if label:
            name = QLabel(label)
            name.setAlignment(Qt.AlignmentFlag.AlignCenter)
            name.setStyleSheet(_LABEL_STYLE)
            layout.addWidget(name)
        self.value_label = QLabel("--")
        self.value_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.value_label.setStyleSheet(
            "background: transparent; border: none; color: #111; font-size: 12pt; font-weight: bold;"
        )
        layout.addWidget(self.value_label)

    def set_value(self, text: str):
        self.value_label.setText(text)

    def set_alert_color(self, color: str | None):
        # None reverts to this class's own default white - only the
        # background changes, border/label styling stays whatever
        # __init__ already set (this overwrites the whole stylesheet
        # since Qt style sheets don't support patching a single
        # property, but the two properties set here are the only two
        # __init__ ever sets on this frame itself).
        self.setStyleSheet(f"QFrame {{ background: {color or 'white'}; border: 1px solid #333; }}")


class _MotorIcon(QWidget):
    """Blower motor - a housing with fan blades and a center hub, drawn
    rather than a plain gradient ball, so it actually reads as "blower"
    at a glance instead of an ambiguous green dot (no rotation
    animation)."""

    def __init__(self, size: int = 40):
        super().__init__()
        self.setFixedSize(size, size)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        cx, cy = w / 2, h / 2
        r = w / 2 - 2

        housing = QRadialGradient(cx - r * 0.3, cy - r * 0.3, r * 1.6)
        housing.setColorAt(0, QColor("#a5e8a5"))
        housing.setColorAt(0.55, QColor("#4caf50"))
        housing.setColorAt(1, QColor("#2e7d32"))
        painter.setPen(QPen(QColor("#1b5e20"), 2))
        painter.setBrush(housing)
        painter.drawEllipse(QPointF(cx, cy), r, r)

        # Four curved fan blades, pinwheel-style, in a darker green so
        # they read against the housing without needing their own outline.
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#256428"))
        blade_r = r * 0.72
        for i in range(4):
            a1 = i * math.pi / 2
            a2 = a1 + math.pi / 5
            mid = (a1 + a2) / 2
            blade = QPainterPath()
            blade.moveTo(cx, cy)
            blade.lineTo(cx + blade_r * math.cos(a1), cy + blade_r * math.sin(a1))
            blade.quadTo(
                cx + blade_r * 1.15 * math.cos(mid), cy + blade_r * 1.15 * math.sin(mid),
                cx + blade_r * math.cos(a2), cy + blade_r * math.sin(a2),
            )
            blade.closeSubpath()
            painter.drawPath(blade)

        hub_r = r * 0.3
        painter.setPen(QPen(QColor("#888"), 1))
        painter.setBrush(QColor("#eeeeee"))
        painter.drawEllipse(QPointF(cx, cy), hub_r, hub_r)
        painter.end()


class _Pipe(QFrame):
    """Thin gray connector segment between diagram elements - the
    mockup's ducting, simplified to straight horizontal/vertical runs
    rather than routed elbow graphics."""

    # A light-dark-light gradient across the pipe's thickness (not its
    # length) gives it a rounded/cylindrical look instead of a flat gray
    # bar - closer to the reference's shaded ducting.
    _GRADIENT_H = "qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #b5b5b5, stop:0.5 #7d7d7d, stop:1 #9c9c9c)"
    _GRADIENT_V = "qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #b5b5b5, stop:0.5 #7d7d7d, stop:1 #9c9c9c)"

    def __init__(self, orientation: str, length: int, thickness: int = 12):
        super().__init__()
        gradient = self._GRADIENT_H if orientation == "h" else self._GRADIENT_V
        self.setStyleSheet(f"background: {gradient}; border: 1px solid #555;")
        if orientation == "h":
            self.setFixedSize(length, thickness)
        else:
            self.setFixedSize(thickness, length)


class _Heater(QWidget):
    """Regeneration heater - a schematic coil icon (zigzag element inside
    a housing), not the reference photo's detailed finned heater
    rendering."""

    def __init__(self, size: tuple[int, int] = (46, 26)):
        super().__init__()
        self.setFixedSize(*size)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        painter.setPen(QPen(QColor("#555"), 1))
        painter.setBrush(QColor("#c9c9c9"))
        painter.drawRect(0, 0, w, h)

        cap_w = max(2, int(w * 0.06))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#222"))
        painter.drawRect(0, 0, cap_w, h)

        coil = QPainterPath()
        segments = 4
        step = w / segments
        coil.moveTo(2, h / 2)
        for i in range(segments):
            x = step * i + step / 2
            y = h * 0.22 if i % 2 == 0 else h * 0.78
            coil.lineTo(x, y)
        coil.lineTo(w - 2, h / 2)
        painter.setPen(QPen(QColor("#e08a3c"), 3))
        painter.drawPath(coil)
        painter.end()


class _Arrow(QWidget):
    """Bold triangle-headed arrow, drawn rather than a text glyph - a
    unicode arrow character renders thin and font-dependent at this
    size; this reliably matches the reference's thick solid arrows
    however the app is themed."""

    def __init__(self, direction: str, color: str, size: tuple[int, int] = (50, 90)):
        super().__init__()
        self._direction = direction  # "up" or "down"
        self._color = QColor(color)
        self.setFixedSize(*size)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        stem_w = w * 0.36
        head_w = w * 0.85
        head_h = h * 0.38
        path = QPainterPath()
        if self._direction == "down":
            path.addRect((w - stem_w) / 2, 0, stem_w, h - head_h)
            path.moveTo((w - head_w) / 2, h - head_h)
            path.lineTo((w + head_w) / 2, h - head_h)
            path.lineTo(w / 2, h)
            path.closeSubpath()
        else:
            path.addRect((w - stem_w) / 2, head_h, stem_w, h - head_h)
            path.moveTo((w - head_w) / 2, head_h)
            path.lineTo((w + head_w) / 2, head_h)
            path.lineTo(w / 2, 0)
            path.closeSubpath()
        painter.setBrush(self._color)
        painter.setPen(QPen(QColor("#1a1a1a"), 1.5))
        painter.drawPath(path.simplified())
        painter.end()


class _DamperIndicator(QLabel):
    """Small green circle with an X - the reference's damper-position
    icon in the pipe runs between/below the valves. Static; no register
    decodes an actual damper position yet, so this doesn't reflect live
    state."""

    def __init__(self, size: int = 22):
        super().__init__()
        self.setFixedSize(size, size)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setText("✕")
        self.setStyleSheet(
            f"background: #4caf50; border-radius: {size // 2}px; border: 2px solid #2e7d32; "
            "color: #16371a; font-weight: bold; font-size: 10pt;"
        )


class _FlowArrow(QLabel):
    """Small inline flow-direction arrow on a plain pipe stub - fine as a
    text glyph at this size (unlike the big directional arrows inside
    the beds, which need _Arrow's drawn shape instead)."""

    def __init__(self, char: str = "→"):
        super().__init__(char)
        self.setStyleSheet("background: transparent; border: none; color: #222; font-size: 12pt; font-weight: bold;")


class _Blower(QWidget):
    """Blower motor with its intake pipe/arrow - the reference's short
    pipe stub with a flow arrow feeding each motor from outside air.
    motor_size scales the fan icon (and this widget's own footprint with
    it) - the Process Blower is drawn bigger than the Regeneration
    Blower, per the mockup's own emphasis on it as the primary airflow
    source."""

    _INTAKE_W = 34  # arrow + spacing + intake pipe, ahead of the motor - fixed regardless of motor_size

    def __init__(self, motor_size: int = 40, with_intake: bool = True):
        super().__init__()
        self.motor_size = motor_size
        intake_w = self._INTAKE_W if with_intake else 0
        self.setFixedSize(intake_w + motor_size, motor_size)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)
        if with_intake:
            layout.addWidget(_FlowArrow("→"))
            layout.addWidget(_Pipe("h", 16))
        layout.addWidget(_MotorIcon(motor_size))


class _ValveJunction(QWidget):
    """Pipe cross-junction with a hexagonal valve body - the desiccant
    dryer's damper that switches airflow between the two beds on the
    dry/regen cycle. A compound shape (pipe stubs in a cross plus a
    hexagon over the crossing), so it's drawn rather than styled."""

    def __init__(self, size: int = 46):
        super().__init__()
        self.setFixedSize(size, size)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        cx, cy = w / 2, h / 2
        stub = w * 0.34

        # No pen/borders between the 4 stubs (or between them and the
        # hexagon drawn over them below) - a visible seam there made this
        # look like separate parts stuck together rather than one
        # fitting, and broke the illusion of a continuous pipe run
        # through the valve.
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#8a8a8a"))
        painter.drawRect(int(cx - w * 0.14), 0, int(w * 0.28), int(stub))  # top stub
        painter.drawRect(int(cx - w * 0.14), int(h - stub), int(w * 0.28), int(stub))  # bottom stub
        painter.drawRect(0, int(cy - h * 0.14), int(stub), int(h * 0.28))  # left stub
        painter.drawRect(int(w - stub), int(cy - h * 0.14), int(stub), int(h * 0.28))  # right stub

        r = w * 0.3
        hexagon = QPainterPath()
        for i in range(6):
            angle = math.pi / 6 + i * math.pi / 3
            x, y = cx + r * math.cos(angle), cy + r * math.sin(angle)
            hexagon.moveTo(x, y) if i == 0 else hexagon.lineTo(x, y)
        hexagon.closeSubpath()
        painter.setPen(QPen(QColor("#2a2a2a"), 1.5))
        painter.setBrush(QColor("#5a5a5a"))
        painter.drawPath(hexagon)
        painter.end()


class _DesiccantBed(QFrame):
    """Gray desiccant-bed box with a big directional arrow - matches the
    reference's L/R boxes. The arrow doesn't reflect live data yet - see
    DryerDetailPage's docstring on what's still unmapped."""

    def __init__(self, letter: str, direction: str, arrow_color: str):
        super().__init__()
        self.setStyleSheet("QFrame { background: #9a9a9a; border: 2px solid #4a4a4a; border-radius: 2px; }")
        self.setFixedSize(120, 175)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 6, 6, 6)

        layout.addStretch(1)
        layout.addWidget(_Arrow(direction, arrow_color), alignment=Qt.AlignmentFlag.AlignHCenter)
        layout.addStretch(1)

        letter_label = QLabel(letter)
        letter_label.setStyleSheet("background: transparent; border: none; color: white; font-size: 16pt; font-weight: bold;")
        layout.addWidget(letter_label)


class _PipeFitting(QWidget):
    """Small gray inlet/outlet pipe stub above/below the hopper -
    schematic, not the reference photo's detailed flanged valve assembly
    (this whole diagram is deliberately simplified shapes throughout, not
    an attempt at photorealism)."""

    def __init__(self, shape: str, size: tuple[int, int]):
        super().__init__()
        self._shape = shape  # "inlet" (funnel, wide top/narrow bottom) or "outlet" (plain stub)
        self.setFixedSize(*size)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#b7bdc4"))
        if self._shape == "inlet":
            path = QPainterPath()
            path.moveTo(0, 0)
            path.lineTo(w, 0)
            path.lineTo(w * 0.65, h)
            path.lineTo(w * 0.35, h)
            path.closeSubpath()
            painter.drawPath(path)
        else:
            painter.drawRect(0, 0, w, h)
        painter.end()


class _BedClusterDiagram(QWidget):
    """The process/regeneration air circuit between the two desiccant
    beds - Process Blower (top) and Regeneration Blower + Heater
    (bottom) piped into the valve stack, plus each bed's own outer vent.
    Built from absolutely-positioned child widgets with pipe segments
    hand-drawn between their exact edges, NOT a QGridLayout - a grid
    layout centers each small icon inside its (much larger) shared-size
    cell, which left visible gaps between every element instead of a
    connected-looking pipe network (confirmed against a reference photo:
    the grid version "doesn't even look close"). The value boxes are
    wired up by DryerDetailPage._build_diagram(): box_128 (Dryer Inlet/
    Aux 1 Temp), box_481 (Regeneration Outlet/Regen Out Temp), box_556
    (Regen Heater Temp/Regen Temp), box_outlet_r (Dryer Outlet/Aux 2
    Temp - the Right bed's own outlet pipe, mirroring box_481 on the
    Left bed's outlet), dew_box (Dew Point - stacked directly above
    box_outlet_r, not pipe-connected like the others since Dew Point
    isn't itself a duct reading at this location, just placed here per
    explicit request after moving out of _HopperAssembly)."""

    # Height trimmed from an original 470 - the Regen Blower/Heater
    # cluster moved up 70px (see the move() calls below) to shrink the
    # bare gap between it and the desiccant beds above it.
    _SIZE = (620, 400)

    def __init__(self):
        super().__init__()
        self.setFixedSize(*self._SIZE)

        self.blower_top = _Blower(motor_size=64, with_intake=False)
        self.blower_bottom = _Blower()
        self.heater = _Heater()
        self.bed_l = _DesiccantBed("L", "down", "#2e7d32")
        self.bed_r = _DesiccantBed("R", "up", "#c8681c")
        self.valve_upper = _ValveJunction()
        self.valve_lower = _ValveJunction()
        # Sits right after (below) the Upper 4-way valve, per the
        # reference photo - reconnected to valve_upper only (see
        # paintEvent), NOT down to valve_lower like an earlier pass had
        # it; that lower link was a real modeling error; the PCT itself
        # staying in place after the upper valve is not.
        self.damper_mid = _DamperIndicator()
        self.damper_bottom = _DamperIndicator()
        # All four share width=80 for a uniform look - wide enough for
        # each one's caption at 8pt (see _ValueBox) without colliding
        # with a neighboring pipe/arrow (box_481/box_outlet_r especially
        # - see their move() calls below).
        self.box_128 = _ValueBox(width=80, label="Dryer Inlet")
        self.box_481 = _ValueBox(width=80, label="Regen Outlet")
        self.box_556 = _ValueBox(width=80, label="Regen Heater")
        self.box_outlet_r = _ValueBox(width=80, label="Dryer Outlet")
        self.dew_box = _ValueBox(width=80, label="Dew Point")
        self.vent_arrow_l = _FlowArrow("←")
        self.vent_arrow_r = _FlowArrow("→")

        for child in (
            self.blower_top, self.blower_bottom, self.heater, self.bed_l, self.bed_r,
            self.valve_upper, self.valve_lower, self.damper_mid, self.damper_bottom,
            self.box_128, self.box_481, self.box_556, self.box_outlet_r, self.dew_box,
            self.vent_arrow_l, self.vent_arrow_r,
        ):
            child.setParent(self)

        # Centered above the valve stack (x=315, midway between the two
        # beds), not off to one side - a straight drop into the upper
        # valve rather than the side-fed elbow the smaller Regeneration
        # Blower still uses below.
        self.blower_top.move(283, 8)
        self.blower_bottom.move(55, 330)
        self.bed_l.move(160, 110)
        self.bed_r.move(350, 110)
        self.valve_upper.move(292, 137)
        self.valve_lower.move(292, 222)
        self.damper_mid.move(278, 191)
        self.damper_bottom.move(285, 335)
        self.heater.move(140, 337)
        # Level with the blower's own center, off its right side - not
        # branching off the drop pipe below it.
        self.box_128.move(360, 26)
        # Right edge fixed at 110 (the vent pipe's own left end, see
        # paintEvent's pipe(110, 202, 160, 202)), computed from the box's
        # own width rather than a hardcoded x - keeps the two in sync if
        # the width (or its font/caption) changes again later.
        self.box_481.move(110 - self.box_481.width(), 188)
        self.box_556.move(330, 322)
        self.vent_arrow_l.move(128, 190)
        self.vent_arrow_r.move(485, 190)
        # Mirrors box_481: left edge fixed flush against the Right bed's
        # own vent pipe (paintEvent's pipe(470, 202, 515, 202) ends at
        # 515), extending rightward into open canvas instead.
        self.box_outlet_r.move(515, 188)
        # Directly above box_outlet_r, same left edge - computed from
        # box_outlet_r's own height rather than a hardcoded gap, same
        # "derive from the other box's real geometry" reasoning as
        # box_481's x above.
        self.dew_box.move(515, 188 - self.dew_box.height() - 8)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)  # no seams where a pipe meets a valve's borderless stub

        def pipe(x1: int, y1: int, x2: int, y2: int, thickness: int = 12):
            half = thickness // 2
            horizontal = y1 == y2
            if horizontal:
                rect = (min(x1, x2) - half, y1 - half, abs(x2 - x1) + thickness, thickness)
                grad = QLinearGradient(0, rect[1], 0, rect[1] + rect[3])
            else:
                rect = (x1 - half, min(y1, y2) - half, thickness, abs(y2 - y1) + thickness)
                grad = QLinearGradient(rect[0], 0, rect[0] + rect[2], 0)
            grad.setColorAt(0, QColor("#b5b5b5"))
            grad.setColorAt(0.5, QColor("#7d7d7d"))
            grad.setColorAt(1, QColor("#9c9c9c"))
            painter.setBrush(grad)
            painter.drawRect(*rect)

        # Process Blower -> straight down -> upper valve
        pipe(315, 72, 315, 137)
        pipe(347, 40, 360, 40)  # "128" reading, off the blower's own right side at its center

        # Upper valve -> lower valve, straight through - per the reference
        # photo, the two valves ARE directly connected by one continuous
        # run, with the PCT (damper_mid) T-branching off it partway down
        # rather than sitting on its own dead-end stub.
        pipe(315, 183, 315, 222)
        pipe(300, 202, 315, 202)

        # Beds <-> valve stack (each bed's inner/valve-facing side)
        pipe(280, 160, 292, 160)
        pipe(338, 160, 350, 160)
        pipe(280, 245, 292, 245)
        pipe(338, 245, 350, 245)

        # Lower valve -> elbow -> heater -> Regeneration Blower
        pipe(315, 268, 315, 350)
        pipe(186, 350, 315, 350)
        pipe(133, 350, 140, 350)

        # Each bed's own outer vent - independent stubs per the reference
        # photo, not routed back to either valve (an earlier pass added a
        # "behind the bed" connection here that the photo doesn't show).
        pipe(110, 202, 160, 202)
        pipe(470, 202, 515, 202)

        painter.end()


class _Hopper(QFrame):
    """The material storage hopper - a silo-shaped orange body tapering
    to a funnel drain at the bottom. Process air connections live outside
    it (see _HopperAssembly's tiles, left of the hopper) rather than inside
    - the process air actually enters/exits through the hopper's left
    side per the real dryer, not through readouts floating inside the
    material itself. The shape is drawn in paintEvent (QSS can't do a
    non-rectangular fill) with no pen/stroke at all, and the frame itself
    is explicitly borderless/NoFrame - belt and suspenders against any
    stray default QFrame border showing up under a different Qt style/
    platform than this was last checked on."""

    _FUNNEL_TOP = 0.72  # fraction of height where the straight body ends and the funnel taper begins
    _SPOUT_W = 0.3  # fraction of width the funnel narrows to at the very bottom

    def __init__(self):
        super().__init__()
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet("QFrame { border: none; background: transparent; }")

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        funnel_top = h * self._FUNNEL_TOP
        spout_half_w = w * self._SPOUT_W / 2

        path = QPainterPath()
        path.moveTo(0, 0)
        path.lineTo(w, 0)
        path.lineTo(w, funnel_top)
        path.lineTo(w / 2 + spout_half_w, h)
        path.lineTo(w / 2 - spout_half_w, h)
        path.lineTo(0, funnel_top)
        path.closeSubpath()

        painter.setBrush(QColor("#e08a3c"))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawPath(path)
        painter.end()


class _HopperAssembly(QWidget):
    """The hopper and its two duct readouts (Process Air Return/Process
    Air Inlet), connected by hand-drawn pipe segments to exact widget
    edges - same rationale as _BedClusterDiagram's own docstring for the
    absolute positioning. This used to be a QVBoxLayout of stretch
    factors (see git history) chasing "how far is the gap to the hopper"
    through two separate regressions (a maximized window's leftover
    width landing on the tiles one pass, then somewhere else the next) -
    stretch factors can't promise an exact pixel gap because they're
    relative shares of whatever space is left over, which depends on
    window size and font metrics neither of which this diagram controls.
    Absolute positions computed once from each tile's own sizeHint()
    don't have that problem.

    Dew Point and Process Setpoint used to also live here (a 3rd/4th
    tile) - moved out per explicit request: Dew Point now sits over
    box_outlet_r in _BedClusterDiagram, and Process Setpoint moved into
    the Temperature Setpoints popup (see DryerDetailPage._on_temperature_
    settings_clicked) alongside the other editable setpoints, off the
    main screen entirely. Process Air Inlet took over Dew Point's old
    (centered-in-the-gap) position rather than keeping its own original
    near-the-hopper's-bottom spot."""

    def __init__(self):
        super().__init__()

        self.return_tile = _Tile("Process Air Return")
        self.inlet_tile = _Tile("Process Air Inlet")
        self.hopper = _Hopper()
        self.hopper.setFixedSize(150, 240)
        self.fitting_in = _PipeFitting("inlet", size=(50, 18))
        self.fitting_out = _PipeFitting("outlet", size=(24, 18))

        for child in (
            self.return_tile, self.inlet_tile,
            self.hopper, self.fitting_in, self.fitting_out,
        ):
            child.setParent(self)

        # Both sized to the same (widest, "Process Air Return")
        # width/height, not each one's own natural sizeHint - a uniform
        # column reads as one group, and it's what lets the pipe below
        # use a single x-coordinate for both tiles' right edges.
        tiles = (self.return_tile, self.inlet_tile)
        for t in tiles:
            t.adjustSize()
        tile_w = max(t.width() for t in tiles)
        tile_h = max(t.height() for t in tiles)
        for t in tiles:
            t.setFixedSize(tile_w, tile_h)

        pipe_len = 16
        hopper_x = tile_w + pipe_len
        hopper_y_span = 400  # matches _BedClusterDiagram's height - keeps the hopper itself aligned with that row
        hopper_y = (hopper_y_span - self.hopper.height()) // 2

        # Return near the hopper's own top edge - physically the air
        # returning to the desiccant beds for reconditioning exits high
        # (see the register-mapping comment in _build_diagram). Inlet
        # sits where Dew Point used to (centered between Return's bottom
        # edge and where Inlet's own original near-the-bottom slot was),
        # not its own original position - see this class's docstring.
        return_y = hopper_y + 12
        original_inlet_y = hopper_y + self.hopper.height() - tile_h - 12
        gap_top = return_y + tile_h
        gap_bottom = original_inlet_y
        inlet_y = gap_top + (gap_bottom - gap_top - tile_h) // 2

        self.return_tile.move(0, return_y)
        self.inlet_tile.move(0, inlet_y)
        self.hopper.move(hopper_x, hopper_y)
        self.fitting_in.move(hopper_x + (150 - self.fitting_in.width()) // 2, hopper_y - self.fitting_in.height())
        self.fitting_out.move(hopper_x + (150 - self.fitting_out.width()) // 2, hopper_y + self.hopper.height())

        self._tile_w = tile_w
        self._hopper_x = hopper_x
        self._pipe_ys = [return_y + tile_h // 2, inlet_y + tile_h // 2]

        self.setFixedSize(hopper_x + 150, hopper_y_span)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)

        thickness = 12
        half = thickness // 2
        for y in self._pipe_ys:
            rect = (self._tile_w, y - half, self._hopper_x - self._tile_w, thickness)
            grad = QLinearGradient(0, rect[1], 0, rect[1] + rect[3])
            grad.setColorAt(0, QColor("#b5b5b5"))
            grad.setColorAt(0.5, QColor("#7d7d7d"))
            grad.setColorAt(1, QColor("#9c9c9c"))
            painter.setBrush(grad)
            painter.drawRect(*rect)
        painter.end()


class _TrendChart(QFrame):
    """Small live multi-line trend chart, replacing the old static
    "Trend (not yet available)" placeholder - per the user's own
    reference photo (a black graph-paper grid with a few colored
    traces). Plots Process Temp/Process Setpoint/Dew Point
    (40012/40010/40016) as this page actually polls them, starting
    empty every time - there's no history logged to disk anywhere in
    this app, so this only ever shows what's been observed live since
    the page was opened, same honesty about "no fabricated data" as the
    rest of this page (see e.g. _PROCESS_STATUS_ALARM_BITS)."""

    # ~3 minutes of history at DryerStatusPoller's 3s interval - long
    # enough to see a real trend shape without the line getting
    # unreadably dense in a box this small.
    _MAX_SAMPLES = 60
    _SERIES = {
        "temp": QColor("#e53935"),      # red - Process Temp
        "setpoint": QColor("#8bc34a"),  # green - Process Setpoint
        "dew": QColor("#c9c93a"),       # yellow - Dew Point
    }

    def __init__(self):
        super().__init__()
        self.setStyleSheet("QFrame { background: black; border: 2px solid #222; }")
        self._points: dict[str, deque] = {key: deque(maxlen=self._MAX_SAMPLES) for key in self._SERIES}

    def add_sample(self, temp, setpoint, dew):
        for key, value in (("temp", temp), ("setpoint", setpoint), ("dew", dew)):
            if value is not None:
                self._points[key].append(value)
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()

        # Graph-paper grid, matching the reference photo's look.
        painter.setPen(QPen(QColor("#3a3a3a"), 1))
        cols, rows = 8, 4
        for i in range(1, cols):
            x = round(w * i / cols)
            painter.drawLine(x, 0, x, h)
        for i in range(1, rows):
            y = round(h * i / rows)
            painter.drawLine(0, y, w, y)

        # Each series auto-scaled independently to use the chart's full
        # height - this shows trend SHAPE over the last few minutes, not
        # absolute values (the tiles elsewhere on this page already show
        # the current live number for each of these).
        margin = 3
        for key, points in self._points.items():
            if len(points) < 2:
                continue
            lo, hi = min(points), max(points)
            span = (hi - lo) or 1
            denom = len(points) - 1
            path = QPainterPath()
            for i, value in enumerate(points):
                x = w * i / denom
                y = h - margin - ((value - lo) / span) * (h - 2 * margin)
                path.moveTo(x, y) if i == 0 else path.lineTo(x, y)
            painter.setPen(QPen(self._SERIES[key], 2))
            painter.drawPath(path)
        painter.end()


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

    This page replaces NodeDetailPage entirely for a confirmed Dryer -
    Equipment/Model/SPI Address/Baud Rate, Remote Control (reboot + OTA),
    and a live Registers table all got their own teal-HMI-styled popups
    here (see _on_configuration_menu/_on_remote_control_menu/_on_
    registers_menu). There's no plain "navigate to NodeDetailPage" menu
    item left at all - see MainWindow's own comment where this page is
    constructed for why.
    """

    reboot_requested = Signal(str)  # mac - "Reboot Node" chosen from the Remote Control popup
    ota_requested = Signal(str, str)  # ip, file_path - "Push Update" chosen from the same popup
    # mac, new_name - Name field edited in the Configuration popup. Goes
    # over RTS-NOW's generic settings mechanism (reserved "friendlyName"
    # key), same as NodeDetailPage's own rename field - there's no direct
    # HTTP endpoint for this in DryerWebServer.cpp (confirmed by reading
    # it: friendlyName() there is read-only, sourced from the same
    # RTS-NOW-set name), unlike Equipment/Model/SPI Address/Baud Rate.
    rename_requested = Signal(str, str)
    # mac, reg, value - a register write for a node with no IP at all
    # (RegisterWriter's direct http://<ip>/api/writeregister can't reach
    # it). Goes out as RTSNOW_WRITE_REGISTER over the mesh instead - see
    # main_window.py's connection to this and its "write_register_ack"
    # handling, which calls back into on_write_register_ack() below.
    write_register_requested = Signal(str, int, float)

    def __init__(self):
        super().__init__()
        self._mac: str | None = None
        self._ip: str | None = None
        self._poller: DryerStatusPoller | None = None
        # A register can now back more than one readout (e.g. 40012
        # Process Temp - both the top-bar tile and, physically, the
        # temperature of the process air entering the hopper's left side)
        # - a plain dict[int, widget] can only remember the last one
        # assigned to a given key, silently dropping updates to the
        # other. _Tile and _ValueBox both expose set_value(str), which is
        # all this needs from either.
        self._register_tiles: dict[int, list[_Tile | _ValueBox]] = {}
        self._last_process_status = "--"
        self._last_machine_status = "--"
        # "0" or "1" while a machine-status toggle (see
        # _on_machine_status_clicked) is waiting for a poll to confirm
        # it actually took - None the rest of the time. Read/cleared by
        # _on_status_received, which is what actually decides when to
        # stop showing "Stopping.../Starting..." and go back to the
        # normal Machine Stopped/Running text.
        self._machine_status_pending: str | None = None
        # True while a mesh-based clear-alarms write (reg 40014 = 3, see
        # _on_clear_alarms_clicked) is awaiting its write_register_ack -
        # both that and a normal start/stop toggle write the same
        # register, so on_write_register_ack() needs this to tell which
        # one a reg-40014 ack is actually confirming.
        self._mesh_clear_alarms_pending = False
        # Device-level settings (not SPI-CCP dryer registers, so not
        # part of _last_live) - top-level /api/data fields, read by
        # _on_configuration_menu's popup to show current values when
        # opened.
        self._last_equipment_type = "?"
        self._last_model = "?"
        self._last_spi_address = "?"
        self._last_spi_baud = "?"
        # OTA file picker state persists across popup open/close (matches
        # NodeDetailPage's own _ota_file_path) - the progress bar/status
        # label widgets are recreated fresh each time the Remote Control
        # popup opens (see _on_remote_control_menu) and referenced here
        # so set_ota_progress/set_ota_result (called by MainWindow from a
        # background OtaPushClient, possibly after the popup's been
        # closed) can reach them, guarded against the dialog having
        # already been destroyed.
        self._ota_file_path: str | None = None
        self._ota_progress_bar = None
        self._ota_status_label = None
        self._ota_push_btn = None
        # Guarded the same way as the OTA widgets above - set while the
        # Configuration popup is open, may outlive it if a rename
        # confirmation (or timeout) arrives after Back was clicked.
        self._rename_status_label = None
        # Snapshot of the last poll's live registers, keyed by reg - lets
        # the Temperature Setpoints dialog (see _on_temperature_settings_
        # clicked) read Process Limit Delta/Dew Point Trigger on demand
        # without needing dedicated always-updating tile widgets for
        # values that no longer have a spot on the main screen.
        self._last_live: dict[int, dict] = {}
        # Keeps each in-flight RegisterWriter (see _write_register)
        # referenced by mac until its own finished signal fires - the
        # exact DeviceInfoFetcher crash this project already hit once
        # (dropping a QThread's last reference from a signal that fires
        # from inside run(), before the OS thread has actually finished
        # unwinding, can destroy it while Qt still considers it running
        # and abort the whole app). Never touched from write_succeeded/
        # write_error, only from the writer's own finished signal.
        self._pending_writers: list[RegisterWriter] = []
        self._build_ui()
        self._show_placeholder()

    def _add_register_tile(self, reg: int, tile: "_Tile | _ValueBox"):
        self._register_tiles.setdefault(reg, []).append(tile)

    def _build_ui(self):
        layout = QVBoxLayout(self)

        self.placeholder_label = QLabel("Select a Dryer from the RTSNow page or sidebar to see its live status.")
        self.placeholder_label.setStyleSheet("color: gray;")
        layout.addWidget(self.placeholder_label)

        # Bezel frame around the whole live-status view, echoing the
        # mockup's thick dark surround on a real HMI touchscreen.
        self.content = QFrame()
        self.content.setStyleSheet("QFrame { background: #e9edf2; border: 4px solid #33415c; border-radius: 6px; }")
        content_layout = QVBoxLayout(self.content)
        content_layout.setContentsMargins(14, 14, 14, 14)

        # No standalone Process Temp tile up top anymore - register 40012
        # already shows on the diagram itself (Process Air Inlet, see
        # _build_diagram/_HopperAssembly), and a top-bar duplicate of the
        # same number was redundant.

        # Name/Model (upper-left) and Comms status (upper-right) are
        # overlaid directly on the diagram's own screen now, not a
        # separate row above it - see _build_diagram, which creates and
        # positions them (as children of that QFrame) rather than here.
        content_layout.addWidget(self._build_diagram())

        content_layout.addLayout(self._build_bottom_bar())
        layout.addWidget(self.content)

    def _build_diagram(self) -> QWidget:
        diagram = QFrame()
        diagram.setStyleSheet("QFrame { background: #79dde2; border: 2px solid #222; border-radius: 4px; }")
        diagram_layout = QHBoxLayout(diagram)
        diagram_layout.setContentsMargins(20, 20, 20, 20)
        diagram_layout.setSpacing(4)

        # Stretch on both sides of the row - lets the fixed-size content
        # below center itself horizontally once the frame is told to
        # expand (see setSizePolicy below) into whatever width the
        # window actually gives it, instead of leaving all the extra
        # space stranded on one side.
        diagram_layout.addStretch(1)

        bed_cluster = _BedClusterDiagram()
        self._add_register_tile(40020, bed_cluster.box_128)  # "Dryer Inlet" (Aux 1 Temp)
        self._add_register_tile(40019, bed_cluster.box_481)  # "Regeneration Outlet" (Regen Out Temp)
        self._add_register_tile(40018, bed_cluster.box_556)  # "Regen Heater Temp" (Regen Temp)
        self._add_register_tile(40021, bed_cluster.box_outlet_r)  # "Dryer Outlet" (Aux 2 Temp)
        self._add_register_tile(40016, bed_cluster.dew_box)  # "Dew Point" - moved here from _HopperAssembly
        # Kept as its own reference (not just reachable via
        # _register_tiles) so _on_status_received can set its background
        # color based on comparing it against Dew Point Trigger - a
        # comparison, not a value display, so it doesn't fit the generic
        # register->tile text update loop.
        self._dew_box = bed_cluster.dew_box
        diagram_layout.addWidget(bed_cluster, alignment=Qt.AlignmentFlag.AlignVCenter)

        # Process air connections into/out of the hopper's left side -
        # dry hot air enters low (Process Temp, 40012 - the same figure
        # shown in the top bar, since that register is literally the air
        # temperature going into the material) and exits high after
        # passing up through the material (Return Temp, 40015 - the air
        # returning to the desiccant beds for reconditioning). See
        # _HopperAssembly's own docstring for why this is absolutely
        # positioned rather than another QVBoxLayout of stretch factors.
        # Process Setpoint (40010) no longer has a tile here at all - see
        # _on_temperature_settings_clicked.
        hopper_assembly = _HopperAssembly()
        self._add_register_tile(40015, hopper_assembly.return_tile)
        self._add_register_tile(40012, hopper_assembly.inlet_tile)
        diagram_layout.addSpacing(12)
        diagram_layout.addWidget(hopper_assembly, alignment=Qt.AlignmentFlag.AlignVCenter)
        diagram_layout.addStretch(1)

        # Expanding, not fixed - the screen should fill whatever width/
        # height the window actually gives this page (confirmed as a
        # real gap by an actual screenshot: the bottom bar below already
        # stretches to the bezel's full width via its own stretch=1
        # items, but this frame was capped at its own fixed content size,
        # leaving a visible strip of the bezel's plain background beside
        # and below it). setMinimumSize keeps it from ever shrinking
        # below what bed_cluster/hopper_assembly actually need; the two
        # addStretch(1) calls above/around them center that fixed-size
        # content within whatever extra space Expanding pulls in.
        diagram.setMinimumSize(diagram.sizeHint())
        diagram.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        # The overlay labels are positioned against the frame's own
        # width/height (see _position_overlay_labels) - now that the frame
        # can actually resize, they need to be re-positioned whenever that
        # happens, not just whenever their own text changes.
        def _on_diagram_resize(event):
            self._position_overlay_labels()

        diagram.resizeEvent = _on_diagram_resize

        # Name/Model (upper-left) and Comms status (upper-right),
        # overlaid directly on the screen itself - matches a real dryer
        # HMI's own title bar, which lives on-screen, not in a separate
        # row of desktop-chrome above it.
        # "border: none" is required, not decorative - QLabel is itself a
        # QFrame subclass, so without it these three inherit the diagram
        # QFrame's own "border: 2px solid #222" rule (confirmed by
        # actually rendering this and seeing a stray box around "Dryer 1"
        # and "Comms OK" that nothing here asked for).
        self.name_label = QLabel(parent=diagram)
        self.name_label.setStyleSheet(
            "font-size: 16pt; font-weight: bold; color: #16324a; background: transparent; border: none;"
        )
        self.name_label.move(10, 6)

        self.model_label = QLabel(parent=diagram)
        self.model_label.setStyleSheet("color: #3a4f61; background: transparent; border: none;")
        self.model_label.move(10, 32)

        # Stacked WiFi/MESH RSSI readout, upper-right - replaces what used
        # to be a single "Comms OK"/"Comms lost"/"Comms unknown" label.
        # Deliberately sourced from the gateway's known-devices feed (see
        # show_device(), called on every ~2s known_devices refresh via
        # main_window's refresh_if_current), not from this page's own
        # direct-HTTP poller - that keeps showing a real, independently-
        # useful reading (is this node even reaching the gateway, and how
        # well) even during an HTTP outage, when the old text would have
        # just said "Comms lost" and nothing else.
        self.wifi_rssi_label = QLabel("WiFi: –", parent=diagram)
        self.wifi_rssi_label.setStyleSheet("color: #16324a; background: transparent; border: none;")
        self.mesh_rssi_label = QLabel("MESH: –", parent=diagram)
        self.mesh_rssi_label.setStyleSheet("color: #16324a; background: transparent; border: none;")

        # Node info, bottom-left - NodeType/MAC/Firmware/IP, refreshed
        # alongside the RSSI labels above from the same known_devices feed.
        self.node_info_label = QLabel("", parent=diagram)
        self.node_info_label.setStyleSheet("color: #16324a; background: transparent; border: none; font-size: 9pt;")

        self._diagram = diagram
        self._position_overlay_labels()

        return diagram

    def _position_overlay_labels(self):
        # Called after every setText() on any of these three (different
        # text means different widths/heights) and on every diagram
        # resize (see _on_diagram_resize) - name_label/model_label are
        # fixed-position (upper-left) and don't need this.
        self.wifi_rssi_label.adjustSize()
        self.wifi_rssi_label.move(self._diagram.width() - self.wifi_rssi_label.width() - 10, 4)
        self.mesh_rssi_label.adjustSize()
        self.mesh_rssi_label.move(
            self._diagram.width() - self.mesh_rssi_label.width() - 10,
            self.wifi_rssi_label.y() + self.wifi_rssi_label.height() + 2,
        )
        self.node_info_label.adjustSize()
        self.node_info_label.move(10, self._diagram.height() - self.node_info_label.height() - 6)

    def _build_bottom_bar(self) -> QHBoxLayout:
        bottom = QHBoxLayout()

        self.alarm_btn = QPushButton("ALARM")
        # Explicit color, not left to inherit from the system palette -
        # confirmed via a real screenshot under macOS Dark Mode that
        # inherited text color there is white, which on menu_btn's white
        # background rendered fully invisible ("MENUS" with no visible
        # text). Same fix applied to menu_btn and _on_menus_clicked's
        # popup below.
        self.alarm_btn.setStyleSheet(
            "background: #f4e04d; color: #111; border: 2px solid #222222; font-size: 16pt; font-weight: bold;"
        )
        self.alarm_btn.clicked.connect(self._on_alarm_clicked)

        self.trend_chart = _TrendChart()

        self.menu_btn = QPushButton("MENUS")
        self.menu_btn.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; font-size: 16pt; font-weight: bold;"
        )
        # A real full-screen MENUS grid (per the user-supplied reference
        # photo of an actual dryer HMI's menu screen), not a small native
        # QMenu dropdown - see _on_menus_clicked.
        self.menu_btn.clicked.connect(self._on_menus_clicked)

        # No caption label anymore, just the status text itself, centered
        # in the box - AlignCenter alone is enough since this QLabel is
        # the sole item in machine_status_box's layout, so it already
        # fills the whole box (see the uniform-height pass below).
        #
        # Also doubles as a toggle button (see _on_machine_status_
        # clicked) - clicking it sends the opposite of whatever's
        # currently confirmed (Running -> write 0, Stopped/unknown ->
        # write 1), same instance-level mousePressEvent-override pattern
        # as _make_editable_tile's tiles.
        self.machine_status_box = QFrame()
        self.machine_status_box.setStyleSheet(
            f"QFrame {{ background: {_MACHINE_STATUS_COLOR_UNKNOWN}; border: 2px solid #222; }}"
        )
        self.machine_status_box.setCursor(Qt.CursorShape.PointingHandCursor)
        self.machine_status_box.setToolTip("Click to start/stop the machine")
        self.machine_status_box.mousePressEvent = self._on_machine_status_clicked
        status_layout = QVBoxLayout(self.machine_status_box)
        status_layout.setContentsMargins(10, 8, 10, 8)
        self.machine_status_value = QLabel("--")
        self.machine_status_value.setAlignment(Qt.AlignmentFlag.AlignCenter)
        # 16pt, not the plain-digit tiles' 24pt - "Machine Stopped"/
        # "Machine Running" is a full word now, not a single character.
        self.machine_status_value.setStyleSheet(
            "border: none; background: transparent; font-size: 16pt; font-weight: bold; color: #111;"
        )
        status_layout.addWidget(self.machine_status_value)

        # Equal stretch alone only guarantees equal WIDTH - Alarm/Menu's
        # bigger font plus their old padding (removed above) made them
        # visibly taller than Trend/Machine Status. Measuring each one's
        # own natural sizeHint and locking all four to the tallest
        # (confirmed by actually rendering this - see scratchpad/)
        # guarantees they match on both axes, not just width.
        boxes = (self.alarm_btn, self.trend_chart, self.menu_btn, self.machine_status_box)
        common_h = max(b.sizeHint().height() for b in boxes)
        for b in boxes:
            b.setFixedHeight(common_h)
            bottom.addWidget(b, stretch=1)

        return bottom

    def _on_alarm_clicked(self):
        # Same teal HMI-screen look as _on_menus_clicked/_on_temperature_
        # settings_clicked's popups (title top-left, Exit bottom-right) -
        # per a real dryer HMI's own ALARMS screen (user-supplied
        # reference photo: an Alarm/Post Time table, plus a Clear Alarms
        # button). Rows come from _PROCESS_STATUS_ALARM_BITS, decoded
        # against the official spec the user supplied - see that
        # constant's own comment for exactly which bits and why only
        # those four. Not shown: severity-based row coloring (the spec
        # names alarms, not severity levels, so there's no documented
        # basis to color one red and another yellow like the reference
        # photo does) or real historical timestamps (nothing in this
        # project logs when a bit last changed) - "Post Time" just says
        # "Active" for whatever's currently posted.
        try:
            status_value = int(self._last_process_status)
        except (TypeError, ValueError):
            status_value = None
        active_alarms = []
        if status_value is not None:
            active_alarms = [name for bit, name in _PROCESS_STATUS_ALARM_BITS if status_value & (1 << bit)]

        dlg = QDialog(self)
        dlg.setWindowTitle("Alarms")
        dlg.setStyleSheet("QDialog { background: #79dde2; }")
        layout = QVBoxLayout(dlg)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)

        title = QLabel("ALARMS")
        title.setStyleSheet("font-size: 22pt; font-weight: bold; color: #111; background: transparent;")
        layout.addWidget(title)

        row_height = 32
        row_count = max(8, len(active_alarms))  # currently always 8 - only 4 bits are tracked
        table = QTableWidget(row_count, 2)
        table.setHorizontalHeaderLabels(["Alarm", "Post Time"])
        table.verticalHeader().setVisible(False)
        table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        table.setStyleSheet(
            "QTableWidget { background: white; gridline-color: #222; border: 2px solid #222; "
            "font-size: 12pt; font-weight: bold; color: #111; }"
            "QHeaderView::section { background: #79dde2; color: #111; border: none; "
            "border-bottom: 2px solid #222; font-weight: bold; padding: 4px; }"
        )
        table.horizontalHeader().setStretchLastSection(True)
        table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        for row in range(row_count):
            table.setRowHeight(row, row_height)
            if row < len(active_alarms):
                name_item = QTableWidgetItem(active_alarms[row])
                time_item = QTableWidgetItem("Active")
                # One consistent "posted" color for every alarm, not a
                # per-alarm severity scheme - see the comment above on
                # why (the spec doesn't document severity).
                for item in (name_item, time_item):
                    item.setBackground(QColor("#f4e04d"))
                table.setItem(row, 0, name_item)
                table.setItem(row, 1, time_item)
            else:
                table.setItem(row, 0, QTableWidgetItem(""))
                table.setItem(row, 1, QTableWidgetItem(""))
        # QTableWidget's own sizeHint doesn't account for setRowHeight
        # calls made after construction - without this, the layout gave
        # it some generic default height instead of enough to show all 8
        # rows, so it scrolled (confirmed by actually rendering this and
        # seeing a scrollbar cutting off the last few rows).
        table.setFixedHeight(
            table.horizontalHeader().height() + row_count * row_height + 2 * table.frameWidth()
        )
        layout.addWidget(table)

        raw = QLabel(
            f"Raw Process Status: {self._last_process_status}    "
            f"Raw Machine Status: {self._last_machine_status}"
        )
        raw.setStyleSheet("color: #333; background: transparent; font-size: 10pt;")
        layout.addWidget(raw)
        layout.addStretch(1)

        entry_style = (
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 14pt; font-weight: bold; padding: 22px;"
        )
        bottom_row = QHBoxLayout()
        bottom_row.addStretch(1)
        clear_btn = QPushButton("Clear Alarms")
        clear_btn.setStyleSheet(entry_style)
        clear_btn.clicked.connect(self._on_clear_alarms_clicked)
        bottom_row.addWidget(clear_btn)
        bottom_row.addStretch(1)
        exit_btn = QPushButton("Exit")
        exit_btn.setStyleSheet(entry_style)
        exit_btn.clicked.connect(dlg.accept)
        bottom_row.addWidget(exit_btn)
        layout.addLayout(bottom_row)

        dlg.resize(650, 520)
        dlg.exec()

    def _on_clear_alarms_clicked(self):
        # Machine Status (40014) = 3 - confirmed as a real, momentary
        # "clear alarms" command (not a state) via the user's own
        # confirmed usage on an FN dryer (see EquipmentModel::
        # writeRegister()'s comment in the firmware) - same
        # RegisterWriter/_pending_writers plumbing as _write_register,
        # just with no tile to update afterward.
        if not self._ip:
            # No direct IP at all (e.g. Dryer SHED) - over the mesh
            # instead. See on_write_register_ack()'s own reg==40014
            # branch for how the result comes back.
            if self._mac is not None:
                self._mesh_clear_alarms_pending = True
                self.write_register_requested.emit(self._mac, 40014, 3.0)
            return
        writer = RegisterWriter(self._ip, 40014, 3.0)
        self._pending_writers.append(writer)
        writer.write_succeeded.connect(
            lambda *_: QMessageBox.information(self, "Alarms cleared", "Clear-alarms command sent.")
        )
        writer.write_error.connect(
            lambda _reg, message: QMessageBox.warning(self, "Clear failed", message)
        )
        writer.finished.connect(lambda w=writer: self._pending_writers.remove(w))
        writer.start()

    def _set_machine_status_display(self, text: str, color: str):
        self.machine_status_value.setText(text)
        self.machine_status_box.setStyleSheet(f"QFrame {{ background: {color}; border: 2px solid #222; }}")

    def _refresh_machine_status_display(self):
        # Confirmed Running/Stopped -> its real color; anything else
        # (still "--", or a poll that hasn't reached the pending target
        # yet - see _on_status_received) -> neutral gray. Centralized
        # here so _on_status_received and _on_machine_status_clicked's
        # error rollback can't drift into picking different colors for
        # the same state.
        color = {
            "0": _MACHINE_STATUS_COLOR_STOPPED,
            "1": _MACHINE_STATUS_COLOR_RUNNING,
        }.get(self._last_machine_status, _MACHINE_STATUS_COLOR_UNKNOWN)
        self._set_machine_status_display(
            _MACHINE_STATUS_TEXT.get(self._last_machine_status, self._last_machine_status), color
        )

    def _on_machine_status_clicked(self, event=None):
        # Toggle: Running (confirmed "1") -> write 0 to stop it,
        # anything else (Stopped "0", or unconfirmed "--") -> write 1 to
        # start it. Refuses to guess from an unconfirmed state rather
        # than assuming "not running" means "safe to start" - "--" only
        # happens before the first successful poll or while comms are
        # lost, neither of which is a safe basis for a start command on
        # real equipment.
        current = self._last_machine_status
        if current not in ("0", "1"):
            QMessageBox.warning(
                self, "Unknown state",
                "Machine Status hasn't been confirmed by a poll yet - not safe to toggle.",
            )
            return
        target = "0" if current == "1" else "1"
        self._machine_status_pending = target
        self._set_machine_status_display(
            "Stopping..." if target == "0" else "Starting...", _MACHINE_STATUS_COLOR_UNKNOWN
        )

        if not self._ip:
            # No direct IP at all (e.g. Dryer SHED) - over the mesh
            # instead. The optimistic "Stopping.../Starting..." text
            # above still applies; on_write_register_ack()'s reg==40014
            # branch rolls it back on an outright rejection, same as
            # on_error below does for the direct-IP path. A genuine
            # success just leaves it in place until the next
            # register_values broadcast (already arriving every ~10s
            # regardless of IP) confirms the real transition via
            # _on_status_received's own pending-value check.
            if self._mac is not None:
                self.write_register_requested.emit(self._mac, 40014, float(target))
            return

        writer = RegisterWriter(self._ip, 40014, float(target))
        self._pending_writers.append(writer)

        def on_error(_reg, message, target=target):
            # Roll back the optimistic "Stopping.../Starting..." text -
            # this path is for the write itself failing outright (device
            # rejected it, unreachable, etc.), as opposed to the write
            # succeeding but the device just not having transitioned yet
            # by the next poll, which _on_status_received's own pending-
            # value check already handles by continuing to wait.
            if self._machine_status_pending == target:
                self._machine_status_pending = None
            self._refresh_machine_status_display()
            QMessageBox.warning(self, "Write failed", f"Register 40014: {message}")

        writer.write_error.connect(on_error)
        writer.finished.connect(lambda w=writer: self._pending_writers.remove(w))
        writer.start()

    def _on_menus_clicked(self):
        # A full-screen grid, per a real dryer HMI's own MENUS screen
        # (user-supplied reference photo) - not a small native QMenu
        # dropdown, which read as too minor/desktop-y next to the rest of
        # this page's HMI-styled look. Only 2 entries exist right now, so
        # there's no pagination yet ("MENUS (1/2)" in the reference) -
        # the grid layout is what makes adding more later (to fill out a
        # second page) just a matter of appending to _entries, not a
        # redesign.
        entries = [
            ("Configuration / Setup", self._on_configuration_menu),
            ("Temperature Setpoints", self._on_temperature_settings_clicked),
            ("Remote Control", self._on_remote_control_menu),
            ("Registers", self._on_registers_menu),
        ]

        dlg = QDialog(self)
        dlg.setWindowTitle("Menus")
        dlg.setStyleSheet("QDialog { background: #79dde2; }")
        layout = QVBoxLayout(dlg)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)

        title = QLabel("MENUS")
        title.setStyleSheet("font-size: 22pt; font-weight: bold; color: #111; background: transparent;")
        layout.addWidget(title)

        grid = QGridLayout()
        grid.setSpacing(14)
        entry_style = (
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 14pt; font-weight: bold; padding: 22px;"
        )
        for i, (label, handler) in enumerate(entries):
            btn = QPushButton(label)
            btn.setStyleSheet(entry_style)
            # Closes this MENUS popup before opening the entry's own
            # dialog - stacking a second dialog on top of this one would
            # be worse than just closing it
            # first every time.
            def make_slot(dlg=dlg, handler=handler):
                def _slot():
                    dlg.accept()
                    handler()
                return _slot
            btn.clicked.connect(make_slot())
            grid.addWidget(btn, i // 2, i % 2)
        layout.addLayout(grid)
        layout.addStretch(1)

        exit_row = QHBoxLayout()
        exit_row.addStretch(1)
        exit_btn = QPushButton("Exit")
        exit_btn.setStyleSheet(entry_style)
        exit_btn.clicked.connect(dlg.reject)
        exit_row.addWidget(exit_btn)
        layout.addLayout(exit_row)

        dlg.resize(520, 420)
        dlg.exec()

    def _push_setting(self, endpoint: str, value):
        # Same direct-by-IP HTTP pattern as _write_register, for the
        # simple device-level settings endpoints instead of a numbered
        # dryer register - see SettingWriter's own docstring for exactly
        # which endpoints these are and why cleanup happens on finished.
        if not self._ip:
            QMessageBox.warning(self, "No IP", "This node has no known IP address (mesh-only node).")
            return
        writer = SettingWriter(self._ip, endpoint, value)
        self._pending_writers.append(writer)
        writer.write_error.connect(lambda _endpoint, message: QMessageBox.warning(self, "Write failed", message))
        writer.finished.connect(lambda w=writer: self._pending_writers.remove(w))
        writer.start()

    def _on_configuration_menu(self):
        # Equipment/Model/SPI Address/Baud Rate, restyled to match this
        # page's teal HMI popups (title top-left, Back bottom-right) -
        # per the user's own screenshot of NodeDetailPage's existing
        # Configuration section, which they wanted "in a new menu style
        # config page" instead. Pushes changes the same direct-by-IP way
        # as everything else on this page (see _push_setting) rather than
        # reusing NodeDetailPage's RTS-NOW-over-ESP-NOW settings push -
        # DryerWebServer.cpp already exposes /api/equipment, /api/model,
        # /api/baud, and /api/writestationid as simple one-value POSTs,
        # so there's a second, working mechanism for these exact fields
        # already, with no ESP-NOW gateway hop needed.
        #
        # Optimistic like the equipment/model buttons elsewhere on this
        # page - not tracked with a pending/confirm cycle like Machine
        # Status's toggle, since getting this wrong isn't a safety issue
        # the way an unconfirmed start/stop command is, and the next
        # regular poll (_on_status_received) corrects it either way
        # within ~3s if the write didn't actually take.
        selected_style = (
            "background: #1e88e5; color: white; border: 2px solid #0d47a1; "
            "border-radius: 6px; font-size: 12pt; font-weight: bold; padding: 10px 16px;"
        )
        normal_style = (
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 12pt; font-weight: bold; padding: 10px 16px;"
        )
        label_style = "font-size: 13pt; font-weight: bold; color: #111; background: transparent;"

        dlg = QDialog(self)
        dlg.setWindowTitle("Configuration")
        dlg.setStyleSheet("QDialog { background: #79dde2; }")
        layout = QVBoxLayout(dlg)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(18)

        title = QLabel("CONFIGURATION")
        title.setStyleSheet("font-size: 22pt; font-weight: bold; color: #111; background: transparent;")
        layout.addWidget(title)

        # --- Name --- goes over RTS-NOW's settings mechanism (see
        # rename_requested's own comment), not a direct HTTP POST like
        # everything else in this popup - the only field here that
        # doesn't use _push_setting.
        name_row = QHBoxLayout()
        name_label_widget = QLabel("Name:")
        name_label_widget.setStyleSheet(label_style)
        name_row.addWidget(name_label_widget)
        name_edit = QLineEdit(self.name_label.text())
        name_edit.setFixedWidth(220)
        name_edit.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 4px; font-size: 13pt; padding: 8px;"
        )

        def on_name_finished():
            new_name = name_edit.text().strip()
            if not new_name or new_name == self.name_label.text():
                return
            if not self._mac:
                return
            rename_status.setText("Sent - awaiting node confirmation...")
            self.rename_requested.emit(self._mac, new_name)

        name_edit.editingFinished.connect(on_name_finished)
        name_row.addWidget(name_edit)
        rename_status = QLabel("")
        rename_status.setStyleSheet("color: #333; background: transparent; font-size: 10pt;")
        self._rename_status_label = rename_status
        name_row.addWidget(rename_status)
        name_row.addStretch(1)
        layout.addLayout(name_row)

        # --- Equipment ---
        equip_row = QHBoxLayout()
        equip_label = QLabel("Equipment:")
        equip_label.setStyleSheet(label_style)
        equip_row.addWidget(equip_label)
        equip_buttons: dict[str, QPushButton] = {}

        def refresh_equip_styles():
            for name, btn in equip_buttons.items():
                btn.setStyleSheet(selected_style if name == self._last_equipment_type else normal_style)

        def on_equipment_clicked(name):
            # No-op guard, not just an optimization - confirmed by
            # reading handleSetEquipment() in the firmware that /api/
            # equipment ALWAYS resets Model to that equipment's first
            # option as a side effect, even when the posted value is the
            # same as what's already selected. Without this guard, an
            # exploratory/redundant click on the already-selected
            # Equipment button would silently reset a real dryer's Model
            # to FC.
            if name == self._last_equipment_type:
                return
            self._push_setting("api/equipment", name)
            self._last_equipment_type = name
            # Mirrors that same firmware side effect locally, so this
            # popup's own Model row doesn't keep optimistically showing
            # the OLD model after an equipment change actually resets it
            # on the device.
            self._last_model = MODELS_BY_EQUIPMENT.get(name, ["?"])[0]
            refresh_equip_styles()
            rebuild_model_row()

        for name in EQUIPMENT_TYPES:
            btn = QPushButton(name)
            btn.clicked.connect(lambda checked=False, n=name: on_equipment_clicked(n))
            equip_buttons[name] = btn
            equip_row.addWidget(btn)
        equip_row.addStretch(1)
        layout.addLayout(equip_row)

        # --- Model (rebuilt whenever Equipment changes, same as
        # NodeDetailPage._rebuild_model_buttons) ---
        model_row = QHBoxLayout()
        model_label = QLabel("Model:")
        model_label.setStyleSheet(label_style)
        model_row.addWidget(model_label)
        model_buttons_holder = QWidget()
        model_buttons_layout = QHBoxLayout(model_buttons_holder)
        model_buttons_layout.setContentsMargins(0, 0, 0, 0)
        model_row.addWidget(model_buttons_holder)
        model_row.addStretch(1)
        layout.addLayout(model_row)

        model_buttons: dict[str, QPushButton] = {}

        def refresh_model_styles():
            for name, btn in model_buttons.items():
                btn.setStyleSheet(selected_style if name == self._last_model else normal_style)

        def on_model_clicked(name):
            if name == self._last_model:
                return
            self._push_setting("api/model", name)
            self._last_model = name
            refresh_model_styles()

        def rebuild_model_row():
            while model_buttons_layout.count():
                item = model_buttons_layout.takeAt(0)
                w = item.widget()
                if w:
                    w.deleteLater()
            model_buttons.clear()
            for name in MODELS_BY_EQUIPMENT.get(self._last_equipment_type, []):
                btn = QPushButton(name)
                btn.clicked.connect(lambda checked=False, n=name: on_model_clicked(n))
                model_buttons[name] = btn
                model_buttons_layout.addWidget(btn)
            refresh_model_styles()

        rebuild_model_row()
        refresh_equip_styles()

        # --- SPI Address ---
        addr_row = QHBoxLayout()
        addr_label = QLabel("SPI Address:")
        addr_label.setStyleSheet(label_style)
        addr_row.addWidget(addr_label)
        addr_edit = QLineEdit(str(self._last_spi_address))
        addr_edit.setFixedWidth(100)
        addr_edit.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 4px; font-size: 13pt; padding: 8px;"
        )

        def on_addr_finished():
            text = addr_edit.text().strip()
            if not text.isdigit():
                QMessageBox.warning(dlg, "Invalid address", "SPI Address must be a number.")
                addr_edit.setText(str(self._last_spi_address))
                return
            if int(text) == self._last_spi_address:
                return
            self._push_setting("api/writestationid", text)
            self._last_spi_address = int(text)

        addr_edit.editingFinished.connect(on_addr_finished)
        addr_row.addWidget(addr_edit)
        addr_row.addStretch(1)
        layout.addLayout(addr_row)

        # --- Baud Rate ---
        baud_row = QHBoxLayout()
        baud_label = QLabel("Baud Rate:")
        baud_label.setStyleSheet(label_style)
        baud_row.addWidget(baud_label)
        baud_buttons: dict[int, QPushButton] = {}

        def refresh_baud_styles():
            for rate, btn in baud_buttons.items():
                btn.setStyleSheet(selected_style if rate == self._last_spi_baud else normal_style)

        def on_baud_clicked(rate):
            if rate == self._last_spi_baud:
                return
            self._push_setting("api/baud", rate)
            self._last_spi_baud = rate
            refresh_baud_styles()

        for rate in BAUD_RATES:
            btn = QPushButton(str(rate))
            btn.clicked.connect(lambda checked=False, r=rate: on_baud_clicked(r))
            baud_buttons[rate] = btn
            baud_row.addWidget(btn)
        baud_row.addStretch(1)
        layout.addLayout(baud_row)
        refresh_baud_styles()

        layout.addStretch(1)

        back_row = QHBoxLayout()
        back_row.addStretch(1)
        back_btn = QPushButton("Back")
        back_btn.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 14pt; font-weight: bold; padding: 22px;"
        )
        back_btn.clicked.connect(dlg.accept)
        back_row.addWidget(back_btn)
        layout.addLayout(back_row)

        dlg.resize(650, 480)
        dlg.exec()

    def _on_remote_control_menu(self):
        # Reboot goes over the gateway's serial/ESP-NOW connection, not
        # direct-by-IP HTTP like every other write on this page - there's
        # no /api/reboot (or equivalent) in DryerWebServer.cpp, confirmed
        # by reading it, so this reuses the same reboot_requested/
        # _on_reboot_requested path NodeDetailPage's own Reboot button
        # already goes through (see MainWindow's wiring) rather than
        # inventing a second mechanism.
        dlg = QDialog(self)
        dlg.setWindowTitle("Remote Control")
        dlg.setStyleSheet("QDialog { background: #79dde2; }")
        layout = QVBoxLayout(dlg)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(18)

        title = QLabel("REMOTE CONTROL")
        title.setStyleSheet("font-size: 22pt; font-weight: bold; color: #111; background: transparent;")
        layout.addWidget(title)

        entry_style = (
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 14pt; font-weight: bold; padding: 22px;"
        )

        def on_reboot_clicked():
            if not self._mac:
                return
            confirmed = QMessageBox.question(
                dlg, "Reboot node",
                f"Reboot {self.name_label.text()} ({self._mac})? It will briefly go offline.",
            )
            if confirmed == QMessageBox.StandardButton.Yes:
                self.reboot_requested.emit(self._mac)

        reboot_btn = QPushButton("Reboot Node")
        reboot_btn.setStyleSheet(entry_style)
        reboot_btn.clicked.connect(on_reboot_clicked)
        layout.addWidget(reboot_btn)

        # --- OTA Update - moved here from its own separate menu entry
        # per explicit request, since it's conceptually the same "remote
        # control" category as Reboot. Direct-by-IP over Wi-Fi (see
        # OtaPushClient), NOT through the gateway like Reboot above -
        # this popup deliberately stays open during the push instead of
        # closing immediately like every other MENUS entry, so the
        # progress bar/status label have somewhere to report to; see
        # set_ota_progress/set_ota_result for how MainWindow reaches them
        # even if this dialog gets closed while a push is still in
        # flight.
        ota_label = QLabel("OTA Update")
        ota_label.setStyleSheet("font-size: 13pt; font-weight: bold; color: #111; background: transparent;")
        layout.addWidget(ota_label)

        file_row = QHBoxLayout()
        browse_btn = QPushButton("Browse for .bin...")
        browse_btn.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 11pt; font-weight: bold; padding: 8px 14px;"
        )
        file_label = QLabel(self._ota_file_path or "No file selected")
        file_label.setStyleSheet("color: #111; background: transparent; font-size: 10pt;")
        push_btn = QPushButton("Push Update")
        push_btn.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 11pt; font-weight: bold; padding: 8px 14px;"
        )
        push_btn.setEnabled(bool(self._ip and self._ota_file_path))
        self._ota_push_btn = push_btn

        def on_browse_clicked():
            file_path, _ = QFileDialog.getOpenFileName(dlg, "Select firmware image", "", "Firmware images (*.bin)")
            if not file_path:
                return
            self._ota_file_path = file_path
            file_label.setText(file_path)
            push_btn.setEnabled(bool(self._ip and self._ota_file_path))

        def on_push_clicked():
            if not self._ip or not self._ota_file_path:
                return
            if QMessageBox.question(
                dlg, "Push OTA update",
                f"Push {self._ota_file_path} to {self.name_label.text()} ({self._ip})? "
                "The node will reboot when done.",
            ) != QMessageBox.StandardButton.Yes:
                return
            ota_progress.setValue(0)
            ota_status_label.setText("Starting...")
            push_btn.setEnabled(False)
            self.ota_requested.emit(self._ip, self._ota_file_path)

        browse_btn.clicked.connect(on_browse_clicked)
        push_btn.clicked.connect(on_push_clicked)
        file_row.addWidget(browse_btn)
        file_row.addWidget(file_label, stretch=1)
        file_row.addWidget(push_btn)
        layout.addLayout(file_row)

        ota_progress = QProgressBar()
        ota_progress.setRange(0, 100)
        layout.addWidget(ota_progress)
        ota_status_label = QLabel("")
        ota_status_label.setStyleSheet("color: #333; background: transparent; font-size: 10pt;")
        layout.addWidget(ota_status_label)
        self._ota_progress_bar = ota_progress
        self._ota_status_label = ota_status_label

        layout.addStretch(1)

        back_row = QHBoxLayout()
        back_row.addStretch(1)
        back_btn = QPushButton("Back")
        back_btn.setStyleSheet(entry_style)
        back_btn.clicked.connect(dlg.accept)
        back_row.addWidget(back_btn)
        layout.addLayout(back_row)

        dlg.resize(480, 480)
        dlg.exec()

    def set_ota_progress(self, percent: int):
        # Called by MainWindow from a background OtaPushClient - guarded
        # since the Remote Control popup that created these widgets may
        # have already been closed (Back doesn't cancel an in-flight
        # push, it just stops watching it), which deletes the underlying
        # C++ objects and turns any further access into a RuntimeError
        # rather than a silent no-op.
        try:
            self._ota_progress_bar.setValue(percent)
            self._ota_status_label.setText(f"Uploading... {percent}%")
        except (RuntimeError, AttributeError):
            pass

    def set_ota_result(self, success: bool, message: str):
        try:
            self._ota_status_label.setText(("Success: " if success else "Failed: ") + message)
            self._ota_push_btn.setEnabled(True)
        except (RuntimeError, AttributeError):
            pass

    def set_rename_status(self, text: str):
        # Same guarded-widget-may-be-gone reasoning as set_ota_progress/
        # set_ota_result - MainWindow calls this from the rename
        # confirmation timer or the unsolicited setting_ack event, either
        # of which can land after the Configuration popup's been closed.
        try:
            self._rename_status_label.setText(text)
        except (RuntimeError, AttributeError):
            pass

    def _on_registers_menu(self):
        # Live while open, refreshed every _REGISTERS_REFRESH_MS from
        # _last_live - which the regular poller already keeps updated in
        # the background regardless of whether this dialog is open, so
        # there's no new data path here, just a timer re-reading it.
        dlg = QDialog(self)
        dlg.setWindowTitle("Registers")
        dlg.setStyleSheet("QDialog { background: #79dde2; }")
        layout = QVBoxLayout(dlg)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)

        title = QLabel("REGISTERS")
        title.setStyleSheet("font-size: 22pt; font-weight: bold; color: #111; background: transparent;")
        layout.addWidget(title)

        table = QTableWidget(0, 4)
        table.setHorizontalHeaderLabels(["Register", "Name", "Value", "Last Heard"])
        table.verticalHeader().setVisible(False)
        table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        table.setStyleSheet(
            "QTableWidget { background: white; gridline-color: #222; border: 2px solid #222; "
            "font-size: 11pt; font-weight: bold; color: #111; }"
            "QHeaderView::section { background: #79dde2; color: #111; border: none; "
            "border-bottom: 2px solid #222; font-weight: bold; padding: 4px; }"
        )
        table.horizontalHeader().setStretchLastSection(True)
        table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        row_height = 28
        layout.addWidget(table)

        empty = QLabel("No live register data yet.")
        empty.setStyleSheet("color: #333; background: transparent; font-size: 10pt;")
        layout.addWidget(empty)

        def refresh_table():
            rows = sorted(self._last_live.items())
            table.setRowCount(len(rows))
            for row, (reg, reg_data) in enumerate(rows):
                table.setRowHeight(row, row_height)
                table.setItem(row, 0, QTableWidgetItem(str(reg)))
                table.setItem(row, 1, QTableWidgetItem(str(reg_data.get("name", ""))))
                table.setItem(row, 2, QTableWidgetItem(str(reg_data.get("value", ""))))
                # receivedAt (mesh path - show_register_values()) wins over
                # ageMs (direct-IP path - _on_status_received()'s own
                # /api/data registers) when both could theoretically be
                # present, since a live-computed age is always more
                # current than one frozen at whatever instant that JSON
                # was fetched.
                received_at = reg_data.get("receivedAt")
                if received_at is not None:
                    age_ms = int((time.monotonic() - received_at) * 1000)
                else:
                    age_ms = reg_data.get("ageMs")
                age_text = f"{age_ms // 1000}s ago" if age_ms is not None else "?"
                table.setItem(row, 3, QTableWidgetItem(age_text))
            table.setFixedHeight(
                table.horizontalHeader().height() + max(len(rows), 1) * row_height + 2 * table.frameWidth()
            )
            empty.setVisible(not rows)

        refresh_table()
        layout.addStretch(1)

        back_row = QHBoxLayout()
        back_row.addStretch(1)
        back_btn = QPushButton("Back")
        back_btn.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 14pt; font-weight: bold; padding: 22px;"
        )
        back_btn.clicked.connect(dlg.accept)
        back_row.addWidget(back_btn)
        layout.addLayout(back_row)

        # Parented to dlg (auto-deleted with it) but also explicitly
        # stopped on close - relying on parent-deletion alone would still
        # let one more queued timeout fire in the gap between the user
        # clicking Back and Qt actually destroying the timer, which would
        # touch this closure's table/empty widgets right as they're being
        # torn down.
        _REGISTERS_REFRESH_MS = 1000
        timer = QTimer(dlg)
        timer.timeout.connect(refresh_table)
        timer.start(_REGISTERS_REFRESH_MS)
        dlg.finished.connect(timer.stop)

        dlg.resize(500, 600)
        dlg.exec()

    def _make_editable_tile(self, reg: int, label: str, value) -> "_Tile":
        # A plain _Tile with its mousePressEvent overridden on this one
        # instance (same instance-level-override pattern already used
        # for the diagram frame's resizeEvent) rather than making every
        # _Tile everywhere clickable - only these two, inside this one
        # dialog, are meant to be editable.
        tile = _Tile(label)
        tile.set_value(str(value))
        tile.setCursor(Qt.CursorShape.PointingHandCursor)
        tile.setToolTip("Click to enter a new value")

        def _on_click(event, reg=reg, tile=tile):
            self._prompt_write_register(reg, label, tile)

        tile.mousePressEvent = _on_click
        return tile

    def _prompt_write_register(self, reg: int, label: str, tile: "_Tile"):
        current = self._last_live.get(reg, {}).get("value", 0)
        try:
            current_int = int(current)
        except (TypeError, ValueError):
            current_int = 0
        new_value, ok = QInputDialog.getInt(self, label, f"New {label}:", current_int, -999, 999)
        if not ok:
            return
        tile.set_value("...")
        self._write_register(reg, float(new_value), tile)

    def _write_register(self, reg: int, value: float, tile: "_Tile"):
        if not self._ip:
            # No direct IP at all (e.g. Dryer SHED) - over the mesh
            # instead. tile already shows "..." (see
            # _prompt_write_register) - on_write_register_ack() rolls it
            # back on an outright rejection; a genuine success just
            # leaves it until the next register_values broadcast (already
            # arriving every ~10s regardless of IP) confirms the real
            # value via _on_status_received.
            if self._mac is not None:
                self.write_register_requested.emit(self._mac, reg, value)
            return
        writer = RegisterWriter(self._ip, reg, value)
        self._pending_writers.append(writer)

        def on_succeeded(written_reg, written_value, tile=tile):
            # Guarded against the dialog (and this tile with it) having
            # already been closed/destroyed while the write was still in
            # flight - PySide6 raises RuntimeError for a deleted C++
            # object rather than silently no-op-ing.
            try:
                tile.set_value(str(int(written_value)))
            except RuntimeError:
                pass

        def on_error(failed_reg, message, tile=tile):
            try:
                tile.set_value(str(self._last_live.get(failed_reg, {}).get("value", "--")))
            except RuntimeError:
                pass
            QMessageBox.warning(self, "Write failed", f"Register {failed_reg}: {message}")

        writer.write_succeeded.connect(on_succeeded)
        writer.write_error.connect(on_error)
        writer.finished.connect(lambda w=writer: self._pending_writers.remove(w))
        writer.start()

    def _on_temperature_settings_clicked(self):
        # A snapshot at click time, not a live-updating view - same
        # choice _on_alarm_clicked already makes (see its own dialog).
        # All three tiles are clickable to enter+push a new value (see
        # _make_editable_tile/_write_register) - not restricted
        # client-side to just the registers EquipmentModel::
        # writeRegister() currently allowlists (40010/40011/40017 as of
        # this writing - 40017 confirmed working end-to-end against real
        # hardware after that allowlist was extended): that firmware
        # endpoint already returns a clear rejection reason for an
        # unsupported register, which is more robust than this app
        # hardcoding (and risking drifting out of sync with) the same
        # allowlist.
        setpoint = self._last_live.get(40010, {}).get("value", "--")
        limit_delta = self._last_live.get(40011, {}).get("value", "--")
        dew_trigger = self._last_live.get(40017, {}).get("value", "--")

        # Same teal HMI-screen look as _on_menus_clicked's popup (title
        # top-left, white tiles/buttons, Back playing the same bottom-
        # right role Exit does there) - this used to just be a plain
        # native-styled QDialog, which looked jarringly out of place
        # popping up over this page's own screen (confirmed via an
        # actual screenshot: default gray dialog body, native blue
        # default-button styling on Back, neither matching anything else
        # here).
        dlg = QDialog(self)
        dlg.setWindowTitle("Temperature Setpoints")
        dlg.setStyleSheet("QDialog { background: #79dde2; }")
        layout = QVBoxLayout(dlg)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)

        title = QLabel("TEMPERATURE SETPOINTS")
        title.setStyleSheet("font-size: 22pt; font-weight: bold; color: #111; background: transparent;")
        layout.addWidget(title)

        row = QHBoxLayout()
        row.setSpacing(14)
        # Process Setpoint moved here from the main diagram (it used to
        # sit under Process Air Inlet with its own amber tint) - grouped
        # with the other two editable setpoints instead, off the main
        # screen entirely per explicit request.
        row.addWidget(self._make_editable_tile(40010, "Process Setpoint", setpoint))
        row.addWidget(self._make_editable_tile(40011, "Process Limit Delta", limit_delta))
        row.addWidget(self._make_editable_tile(40017, "Dew Point Trigger", dew_trigger))
        layout.addLayout(row)
        layout.addStretch(1)

        back_row = QHBoxLayout()
        back_row.addStretch(1)
        back_btn = QPushButton("Back")
        back_btn.setStyleSheet(
            "background: white; color: #111; border: 2px solid #222; "
            "border-radius: 6px; font-size: 14pt; font-weight: bold; padding: 22px;"
        )
        back_btn.clicked.connect(dlg.accept)
        back_row.addWidget(back_btn)
        layout.addLayout(back_row)

        dlg.resize(520, 420)
        dlg.exec()

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
            self.model_label.setText("")
            # Without this, the PREVIOUS device's register values (temps,
            # dew point, setpoints...) stayed on screen - still fully
            # populated, just now silently mislabeled under the NEW
            # device's name - until the new device's own poller happened
            # to answer. Confirmed live switching Office -> Shed: Shed's
            # page showed Office's numbers for however long Shed's own
            # (often slower, mesh-only) response took to arrive. Wrong
            # temperature/dew-point readings displayed against the wrong
            # physical dryer is a real safety concern, not just a cosmetic
            # one, so this clears to "--" immediately on every device
            # switch rather than leaving stale-but-plausible-looking data
            # up a moment longer.
            self._last_live = {}
            for tiles in self._register_tiles.values():
                for tile in tiles:
                    tile.set_value("--")
            if dev.ip:
                self._start_poller(dev.ip)

        # Refreshed every call (not just on a new selection) - this is the
        # same known_devices snapshot driving the Hardware List table, so
        # RSSI/IP/firmware here should track it just as live.
        self.wifi_rssi_label.setText(f"WiFi: {format_rssi(dev.wifi_rssi)}")
        self.mesh_rssi_label.setText(f"MESH: {format_rssi(dev.espnow_rssi)}")
        self.node_info_label.setText(
            f"{dev.device_type_name} ({dev.board_name})   |   {dev.mac}   |   FW {dev.firmware_version}   |   "
            f"{dev.ip or 'no IP (mesh-only)'}"
        )
        self._position_overlay_labels()

    def refresh_if_current(self, dev: KnownDevice):
        if self._mac == dev.mac:
            self.show_device(dev)

    def current_mac(self) -> str | None:
        return self._mac

    def shutdown(self):
        """Call on application exit - see NodeDetailPage.shutdown()'s
        identical reasoning."""
        self._stop_poller()

    def show_register_values(self, start_register: int, values: list[int]):
        """Feeds a raw mesh register_values snapshot (see main_window.py's
        "register_values" handling) into the exact same rendering path
        _on_status_received already uses for a direct-IP node's own
        /api/data JSON - reconstructed from the raw {reg: value} array
        using this project's own well-known fixed register numbers (see
        RTSNow-SPI-CCP's ModbusRegisterMap.h), not anything mesh-protocol-
        specific. Used for a node with no IP at all (e.g. Dryer SHED's
        persistently weak link) - /api/data can never be reached there,
        but this same register block is already broadcast over ESP-NOW
        every ~10s regardless of IP (plus once on demand whenever
        main_window.py's _ensure_device_info sends poll_registers).

        No per-register "ageMs" here (unlike the direct-IP path, where the
        node itself reports how long ago its own SPI-IM poll touched each
        register - see DryerWebServer.cpp's registerAgeMs()) - the whole
        block arrives as one mesh snapshot at a single instant, so there's
        only one meaningful age: how long ago *this snapshot* was
        received. receivedAt captures that instant; the Registers dialog's
        refresh_table() computes a live, ticking-up age from it on every
        1s redraw, rather than the frozen "?" you'd get with no age field
        at all (ageMs would need to itself keep advancing between mesh
        updates, not just be set once at ingest)."""
        raw = {start_register + i: value for i, value in enumerate(values)}
        model_info = decode_model_type_register(raw.get(MODBUS_REG_MODEL_TYPE, -1)) or {}
        model = model_info.get("model", "")
        received_at = time.monotonic()
        data = {
            "equipmentType": model_info.get("equipmentType", "?"),
            "model": model_info.get("model", "?"),
            "spiAddress": raw.get(40002, "?"),
            "spiBaudRate": raw.get(40003, "?"),
            "registers": [
                {
                    "reg": reg,
                    "name": _mesh_register_name(reg, model),
                    "value": _reinterpret_signed16(value) if reg in _SIGNED_MESH_REGISTERS else value,
                    "present": True,
                    "receivedAt": received_at,
                }
                for reg, value in raw.items()
            ],
        }
        self._on_status_received(data)

    def _on_status_received(self, data: dict):
        self.model_label.setText(f"{data.get('equipmentType', '?')} / {data.get('model', '?')}")
        self._last_equipment_type = data.get("equipmentType", "?")
        self._last_model = data.get("model", "?")
        self._last_spi_address = data.get("spiAddress", "?")
        self._last_spi_baud = data.get("spiBaudRate", "?")

        # The node's own /api/data splits these into two separate top-
        # level arrays - "registers" (per-model, e.g. 40010-40021, each
        # entry carrying "present"/"ageMs") and "deviceRegisters" (the
        # XBEE SETUP block, 40001-40009 - Software Version/SPI Station
        # ID/Baud/Model Type/RSSI/etc., see DryerWebServer.cpp's own
        # comment on why that's read from the live Modbus table rather
        # than ActiveModel - always present, no "present"/"ageMs" fields
        # at all). Merging only "registers" here used to make 40001-40009
        # silently vanish from _last_live/the Registers dialog the moment
        # a direct-HTTP response arrived - confirmed live: the dialog
        # showed the full 40001-40021 range right after opening (from an
        # initial mesh-sourced snapshot, which flattens everything into
        # one block - see show_register_values()), then narrowed to just
        # 40010-40021 once this poller's own response replaced it.
        live = {r["reg"]: r for r in data.get("registers", []) if r.get("present")}
        # deviceRegisters mostly carries no "ageMs" (the firmware reads
        # these straight from the live Modbus table on every request, so
        # there's usually no staleness for it to report - see
        # DryerWebServer.cpp's own comment) - confirmed live: with WiFi
        # enabled (this direct-IP path), the Registers dialog showed a
        # permanent "?" for exactly these 9 rows while 40010+ ticked up
        # normally, disappearing entirely with WiFi off
        # (show_register_values()'s mesh path stamps everything with one
        # receivedAt instead). Since a live Modbus-table read really is
        # only as fresh as this exact HTTP response, stamping it with the
        # same received_at instant here is honest, not fabricated - it's
        # "how long ago we last confirmed this value", identical in spirit
        # to receivedAt on the mesh path. Two of these nine (Board Temp/
        # SPI CRC Error) now DO carry a real firmware ageMs (see
        # DryerRegisters::registerAgeMs()) - left untouched here rather
        # than overwritten, so the dialog shows the node's own update
        # cadence for those specifically instead of this poll's.
        received_at = time.monotonic()
        for r in data.get("deviceRegisters", []):
            live[r["reg"]] = r if "ageMs" in r else {**r, "receivedAt": received_at}
        self._last_live = live
        for reg, tiles in self._register_tiles.items():
            reg_data = live.get(reg)
            text = str(reg_data["value"]) if reg_data else "--"
            for tile in tiles:
                tile.set_value(text)

        self.trend_chart.add_sample(
            temp=live.get(40012, {}).get("value"),
            setpoint=live.get(40010, {}).get("value"),
            dew=live.get(40016, {}).get("value"),
        )

        # Dew Point (40016) vs its own alarm Trigger threshold (40017) -
        # per explicit request: red once dew point rises above (more
        # positive than) the trigger (too humid), green once it's back
        # below it. Exactly at the trigger counts as green (not yet in
        # violation) rather than red - "trigger" reads as the point it
        # becomes a problem, not the last still-safe value. Neutral white
        # if either value isn't live yet, since there's nothing to
        # compare.
        dew_value = live.get(40016, {}).get("value")
        trigger_value = live.get(40017, {}).get("value")
        if dew_value is not None and trigger_value is not None:
            self._dew_box.set_alert_color("#e53935" if dew_value > trigger_value else "#4caf50")
        else:
            self._dew_box.set_alert_color(None)

        self._last_process_status = str(live[40013]["value"]) if 40013 in live else "--"
        self._last_machine_status = str(live[40014]["value"]) if 40014 in live else "--"
        # While a toggle is pending (see _on_machine_status_clicked),
        # keep showing "Stopping.../Starting..." until a poll actually
        # confirms the target value - a poll that still shows the OLD
        # value (device hasn't transitioned yet) must NOT overwrite the
        # transitional text with the stale one, which is exactly what
        # just unconditionally decoding _last_machine_status here would
        # do.
        if self._machine_status_pending is not None:
            if self._last_machine_status == self._machine_status_pending:
                self._machine_status_pending = None
            else:
                self._set_machine_status_display(
                    "Stopping..." if self._machine_status_pending == "0" else "Starting...",
                    _MACHINE_STATUS_COLOR_UNKNOWN,
                )
                return
        self._refresh_machine_status_display()

    def _on_status_error(self, message: str):
        # No dedicated display for this any more - the upper-right label
        # this used to drive ("Comms lost: ...") was replaced with the
        # WiFi/MESH RSSI readout (see show_device()), which is sourced
        # independently from the gateway's known_devices feed and so keeps
        # updating (and stays genuinely informative - "is this node even
        # reaching the gateway, and how well") through a direct-HTTP
        # outage like this one, rather than going blank.
        pass

    def on_write_register_ack(self, reg: int, ok: bool, message: str):
        """Called by main_window.py's "write_register_ack" handling for
        this exact device - the mesh-path completion for whichever of
        _on_clear_alarms_clicked/_on_machine_status_clicked/
        _write_register emitted write_register_requested for this
        register. No per-write value tracking here on success - a genuine
        success is just left for the next automatic register_values
        broadcast to confirm for real (see each of those methods' own
        comment), same as the direct-IP RegisterWriter path already
        relies on the next round-robin poll for."""
        if reg == 40014:
            # Both a clear-alarms command and a start/stop toggle write
            # this same register - _mesh_clear_alarms_pending is what
            # tells them apart (see _on_clear_alarms_clicked's own
            # comment).
            if self._mesh_clear_alarms_pending:
                self._mesh_clear_alarms_pending = False
                if ok:
                    QMessageBox.information(self, "Alarms cleared", "Clear-alarms command sent.")
                else:
                    QMessageBox.warning(self, "Clear failed", message)
                return
            if not ok:
                self._machine_status_pending = None
                self._refresh_machine_status_display()
                QMessageBox.warning(self, "Write failed", f"Register 40014: {message}")
            return

        if not ok:
            for tile in self._register_tiles.get(reg, []):
                try:
                    tile.set_value(str(self._last_live.get(reg, {}).get("value", "--")))
                except RuntimeError:
                    pass
            QMessageBox.warning(self, "Write failed", f"Register {reg}: {message}")
