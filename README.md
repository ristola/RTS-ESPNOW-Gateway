# RTS ESP-NOW Gateway

A single M5Stack **AtomS3U** (USB-A dongle form factor, ESP32-S3), plugged
into a laptop, acting as a **project-agnostic ESP-NOW gateway** for every
RTS ESP project - not tied to any one product. Plug it in, open a serial
terminal, and:

1. **Discover** ESP-NOW devices from any RTS project - project, device
   type, friendly name, IP (if the device is Wi-Fi-joined), and MAC.
2. **Provision Wi-Fi** - send a fresh device's real SSID/password over
   ESP-NOW, so it can join your network without ever having credentials
   typed into it by hand.
3. **Push settings** - an extensible, project-defined key/value mechanism,
   so any project can add its own settable fields without this gateway's
   firmware ever needing to change.

See [`PROTOCOL.md`](PROTOCOL.md) for the full ESP-NOW wire-format design
and the reasoning behind it, [`SERIAL_PROTOCOL.md`](SERIAL_PROTOCOL.md) for
the JSON-lines USB serial protocol `desktop-app/` (below) speaks to this
firmware, and [`PROJECT_MEMORY.md`](PROJECT_MEMORY.md) /
[`TASKS.md`](TASKS.md) for how this project came to exist and what's left.

## Why a separate project

This gateway is meant to outlive any single product. Two existing/planned
RTS projects both need exactly this:

- **[`../CYD-4.3-FN-Tester`](../CYD-4.3-FN-Tester)** - has its own
  ESP-NOW protocol (ShackMate, `SM_`-prefixed) between its CYD touchscreen
  and an FN two-wire "pod." That protocol and hardware relationship are
  unaffected by this project.
- **[`../DryerHealth`](../DryerHealth)** - a resin-dryer monitoring app
  whose own `AGENTS.md` already planned for an "ATOM S3U USB-to-ESP-NOW
  gateway" as a system component, with an explicit rule not to bake
  ESP-NOW logic directly into the desktop app.

Rather than building a second, incompatible gateway per project, this repo
is the one shared implementation both (and any future project) can target.

## Hardware

M5Stack **AtomS3U** (ESP32-S3, USB-A dongle form factor).

| Function | Pin |
|---|---|
| Onboard RGB LED (single WS2812) | GPIO35 |
| Physical button | GPIO41 (reserved - not used by this firmware yet) |
| USB-A | native USB CDC (flashing + the Serial control interface) |

No dedicated PlatformIO board definition exists for the AtomS3U - see
`gateway-firmware/platformio.ini`'s header comment for why
`m5stack-atoms3` is used instead, and why that's safe.

This gateway never joins Wi-Fi itself - it only tunes its radio to one
fixed ESP-NOW channel of its own choosing (see the `channel` command
below), so it never gets an IP address and can't do network OTA. Reflash
over USB.

## Setup

```sh
cd gateway-firmware
pio run -e m5stack-atoms3u -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor
```

Check `ls /dev/cu.*` for the actual port - on macOS, the AtomS3U's native
USB-C/USB-A connection typically shows up as `/dev/cu.usbmodemXXXX` (VID
`303A`, Espressif's own - distinct from a CH340-based board's
`/dev/cu.usbserial-XXXX`).

## Serial commands

Type a command and press Enter in any serial terminal at 115200 baud.

| Command | Effect |
|---|---|
| `list` | Show known devices (from `RTSNOW_ANNOUNCE`/`RTSNOW_HEARTBEAT` traffic) |
| `pending` | Show devices currently waiting to be provisioned |
| `provision <mac> <ssid> <password>` | Accept a pending request, send it Wi-Fi credentials |
| `reject <mac>` | Decline a pending request |
| `setting <mac> <key> <value>` | Push a generic setting to a known device (auto-detects int/float/bool/string) |
| `reboot <mac>` | Ask a known device to restart |
| `poll_registers <mac>` | Ask a known device to report its register table, if it has one |
| `channel <1-11>` | Change and persist this gateway's own ESP-NOW channel |
| `discover` | Broadcast `RTSNOW_DISCOVER` immediately |
| `help` | Show this list |

**v1 limitation**: SSID/password/key/value must not contain spaces (naive
whitespace-tokenized command parsing) - see `TASKS.md`'s "not yet
designed" section for quoted-argument support. This limitation doesn't
apply to the JSON-lines interface below, which carries real JSON strings.

## Desktop app

`desktop-app/` is a cross-platform (Mac/Windows) PySide6 GUI - same stack
as `../DryerHealth`, and organized the same way (a navigable shell with
distinct sections, not one flat window) - that drives the gateway over
the JSON-lines protocol (`SERIAL_PROTOCOL.md`) instead of typing text
commands. A sidebar switches between four sections:

- **Dashboard** - connection status, gateway identity (device ID, MAC,
  channel, protocol version), and known/pending device counts. The MAC
  comes from the OS-reported USB serial number (no gateway command needed
  for it, unlike the rest - this hardware's native USB CDC reports its
  real MAC as that serial number, which is also why it matches the
  device ID exactly).
- **Node Network** - live known-devices and pending-requests tables;
  provision (with a dialog, not a hand-typed MAC) or reject a pending
  request; double-click (or "View Details") a known device to drill into
  its detail page.
