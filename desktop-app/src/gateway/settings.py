from PySide6.QtCore import QSettings

# This app's first-ever persisted setting - no config file/QSettings usage
# existed anywhere before this. Org/app name pair below is what QSettings
# uses to pick the OS-native storage location (e.g. ~/Library/Preferences
# on macOS) - kept stable so a later run always finds the same store.


def _settings() -> QSettings:
    return QSettings("RTSNow", "RTS ESP-NOW Gateway")


def get_default_gateway_ip() -> str | None:
    """The IP of the network gateway to try first on startup, if the user
    has ever set one via "Set As Default Gateway" - None if never set."""
    value = _settings().value("network_gateway/default_ip", "", type=str)
    return value or None


def set_default_gateway_ip(ip: str | None):
    s = _settings()
    if ip:
        s.setValue("network_gateway/default_ip", ip)
    else:
        s.remove("network_gateway/default_ip")
