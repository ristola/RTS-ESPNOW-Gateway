#!/usr/bin/env python3
"""Push firmware directly to a network-reachable RTS-NOW gateway (e.g. the
AtomS3 + PoE Base Ethernet gateway) over its own TCP JSON-lines server -
see gateway-firmware/src/main.cpp's "Firmware update over TCP" section
(handle_ota_start_command/handle_ota_chunk_command/handle_ota_end_command).
No ESP-NOW hop, no mac field, no relay - this connects straight to the
target gateway's own IP:5055 and flashes it directly, same dual-partition
safety as every other RTS-NOW OTA path (self_ota_end() only applies +
reboots after an MD5 match).

Usage:
    python3 gateway_tcp_ota_cli.py --host 10.13.1.198 --port 5055 --firmware path/to/firmware.bin
"""

import argparse
import hashlib
import json
import socket
import sys
import time

# 220 used to match the ESP-NOW mesh path's own RTSNOW_OtaChunk.data[220]
# limit (a real ~250-byte radio-payload cap) - copy-pasted here without
# actually applying to this TCP path at all, which has no such
# constraint. That mismatch alone turned an 813KB image into ~3700
# synchronous round-trips over what should be a fast local socket -
# see handle_ota_chunk_command()'s own comment in main.cpp (its buffer
# was bumped to match). Keep both in sync if this changes.
CHUNK_SIZE = 2048
RETRY_INTERVAL_S = 1.5
MAX_RETRIES = 6
POLL_INTERVAL_S = 0.01


class GatewayTcp:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.05)
        self._buf = b""
        self._id_counter = 0
        # Set once the socket is confirmably dead (a reset, not just a
        # read timing out) - see poll_lines()'s own comment. Once true,
        # neither send() nor poll_lines() will do anything further; the
        # caller (send_and_wait) checks this to stop retrying immediately
        # instead of raising on the next attempt to use a dead socket.
        self.closed = False

    def send(self, cmd: dict) -> int:
        self._id_counter += 1
        cmd = dict(cmd)
        cmd["id"] = self._id_counter
        if self.closed:
            return cmd["id"]
        try:
            self.sock.sendall((json.dumps(cmd) + "\n").encode("utf-8"))
        except OSError:
            self.closed = True
        return cmd["id"]

    def poll_lines(self) -> list[dict]:
        if self.closed:
            return []
        try:
            # Comfortably larger than one hex-encoded 2048-byte chunk line
            # (~4160 bytes with JSON overhead) - not required for
            # correctness (partial reads already accumulate in self._buf
            # across calls) but cuts the number of recv() calls needed per
            # line.
            chunk = self.sock.recv(8192)
            if chunk:
                self._buf += chunk
        except (socket.timeout, BlockingIOError):
            pass
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            # The gateway tore down the TCP connection abruptly - expected
            # when it was mid-reboot (an OTA success case, see
            # push_firmware()'s own handling of this exact situation),
            # ambiguous otherwise. Either way the socket is dead: mark it
            # so the caller stops retrying instead of raising again on the
            # next send().
            self.closed = True
            return []
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
        self.sock.close()


def send_and_wait(gw: GatewayTcp, cmd: dict, matches, description: str, log):
    attempts = 0
    gw.send(cmd)
    next_retry = time.time() + RETRY_INTERVAL_S
    while True:
        for obj in gw.poll_lines():
            result = matches(obj)
            if result is not None:
                return result
        if gw.closed:
            # The connection was reset, not just quiet - retrying would
            # just call send() on a dead socket for no benefit. Same "no
            # response" return as a plain retry-exhausted timeout; the
            # caller (push_firmware) already treats that as ambiguous-
            # but-likely-success on the ota_end step specifically.
            return None
        if time.time() >= next_retry:
            attempts += 1
            if attempts > MAX_RETRIES:
                return None
            log(f"  (no response yet, retry {attempts}/{MAX_RETRIES}: {description})")
            gw.send(cmd)
            next_retry = time.time() + RETRY_INTERVAL_S
        time.sleep(POLL_INTERVAL_S)


def push_firmware(gw: GatewayTcp, firmware_path: str, log=print, chunk_size: int = CHUNK_SIZE) -> tuple[bool, str]:
    with open(firmware_path, "rb") as f:
        data = f.read()
    if not data:
        return False, "Firmware file is empty"

    chunks = [data[i:i + chunk_size] for i in range(0, len(data), chunk_size)]
    md5 = hashlib.md5(data).hexdigest()
    log(f"Starting direct TCP OTA: {len(data)} bytes, {len(chunks)} chunks, md5={md5}")

    start_cmd = {"cmd": "ota_start", "size": len(data), "totalChunks": len(chunks), "md5": md5}
    ack = send_and_wait(
        gw, start_cmd,
        lambda obj: obj if obj.get("event") == "ack" and obj.get("cmd") == "ota_start" else None,
        "ota_start", log,
    )
    if ack is None:
        return False, "Gave up waiting for ota_start ack"
    if not ack.get("ok"):
        return False, f"Gateway rejected ota_start: {ack.get('error', '')}"

    for index, chunk in enumerate(chunks):
        chunk_cmd = {"cmd": "ota_chunk", "index": index, "dataHex": chunk.hex()}
        matcher = (lambda obj, index=index: obj if (
            obj.get("event") == "ota_chunk_ack" and obj.get("index") == index
        ) else None)
        ack = send_and_wait(gw, chunk_cmd, matcher, f"chunk {index + 1}/{len(chunks)}", log)
        if ack is None:
            return False, f"Gave up on chunk {index + 1}/{len(chunks)}"
        if not ack.get("ok"):
            ack = send_and_wait(gw, chunk_cmd, matcher, f"chunk {index + 1}/{len(chunks)} (rejected)", log)
            if ack is None or not ack.get("ok"):
                return False, f"Gateway kept rejecting chunk {index + 1}/{len(chunks)}"
        if (index + 1) % 50 == 0 or index + 1 == len(chunks):
            log(f"  {index + 1}/{len(chunks)} chunks acked ({100 * (index + 1) // len(chunks)}%)")

    log("All chunks sent - requesting verify + apply")
    end_cmd = {"cmd": "ota_end"}
    ack = send_and_wait(
        gw, end_cmd,
        lambda obj: obj if obj.get("event") == "ack" and obj.get("cmd") == "ota_end" else None,
        "ota_end", log,
    )
    if ack is None:
        # self_ota_end() reboots immediately on success without sending an
        # ack at all (see its own comment) - a timeout here, after every
        # chunk already acked ok, means success, not failure.
        return True, "No ota_end ack (expected on success - gateway already rebooting into new firmware)"
    if not ack.get("ok"):
        return False, f"Verification failed: {ack.get('error', '')}"
    return True, "Flashed successfully - gateway is rebooting into the new firmware"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="Gateway's IP, e.g. 10.13.1.198")
    ap.add_argument("--port", type=int, default=5055)
    ap.add_argument("--firmware", required=True, help="Path to a built firmware.bin")
    ap.add_argument("--chunk-size", type=int, default=CHUNK_SIZE,
                    help=f"Override for bootstrapping a target still running firmware with a smaller "
                         f"handle_ota_chunk_command buffer than this script's default ({CHUNK_SIZE})")
    args = ap.parse_args()

    gw = GatewayTcp(args.host, args.port)
    try:
        ok, message = push_firmware(gw, args.firmware, chunk_size=args.chunk_size)
        print(("SUCCESS: " if ok else "FAILED: ") + message)
        return 0 if ok else 1
    finally:
        gw.close()


if __name__ == "__main__":
    sys.exit(main())
