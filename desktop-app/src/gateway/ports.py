from dataclasses import dataclass

from serial.tools import list_ports

# Espressif's own USB VID - the AtomS3U's native USB CDC (and any other
# ESP32-S3 board's) shows up under this vendor ID. Used only to sort
# likely candidates to the top of the port list, not to filter - other
# USB-serial chips (CH340, CP210x, etc.) are still selectable, since a
# node under test may use different hardware than the gateway itself.
ESPRESSIF_USB_VID = 0x303A


@dataclass
class SerialPortInfo:
    device: str
    description: str
    likely_espressif: bool
    # For this hardware's native USB CDC, the OS-reported USB serial
    # number *is* the chip's real MAC address (that's why it lines up
    # exactly with the deviceID the gateway itself reports - deviceID is
    # derived from the low 4 bytes of this same MAC). Not guaranteed on
    # every USB-serial chip, but this repo only ever targets ESP32-S3
    # native USB boards, which do provide it.
    serial_number: str | None = None

    @property
    def label(self) -> str:
        return f"{self.device} - {self.description}"


def list_serial_ports() -> list[SerialPortInfo]:
    ports = []
    for p in list_ports.comports():
        ports.append(
            SerialPortInfo(
                device=p.device,
                description=p.description or "Unknown",
                likely_espressif=(p.vid == ESPRESSIF_USB_VID),
                serial_number=p.serial_number,
            )
        )
    ports.sort(key=lambda p: (not p.likely_espressif, p.device))
    return ports
