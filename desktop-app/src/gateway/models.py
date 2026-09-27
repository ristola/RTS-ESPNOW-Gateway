from dataclasses import dataclass, field
from typing import Optional

# deviceTypeName travels the wire as a fixed, terse identifier
# (RTSNOW_DeviceIdentity.deviceTypeName is a hyphen-free char[16], can't
# fit e.g. "Ethernet-Gateway" with a null terminator) - this maps those
# wire values to a friendlier label for display only, never touching the
# wire format itself. Anything not listed here (PolymerPak's SolarTracker,
# etc.) is returned verbatim, unmapped.
_DEVICE_TYPE_LABELS = {
    "UsbGateway": "USB-Gateway",
    "EthernetGateway": "Ethernet-Gateway",
    # RTSNow-SPI-CCP's own dryer/crystallizer node firmware - "RTSNow-UNADYN"
    # is main_atom_node.cpp's wire deviceTypeName, kept as-is there; this is
    # a display-only rename to the project's actual name.
    "RTSNow-UNADYN": "SPI-CCP",
}


def device_type_label(device_type_name: str) -> str:
    return _DEVICE_TYPE_LABELS.get(device_type_name, device_type_name)


# Any RTS-NOW device whose deviceTypeName is one of these is a gateway
# itself (USB dongle or PoE/Ethernet), not a node with real RS-485/dryer
# equipment attached. Its own explicit set, NOT derived from
# _DEVICE_TYPE_LABELS' keys (that dict now also carries non-gateway
# rename-only entries like "RTSNow-UNADYN" - deriving from it here would
# wrongly mark real dryer nodes as gateways too and hide their equipment
# config box).
GATEWAY_DEVICE_TYPES = frozenset({"UsbGateway", "EthernetGateway"})


@dataclass
class NeighborLink:
    """One entry in a device's own view of which other device it can
    currently hear over ESP-NOW - see KnownDevice.neighbors' own comment.
    device_id, not mac: that's what the wire protocol carries (see
    gateway-firmware's fill_device_json) - resolve against another
    KnownDevice's own device_id when drawing this on the Mesh page."""

    device_id: int
    rssi: int

    @staticmethod
    def from_json(obj: dict) -> "NeighborLink":
        return NeighborLink(device_id=obj["deviceId"], rssi=obj["rssi"])


@dataclass
class KnownDevice:
    device_id: int
    project_name: str
    device_type_name: str
    friendly_name: str
    mac: str
    ip: Optional[str]
    age_ms: int
    # Defaults to "?" (not a required field) since a gateway running
    # firmware from before this field was added won't send it at all -
    # see gateway-firmware/src/main.cpp's fill_device_json/
    # device_announced, which only started including "firmwareVersion"
    # once RTSNOW_DeviceIdentity's own firmwareVersionMajor/Minor/Patch
    # (always present in the wire protocol, see PROTOCOL.md) got wired
    # through to the JSON output.
    firmware_version: str = "?"
    # Two distinct physical links, not two views of one number - wifi_rssi
    # is this node's own link to its Wi-Fi router (self-reported in
    # RTSNOW_Heartbeat), espnow_rssi is this node's link to the gateway
    # specifically (measured by the gateway itself via WiFi promiscuous
    # sniffing - see gateway-firmware/src/main.cpp's
    # on_wifi_promiscuous_rx()). Both None when the gateway hasn't sent a
    # reading yet (older firmware pre-dating this field, or no ESP-NOW
    # frame from this device sniffed yet) - see fill_device_json()'s
    # "omit, don't fake" comment for why that's "key absent", not 0.
    wifi_rssi: Optional[int] = None
    espnow_rssi: Optional[int] = None
    # Physical hardware variant (e.g. "AtomS3Lite", "M5Tough" - see
    # RTSNow-SPI-CCP's BOARD_NAME build flag), distinct from
    # device_type_name (the node's software role, e.g. "RTSNow-UNADYN") -
    # the same device_type_name can run on more than one physical board.
    # "?" when the gateway hasn't sent one yet (older node/gateway
    # firmware pre-dating this field).
    board_name: str = "?"
    # This device's own, locally-sniffed view of which OTHER devices it
    # can currently hear over ESP-NOW - feeds the gateway's own
    # recompute_routing() (see route_via_device_id below). Empty list
    # covers both "genuinely hears no one" and "older firmware that
    # doesn't report this" identically, same as every other tail-appended
    # heartbeat field.
    neighbors: list[NeighborLink] = field(default_factory=list)
    # None (the common case) means the gateway sends straight to this
    # device. Otherwise, the device_id of another known device the
    # gateway is currently relaying outbound commands (settings/reboot/
    # register-request) through instead, because gateway-firmware decided
    # it has a meaningfully better combined path - see that project's
    # recompute_routing() for exactly how. Outbound only: a relayed
    # command's reply may not make it back through the same path yet.
    route_via_device_id: Optional[int] = None

    @staticmethod
    def from_json(obj: dict) -> "KnownDevice":
        return KnownDevice(
            device_id=obj["deviceID"],
            project_name=obj["projectName"],
            device_type_name=obj["deviceTypeName"],
            friendly_name=obj["friendlyName"],
            mac=obj["mac"],
            ip=obj.get("ip"),
            age_ms=obj["ageMs"],
            firmware_version=obj.get("firmwareVersion", "?"),
            wifi_rssi=obj.get("wifiRssi"),
            espnow_rssi=obj.get("espNowRssi"),
            board_name=obj.get("boardName", "?"),
            neighbors=[NeighborLink.from_json(n) for n in obj.get("neighbors", [])],
            route_via_device_id=obj.get("routeViaDeviceId"),
        )


@dataclass
class PendingRequest:
    mac: str
    device_id: int
    project_name: str
    device_type_name: str
    friendly_name: str
    age_ms: int

    @staticmethod
    def from_json(obj: dict) -> "PendingRequest":
        return PendingRequest(
            mac=obj["mac"],
            device_id=obj["deviceID"],
            project_name=obj["projectName"],
            device_type_name=obj["deviceTypeName"],
            friendly_name=obj["friendlyName"],
            age_ms=obj["ageMs"],
        )
