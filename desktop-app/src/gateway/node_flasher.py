import glob
import os
import re
import shutil
import subprocess
import sys

from PySide6.QtCore import QThread, Signal


def find_esptool() -> str | None:
    candidates = glob.glob(os.path.expanduser("~/.platformio/packages/tool-esptoolpy*/esptool.py"))
    return candidates[0] if candidates else None


def find_pio() -> str | None:
    """Resolves the `pio` CLI's full path rather than trusting bare "pio" +
    inherited PATH - a GUI app (this one, launched outside an interactive
    shell) often gets a minimal PATH that doesn't include wherever pio
    actually lives (PlatformIO's own venv, or a package-manager-installed
    copy on a different PATH entry), even though it resolves fine from a
    terminal. Checks PlatformIO's own bundled install first (present on any
    machine that's ever run `pio` at all, regardless of how it was invoked),
    then falls back to whatever the current PATH happens to offer."""
    bundled = os.path.expanduser("~/.platformio/penv/bin/pio")
    if os.path.isfile(bundled):
        return bundled
    return shutil.which("pio")


def list_pio_environments(project_dir: str) -> list[str]:
    """Parses `[env:name]` sections out of a platformio.ini - lets the UI
    default sensibly (auto-select the only environment) and offer a choice
    when a project defines more than one, without hardcoding any project's
    environment name here."""
    ini_path = os.path.join(project_dir, "platformio.ini")
    try:
        with open(ini_path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return []
    return re.findall(r"^\[env:([^\]]+)\]", text, re.MULTILINE)


class ChipMacReader(QThread):
    """Reads one connected ESP32/ESP32-S3's burned-in base MAC address via
    esptool's `read_mac` - talks to the ROM bootloader directly, so it
    works even with no application firmware installed at all. That's
    exactly what lets the app recognize a brand-new, never-flashed node:
    if this MAC doesn't match anything in the gateway's known_devices
    list (populated over ESP-NOW), it's never announced RTSNOW_ - either
    it's unprovisioned, or it's some unrelated device on this port.
    """

    result = Signal(str, object)  # port, mac (str, upper-cased "AA:BB:..." form) or None on failure

    def __init__(self, port: str):
        super().__init__()
        self._port = port

    def run(self):
        esptool = find_esptool()
        if esptool is None:
            self.result.emit(self._port, None)
            return
        try:
            proc = subprocess.run(
                [sys.executable, esptool, "--port", self._port, "read_mac"],
                capture_output=True, text=True, timeout=15,
            )
        except (subprocess.TimeoutExpired, OSError):
            self.result.emit(self._port, None)
            return

        match = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", proc.stdout)
        self.result.emit(self._port, match.group(1).upper() if match else None)


class NodeFlasherClient(QThread):
    """Runs `pio run -t upload` against a PlatformIO project, targeting one
    serial port - used to flash RTS-NOW node firmware onto a newly
    detected, unprovisioned device. Uses `pio` itself (not raw esptool)
    because PlatformIO's own upload already correctly writes
    bootloader+partitions+boot_app0+app together when needed for a truly
    blank chip - reimplementing that multi-region flash sequence
    ourselves would just be redoing what `pio` already does correctly.
    """

    progress = Signal(int)  # 0-100, parsed from esptool's own "(NN %)" progress lines
    log_line = Signal(str)
    finished = Signal(bool, str)

    _PERCENT_RE = re.compile(r"\((\d{1,3})\s*%\)")

    def __init__(self, project_dir: str, port: str, environment: str | None = None):
        super().__init__()
        self._project_dir = project_dir
        self._port = port
        self._environment = environment

    def run(self):
        pio = find_pio()
        if pio is None:
            self.finished.emit(False, "Could not find the `pio` CLI - is PlatformIO installed?")
            return

        cmd = [pio, "run", "-t", "upload", "--upload-port", self._port]
        if self._environment:
            cmd += ["-e", self._environment]

        try:
            proc = subprocess.Popen(
                cmd, cwd=self._project_dir,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1,
            )
        except OSError as exc:
            self.finished.emit(False, f"Could not start pio: {exc}")
            return

        for line in proc.stdout:
            line = line.rstrip("\n")
            self.log_line.emit(line)
            match = self._PERCENT_RE.search(line)
            if match:
                self.progress.emit(min(int(match.group(1)), 100))

        proc.wait()
        if proc.returncode == 0:
            self.progress.emit(100)
            self.finished.emit(True, "Flashed successfully")
        else:
            self.finished.emit(False, f"pio exited with code {proc.returncode} - see log")