- **Node Detail** - one device's full identity (including an editable
  friendly name - "Rename" pushes it as a `friendlyName` setting and
  waits for the node's own confirmation, not just the gateway's forwarding
  ack, before showing success), a settings-push form (no need to retype
  its MAC), and an OTA-update push (browse for a `.bin`, push it over
  Wi-Fi with a live progress bar - only enabled for a Wi-Fi-joined device;
  a mesh-only/`PAIRED` device has no IP to push to).
- **Flash Node** - USB-based onboarding for a brand-new node. "Scan for
  Nodes" reads each other connected board's chip MAC directly via esptool
  (works even with no firmware installed at all) and cross-checks it
  against the gateway's known-devices list - unrecognized MACs are
  flagged **unprovisioned**. Select one and "Flash RTS-NOW Node Firmware"
  runs `pio run -t upload` (defaults to this repo's own `node-firmware/`,
  changeable) with a live progress bar. The gateway's own port is never
  scanned or flashed.

An Activity Log / Raw Serial log stays visible under all four sections.

```sh
cd desktop-app
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python3 main.py
```

## Test node firmware

`node-firmware/` is a minimal RTSNOW_-speaking node (M5Stack AtomS3 Lite)
built purely to test the gateway against - **not** a template for a real
project's node. Two build modes, chosen via `kNeedsWifi` in
`node-firmware/src/main.cpp`:

- **Wi-Fi** (`kNeedsWifi = true`) - sweeps for a gateway, accepts
  provisioning, joins Wi-Fi, announces/heartbeats.
- **Mesh-only** (`kNeedsWifi = false`) - for deployments with no Wi-Fi
  network available at all. Broadcasts `RTSNOW_ANNOUNCE` immediately at
  boot and is recognized by the gateway with **zero operator step** -
  see `PROTOCOL.md`'s "Mesh-only pairing" section for why this needs no
  new wire message.

Either mode: hold the physical button 5 seconds, or send the `forget`
serial command, to erase any stored Wi-Fi credentials and reboot back
into a fresh pairing/provisioning attempt - useful for repeat testing
without reflashing.

### OTA updates (Wi-Fi only)

A `kNeedsWifi = true` node that's `JOINED` can be re-flashed over Wi-Fi
via standard `ArduinoOTA` - manually triggered, not automatic. The
easiest way is `desktop-app`'s Node Detail page (browse for a `.bin`,
push, watch the progress bar) - it reimplements the same small
UDP-invite + TCP-stream protocol natively (`src/gateway/ota_client.py`),
not a wrapper around any external tool. To push one from the command
line instead, the Arduino/ESP32 tooling's own `espota.py` works too
(a joined node advertises as `rtsnow-node-<deviceID>.local` on port 3232):

```sh
python3 $HOME/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
  -i <node-ip> -p 3232 -r -f .pio/build/m5stack-atoms3-lite-test-node/firmware.bin
```

OTA over ESP-NOW itself (for mesh-only nodes with no Wi-Fi at all) was
discussed and deliberately deferred - see `TASKS.md`'s "Not yet designed."
This board's default partition table already has the `otadata`/`app0`/
`app1` partitions OTA needs, so no partition changes were required.

## LED status (gateway)

No display on this board - the LED plus Serial is the only status
feedback.

- **Blue (solid)** - idle, ready, no pending requests.
- **Cyan (blinking)** - one or more devices waiting to be provisioned -
  see `pending`.
- **Green (flicker)** - receiving a message.
- **Red (flicker)** - transmitting a message.
- **Red (blinking, sustained)** - `esp_now_init()` failed (halts).
- A bright white flash at boot is an LED self-test, independent of the
  above.

## LED status (test node)

`node-firmware/`'s LED, in priority order (a higher item overrides a
lower one whenever both would otherwise apply):

- **White (fast blink)** - an OTA update is in progress. Don't power-cycle
  or hold the button while this shows.
- **Yellow (solid)** - the button is currently held (factory reset in
  progress - release before 5s to cancel).
- **Green (solid)** - `JOINED` - connected to Wi-Fi.
- **Blue (solid)** - `PAIRED` - recognized by the gateway over the mesh,
  no Wi-Fi (mesh-only mode).
- **Cyan (solid)** - `HELD` - a gateway replied to this node's
  provisioning request; awaiting credentials or rejection.
- **Yellow/orange (solid)** - `JOINING` - attempting `WiFi.begin()` with
  received credentials.
- **Dim blue (blinking)** - `SWEEPING` - broadcasting
  `RTSNOW_PROVISION_REQUEST` while cycling channels looking for a gateway.

## Status

Fully verified end-to-end on real hardware: gateway (AtomS3U) + test node
(AtomS3 Lite, `node-firmware/`) + `desktop-app/` completing the full
sweep -> hold -> provision -> join -> announce handshake and a settings
push, driven through the app's actual UI. See `PROTOCOL.md`'s "Status"
section, `SERIAL_PROTOCOL.md`'s "Status" section (including a real
Serial-corruption bug found and fixed during this testing), and
`TASKS.md`.

## Not yet designed

See `TASKS.md`'s "Not yet designed / open questions" - notably: a real
project's own production node firmware (`node-firmware/` is only a test
article), setting read-back, real encryption for Wi-Fi credential
provisioning, and DryerHealth's own desktop-side integration.
