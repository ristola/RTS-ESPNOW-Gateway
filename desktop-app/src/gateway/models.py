from dataclasses import dataclass
from typing import Optional


@dataclass
class KnownDevice:
    device_id: int
    project_name: str
    device_type_name: str
    friendly_name: str
    mac: str
    ip: Optional[str]
    age_ms: int

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
