import hashlib
import socket

from PySide6.QtCore import QThread, Signal

# Reimplements the same small UDP-invite + TCP-stream protocol as the
# Arduino/ESP32 tooling's own espota.py (see
# $PLATFORMIO/packages/framework-arduinoespressif32/tools/espota.py for the
# canonical reference) - not a wrapper around that script, so progress and
# errors integrate directly with Qt signals instead of parsing subprocess
# text output. No password/auth support: node-firmware never calls
# ArduinoOTA.setPassword(), so this always sends the no-auth invite.
_FLASH_COMMAND = 0
_DEFAULT_OTA_PORT = 3232
_INVITE_TIMEOUT_S = 10
_INVITE_RETRIES = 10
_CHUNK_SIZE = 1024


class OtaPushClient(QThread):
    """Pushes one firmware image to one ArduinoOTA-enabled node over Wi-Fi."""

    progress = Signal(int)  # 0-100
    finished = Signal(bool, str)  # success, message

    def __init__(self, node_ip: str, file_path: str, ota_port: int = _DEFAULT_OTA_PORT):
        super().__init__()
        self._node_ip = node_ip
        self._file_path = file_path
        self._ota_port = ota_port

    def run(self):
        try:
            with open(self._file_path, "rb") as f:
                content = f.read()
        except OSError as exc:
            self.finished.emit(False, f"Could not read {self._file_path}: {exc}")
            return

        content_size = len(content)
        file_md5 = hashlib.md5(content).hexdigest()

        listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            listen_sock.bind(("0.0.0.0", 0))
            listen_sock.listen(1)
        except OSError as exc:
            self.finished.emit(False, f"Could not open a local port: {exc}")
            return
        local_port = listen_sock.getsockname()[1]

        if not self._send_invitation(local_port, content_size, file_md5):
            listen_sock.close()
            return

        listen_sock.settimeout(_INVITE_TIMEOUT_S)
        try:
            connection, _ = listen_sock.accept()
        except socket.timeout:
            self.finished.emit(False, "Node accepted the invitation but never connected back")
            listen_sock.close()
            return
        listen_sock.close()

        self._stream_file(connection, content, content_size)

    def _send_invitation(self, local_port: int, content_size: int, file_md5: str) -> bool:
        message = f"{_FLASH_COMMAND} {local_port} {content_size} {file_md5}\n".encode()
        invite_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        invite_sock.settimeout(_INVITE_TIMEOUT_S)
        try:
            for _ in range(_INVITE_RETRIES):
                invite_sock.sendto(message, (self._node_ip, self._ota_port))
                try:
                    reply = invite_sock.recv(64).decode(errors="replace")
                except socket.timeout:
                    continue
                if reply == "OK":
                    return True
                self.finished.emit(False, f"Node rejected the update invitation: {reply!r}")
                return False
        finally:
            invite_sock.close()
        self.finished.emit(False, "No response from node - is it still on Wi-Fi and JOINED?")
        return False

    def _stream_file(self, connection: socket.socket, content: bytes, content_size: int):
        try:
            offset = 0
            last_ok = False
            while offset < content_size:
                chunk = content[offset:offset + _CHUNK_SIZE]
                connection.sendall(chunk)
                offset += len(chunk)
                self.progress.emit(int(offset * 100 / content_size))
                try:
                    connection.settimeout(10)
                    reply = connection.recv(16).decode(errors="replace")
                    last_ok = "OK" in reply
                except socket.timeout:
                    last_ok = False

            if not last_ok:
                # The node may still send a final result after the last chunk.
                connection.settimeout(30)
                for _ in range(5):
                    try:
                        reply = connection.recv(32).decode(errors="replace")
                    except socket.timeout:
                        break
                    if "OK" in reply:
                        last_ok = True
                        break

            if last_ok:
                self.finished.emit(True, "Update sent - node is rebooting into the new firmware")
            else:
                self.finished.emit(False, "Node did not confirm the update completed")
        except OSError as exc:
            self.finished.emit(False, f"Transfer error: {exc}")
        finally:
            connection.close()
