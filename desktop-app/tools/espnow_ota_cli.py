#!/usr/bin/env python3
"""Standalone ESP-NOW mesh OTA pusher - CLI equivalent of espnow_flash_page.py's
GUI flow, for scripted/headless testing without opening the desktop app.

Speaks the gateway's JSON-lines serial protocol directly (see
../src/gateway/serial_client.py and espnow_ota_transfer.py, which this
mirrors) - requires exclusive access to the gateway's USB serial port, so
the desktop app must not be connected to it at the same time.

Usage:
    python3 espnow_ota_cli.py --port /dev/cu.usbmodem2101 --mac 44:1B:F6:F6:9A:F8 --firmware path/to/firmware.bin
"""

import argparse
import hashlib
import json
import sys
import time

import serial

CHUNK_SIZE = 220  # must match RTSNOW_OtaChunk.data's size in rtsnow_protocol.h
RETRY_INTERVAL_S = 1.5
MAX_RETRIES = 6
POLL_INTERVAL_S = 0.05


class GatewaySerial:
    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self._buf = b""
        self._id_counter = 0

    def send(self, cmd: dict) -> int:
        self._id_counter += 1
        cmd = dict(cmd)
        cmd["id"] = self._id_counter
        self.ser.write((json.dumps(cmd) + "\n").encode("utf-8"))
        self.ser.flush()
        return cmd["id"]

    def poll_lines(self) -> list[dict]:
        n = self.ser.in_waiting
        if n:
            self._buf += self.ser.read(n)
        out = []
        while b"\n" in self._buf:
            raw, self._buf = self._buf.split(b"\n", 1)
            line = raw.decode(errors="replace").rstrip("\r").lstrip()
            if line.startswith("{"):
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    print(f"[warn] malformed JSON from gateway: {line!r}", file=sys.stderr)
        return out

    def close(self):
        self.ser.close()


def wait_for_known_devices(gw: GatewaySerial, timeout: float = 5.0) -> list[dict] | None:
    gw.send({"cmd": "list"})
    deadline = time.time() + timeout
    while time.time() < deadline:
        for obj in gw.poll_lines():
            if obj.get("event") == "known_devices":
                return obj.get("devices", [])
        time.sleep(POLL_INTERVAL_S)
    return None


def send_and_wait(gw: GatewaySerial, cmd: dict, matches, description: str, log):
    """Send `cmd`, retrying every RETRY_INTERVAL_S up to MAX_RETRIES times,
    until `matches(obj)` returns a truthy event dict. Returns that event, or
    None on giving up - mirrors EspNowOtaTransfer._arm_retry's own budget."""
    attempts = 0
    gw.send(cmd)
    next_retry = time.time() + RETRY_INTERVAL_S
    while True:
        for obj in gw.poll_lines():
            result = matches(obj)
            if result is not None:
                return result
        if time.time() >= next_retry:
            attempts += 1
            if attempts > MAX_RETRIES:
                return None
            log(f"  (no response yet, retry {attempts}/{MAX_RETRIES}: {description})")
            gw.send(cmd)
            next_retry = time.time() + RETRY_INTERVAL_S
        time.sleep(POLL_INTERVAL_S)


def push_firmware(gw: GatewaySerial, mac: str, firmware_path: str, log=print) -> tuple[bool, str]:
    with open(firmware_path, "rb") as f:
        data = f.read()
    if not data:
        return False, "Firmware file is empty"

    chunks = [data[i:i + CHUNK_SIZE] for i in range(0, len(data), CHUNK_SIZE)]
    md5 = hashlib.md5(data).hexdigest()
    log(f"Starting ESP-NOW OTA to {mac}: {len(data)} bytes, {len(chunks)} chunks, md5={md5}")

    start_cmd = {"cmd": "espnow_ota_start", "mac": mac, "size": len(data), "totalChunks": len(chunks), "md5": md5}
    ack = send_and_wait(
        gw, start_cmd,
        lambda obj: obj if obj.get("event") == "espnow_ota_start_ack" and obj.get("mac") == mac else None,
        f"OTA start to {mac}", log,
    )
    if ack is None:
        return False, "Gave up waiting for OTA start ack"
    if not ack.get("ok"):
        return False, f"Node rejected OTA start: {ack.get('message', '')}"

    for index, chunk in enumerate(chunks):
        chunk_cmd = {"cmd": "espnow_ota_chunk", "mac": mac, "index": index, "dataHex": chunk.hex()}
        matcher = (lambda obj, index=index: obj if (
            obj.get("event") == "espnow_ota_chunk_ack" and obj.get("mac") == mac and obj.get("index") == index
        ) else None)
        ack = send_and_wait(gw, chunk_cmd, matcher, f"chunk {index + 1}/{len(chunks)} to {mac}", log)
        if ack is None:
            return False, f"Gave up on chunk {index + 1}/{len(chunks)}"
        if not ack.get("ok"):
            # Immediate resend on an explicit rejection, same as the GUI's
            # on_chunk_ack "not ok" path - a node that rebooted mid-transfer
            # rejects every chunk until a fresh OTA start, so this still has
            # to be bounded rather than resending forever.
            ack = send_and_wait(gw, chunk_cmd, matcher, f"chunk {index + 1}/{len(chunks)} to {mac} (rejected)", log)
            if ack is None or not ack.get("ok"):
                return False, f"Node kept rejecting chunk {index + 1}/{len(chunks)}"
        if (index + 1) % 100 == 0 or index + 1 == len(chunks):
            log(f"  {index + 1}/{len(chunks)} chunks acked ({100 * (index + 1) // len(chunks)}%)")

    log("All chunks sent - requesting verify + apply")
    end_cmd = {"cmd": "espnow_ota_end", "mac": mac}
    ack = send_and_wait(
        gw, end_cmd,
        lambda obj: obj if obj.get("event") == "espnow_ota_end_ack" and obj.get("mac") == mac else None,
        f"OTA end to {mac}", log,
    )
    if ack is None:
        # Genuinely ambiguous, not a failure by default: rtsnow_node.cpp
        # (regular nodes) sends this ack BEFORE rebooting, so a missing ack
        # there usually does mean something went wrong. But another
        # gateway's own self_ota_end() (gateway-firmware/src/main.cpp)
        # calls ESP.restart() from inside itself on success, before the
        # caller ever reaches its own send_to() - so a target that's
        # ANOTHER gateway will never ack this step even on a clean success
        # (confirmed live: target's uptime reset to seconds immediately
        # after this exact timeout). Every chunk already acked ok by this
        # point either way - report that plainly and let the caller verify
        # independently (e.g. ping/re-announce) rather than asserting
        # either outcome with false confidence.
        return True, ("All chunks delivered; no OTA-end ack received - expected if the target is another "
                      "gateway (it reboots before it can ack), ambiguous if it's a regular node. Verify the "
                      "device came back up before trusting this.")
    if not ack.get("ok"):
        return False, f"Verification failed: {ack.get('message', '')}"
    return True, "Flashed successfully - node is rebooting into the new firmware"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="Gateway's USB serial port, e.g. /dev/cu.usbmodem2101")
    ap.add_argument("--mac", required=True, help="Target device MAC, e.g. 44:1B:F6:F6:9A:F8")
    ap.add_argument("--firmware", required=True, help="Path to a built firmware.bin")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--skip-known-check", action="store_true",
                    help="Skip the known_devices pre-check (e.g. gateway firmware too old to answer 'list')")
    args = ap.parse_args()
    mac = args.mac.upper()

    gw = GatewaySerial(args.port, args.baud)
    try:
        time.sleep(2)  # let the gateway's boot banner clear, same settle the GUI app allows on connect
        if not args.skip_known_check:
            devices = wait_for_known_devices(gw)
            if devices is None:
                print("[warn] no known_devices response - proceeding anyway", file=sys.stderr)
            else:
                macs = {d.get("mac") for d in devices}
                if mac not in macs:
                    print(
                        f"[error] {mac} is not currently a known device on this gateway - it must have announced "
                        f"itself over ESP-NOW at least once before it can be targeted this way. "
                        f"Known devices: {sorted(macs)}",
                        file=sys.stderr,
                    )
                    return 1
        ok, message = push_firmware(gw, mac, args.firmware)
        print(("SUCCESS: " if ok else "FAILED: ") + message)
        return 0 if ok else 1
    finally:
        gw.close()


if __name__ == "__main__":
    sys.exit(main())
