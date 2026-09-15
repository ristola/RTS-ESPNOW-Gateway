# Project Memory - RTS ESP-NOW Gateway

Working notes for picking this project back up across sessions. Not
end-user documentation (see `README.md` for that, once it exists) - this
is scratch/context for whoever (human or AI) resumes work here.

## What this project is

A single M5Stack **AtomS3U** (USB-A dongle form factor, ESP32-S3), plugged
into a laptop, acting as a **project-agnostic ESP-NOW gateway** for every
RTS ESP project - not tied to any one product. Its jobs, per explicit user
request:

1. Discover ESP-NOW devices on the air from *any* RTS project and show
   device/project/type/name/IP/MAC over USB serial.
2. Provision a fresh device's real Wi-Fi SSID/password over ESP-NOW (not
   just an ESP-NOW channel number).
3. Push "many other settable settings a particular project needs" - a
   generic, project-defined key/value mechanism, not a fixed schema.

See `PROTOCOL.md` for the actual wire-format design and `TASKS.md` for
current status/next steps.

## How this project came to exist (context for the design choices below)

This started, in the same conversation, as a much narrower thing: a
"USB to ESP-NOW interface" firmware built *inside*
`../CYD-4.3-FN-Tester/AtomS3U-ESPNOW-Bridge/`, speaking that project's own
`SM_`-prefixed "ShackMate" protocol, purely to observe that one project's
mesh (CYD tester + FN two-wire pod) over Serial. That version was actually
flashed and boot-tested on real AtomS3U hardware (channel sweep/pairing
state machine confirmed working over a raw-pyserial capture).

Two things then reframed the whole approach, both direct user feedback
mid-session:

1. A detour into `../DryerHealth` (a separate, real product - industrial
   resin dryer monitoring) revealed that project's own `AGENTS.md` already
   plans for "ATOM S3U USB-to-ESP-NOW gateway" as a first-class system
   component, with an explicit rule: *"Use RTS, not ShackMate"* for
   naming, and *"Desktop app should not directly contain ESP-NOW logic"* -
   i.e. that project already expected a gateway abstraction, just not
   built yet.
2. The user then explicitly said they want **all** their ESP projects to
   be able to use one AtomS3U for discovery + Wi-Fi/password provisioning
   + arbitrary per-project settings - and suggested, correctly, that this
   belongs in its own standalone project rather than living inside
   CYD-4.3-FN-Tester.

Asked directly (`AskUserQuestion`) whether the new gateway should replace
or coexist with the CYD-4.3-FN-Tester-specific bridge: **replace** -
generalize what was already built, with project-neutral naming, rather
than maintaining two parallel USB-ESP-NOW-bridge concepts. Also confirmed
scope: **discovery + Wi-Fi provisioning + generic settings**, not just
discovery alone.

Consequence: `../CYD-4.3-FN-Tester/AtomS3U-ESPNOW-Bridge/` is being
removed (see that project's own `PROJECT_MEMORY.md` for the removal
entry) - this repo is its generalized successor, not a sibling. Two
small, decoupled fixes made to CYD-4.3-FN-Tester along the way were kept
rather than reverted, since they're independently correct regardless of
this gateway's existence: `SM_DeviceIdentity` gained an `ipv4Address`
field (the CYD now advertises its real Wi-Fi IP over its own ShackMate
protocol), and its pairing-confirmation dialog title was generalized off a
hardcoded device-type-specific string.

## Key design decisions and why

- **Free-text `projectName`/`deviceTypeName`, not an enum.** ShackMate's
  `SM_DeviceType` is a closed, append-only enum - fine for one project's
  own device taxonomy, unworkable for a gateway meant to serve projects
  that don't exist yet. This is the single choice that makes "one gateway,
  many projects" real rather than aspirational.
- **Real Wi-Fi credentials, not just a channel.** ShackMate's provisioning
  handshake deliberately never touched Wi-Fi (its pod never joins Wi-Fi at
  all, by design - see that project's `ESPNOW_PROTOCOL.md`). This gateway
  inverts that: handing over real SSID/password is the explicit point,
  since a node needs to actually join the plant/lab Wi-Fi network, not
  just share an ESP-NOW channel with one other device.
- **No persisted "paired devices" list on the gateway.** The CYD needed
  one (to drive its own Diagnostics UI, survive reboots, etc.). This
  gateway's job is closer to stateless: discover whoever's currently on
  the air, provision whoever's currently asking. Whatever persistence
  matters (a node's own Wi-Fi credentials) lives on the node, not here.
  Simpler firmware, nothing to get out of sync.
- **Multiple concurrent pending provisioning requests, not one modal
  dialog.** The CYD's touchscreen UI could only sensibly show one "Pair?"
  popup at a time. A serial/text interface has no such constraint - `pending`
  can list several at once, addressed individually by MAC via
  `provision <mac> ...`.
- **Broadcast-based confirmation, not a unicast ACK, carried over from
  ShackMate's hard-won lesson.** See `PROTOCOL.md`'s "Why broadcast, not
  ACK" - a real, previously-debugged ESP32-S3 hardware finding (synchronous
  unicast sent from inside an ESP-NOW receive callback is unreliable;
  broadcast isn't), designed around from the start here instead of
  rediscovering it. This protocol doesn't even define a
  `RTSNOW_PROVISION_ACK` message, unlike ShackMate's (which kept one only
  as an unreliable best-effort secondary signal).
- **Credentials sent unencrypted - documented, not hidden.** ESP-NOW's
  per-peer AES needs both sides pre-registered as encrypted peers before
  the first encrypted packet, which is impossible for two devices that
  have never talked before (ShackMate hit this exact wall and reverted).
  Unlike ShackMate (which no longer carries a real secret once reverted),
  this protocol *does* carry a real Wi-Fi password unencrypted - flagged
  explicitly in `PROTOCOL.md` as a real, currently-accepted exposure
  (brief window, requires an explicit operator action), not swept under
  the rug. Revisit if this is ever exposed to an untrusted RF environment.
- **Human-typeable serial commands, not a structured protocol, for v1.**
  The gateway needs *some* way for an operator to approve/decline a
  pending request and supply credentials, and there's no touchscreen here
  (unlike the CYD). Plain text commands over serial (`provision <mac>
  <ssid> <password>`) work immediately via any terminal with zero extra
  tooling, and are trivially scriptable later. A stricter structured
  protocol (JSON-lines is the leading candidate) for a future desktop app
  to drive this programmatically is explicitly deferred, not designed yet.

## Companion projects

- **`../CYD-4.3-FN-Tester`** - has its own working ShackMate protocol
  (CYD touchscreen <-> FN two-wire pod) that is **not** being replaced or
  touched by this gateway - only the short-lived `AtomS3U-ESPNOW-Bridge`
  add-on inside that repo is superseded. That project's FN pod is a
  candidate for an eventual RTSNOW_-speaking node firmware, but that
  hasn't been started.
- **`../DryerHealth`** - the project whose `AGENTS.md` already anticipated
  an "ATOM S3U USB-to-ESP-NOW gateway" and an `src/hardware/` abstraction
  for it. Explicitly **not** wired up yet - this session's direction was
  to get the gateway/protocol itself solid first, defer DryerHealth's
  desktop-side integration (and its dryer-node firmware, which also
  doesn't exist yet) to a follow-up.

## Session log

- **2026-09-12**: Repo created. Designed and documented the RTSNOW_
  protocol (`PROTOCOL.md`) and wrote the shared header
  (`gateway-firmware/src/rtsnow_protocol.h`). Firmware
  (`gateway-firmware/src/main.cpp`) and this repo's own `README.md` not
  yet written as of this entry - see `TASKS.md` for the exact next steps.
- **2026-09-12 (continued)**: `platformio.ini`, `main.cpp`, and `README.md`
  written (a different session/pass than the entry above - `TASKS.md` had
  drifted out of sync with the repo, listing these as not-yet-started when
  they were already done; reconciled here). Build-verified clean
  (`pio run -e m5stack-atoms3u`: SUCCESS), flashed to real AtomS3U hardware
  over `/dev/cu.usbmodem2101`, and smoke-tested over raw pyserial (using
  PlatformIO's bundled penv Python, since the system Python lacks
  `pyserial`) - `help`, `list`, `pending`, `discover`, and `channel <n>`
  (including NVS persistence round-trip) all confirmed working over real
  Serial. Gateway firmware side of this project is now solid per
  `TASKS.md`'s original plan; remaining work is the CYD-4.3-FN-Tester
  cleanup and everything under "Not yet designed." Confirmed the
  CYD-4.3-FN-Tester cleanup was already done (that repo's own
  `PROJECT_MEMORY.md` documents it) - every item originally planned for
  this repo is now complete.
- **2026-09-12 (later)**: User asked for a Python desktop front end "like
  we started with DryerHealth" (PySide6, matching that project's stack).
  Two decisions up front via `AskUserQuestion`: the app lives in this repo
  (`desktop-app/`, project-agnostic like the gateway itself, not folded
  into DryerHealth) and the gateway's text-only serial interface gets a
  parallel structured JSON-lines protocol first (`SERIAL_PROTOCOL.md`) -
  the "not yet designed" item from `PROTOCOL.md`/`TASKS.md` - rather than
  having the app scrape human-readable text output.

  Added the JSON protocol to `gateway-firmware/src/main.cpp` (new
  `bblanchon/ArduinoJson` dependency): a line is JSON if its first
  non-whitespace character is `{`, otherwise it's the existing text
  parser - both interfaces stay live on the same serial stream
  simultaneously. Refactored `cmd_provision`/`cmd_reject`/`cmd_setting`/
  `cmd_channel`'s bodies into shared `do_provision`/`do_reject`/
  `do_setting_send`/`do_channel` core functions so neither interface
  duplicates the actual ESP-NOW send logic - each interface just handles
  its own input parsing and success/error presentation. One deliberate
  protocol design choice: the JSON `setting` command takes an explicit
  `valueType` field rather than sniffing the value's type (the text
  interface's `cmd_setting` guesses bool/int/float/string from the typed
  string) - a client always knows what it means to send, so there's
  nothing to guess. Build-verified and smoke-tested directly against real
  gateway hardware (`/dev/cu.usbmodem2101`) before writing any Python.

  Then scaffolded `desktop-app/` (PySide6, mirroring DryerHealth's
  `src/ui/`+`main.py` layout): `src/gateway/serial_client.py`
  (`GatewayClient(QThread)` - single background thread doing both the
  blocking pyserial read loop and draining an outgoing command queue, so
  there's no cross-thread read/write races on the `Serial` object),
  `src/gateway/models.py` (`KnownDevice`/`PendingRequest` dataclasses),
  `src/gateway/ports.py` (port discovery via `pyserial`'s `list_ports`,
  sorting Espressif VID `0x303A` ports first as a hint, not a filter -
  can't distinguish the gateway from a node by VID/PID alone since both
  are AtomS3-family boards), `src/ui/dialogs.py` (`ProvisionDialog`), and
  `src/ui/main_window.py` (the main window: port/connect bar, channel +
  discover controls, known-devices/pending-requests tables with a 2s poll
  timer, provision/reject actions, a generic settings-push form, and a
  two-tab activity/raw log). Verified headless
  (`QT_QPA_PLATFORM=offscreen`, since this environment has no display) by
  scripting the actual UI callbacks against the real gateway - connect,
  `list`/`pending` polling, `discover`, and `channel` apply (with the
  `channel_changed` event round-tripping back into the UI's spinbox) all
  confirmed working end-to-end through the app, not just the underlying
  client library.

  Mid-session, the user plugged in a second board - an **M5Stack AtomS3
  Lite** (`/dev/cu.usbmodem1101`) - specifically to test with, since no
  RTSNOW_-speaking node firmware has ever existed anywhere (see
  `PROTOCOL.md`'s "Status"). Built `node-firmware/` for it: a minimal
  test-only node (explicitly not a template for a real project's node) -
  sweep/hold/join/announce/heartbeat state machine, NVS-persisted
  credentials, a `forget` command to reset for repeat testing, copying
  (not sharing) `rtsnow_protocol.h` per `CYD-4.3-FN-Tester`'s own
  precedent for multiple copies of a protocol header.

  First full run surfaced a real bug: the gateway received the node's
  `RTSNOW_PROVISION_REQUEST` correctly, but the `pending`/`provision_request`
  JSON line arriving at a script reading the gateway's serial port was
  visibly garbled (chunks of characters missing) - not a parsing bug on
  the Python side, actual corrupted bytes on the wire. Root-caused to the
  gateway's ESP-NOW receive callback running in the Wi-Fi driver's own
  FreeRTOS task rather than `loop()`'s, and calling `Serial.print*`
  directly from there for `device_announced`/`provision_request`/
  `provision_confirmed`/`setting_ack`. Tried `Serial.flush()` after each
  print first (cheap, obvious guess) - did **not** fix it, which ruled out
  a simple TX-buffer-overflow explanation and confirmed it was a
  scheduling/context problem. Fixed with a small mutex-protected queue
  (`queue_output`/`drain_output_queue`, guarded by a `portMUX_TYPE`
  critical section) - the receive callback only ever enqueues a
  pre-rendered `String`; only `loop()` ever actually calls `Serial.print`.
  Confirmed clean, uncorrupted output across several repeated full-handshake
  test runs after this fix - this is the kind of hardware-specific lesson
  this project's docs exist to carry forward (see `PROTOCOL.md`'s own
  "Why broadcast, not ACK" section for the precedent this follows: don't
  do risky synchronous work from inside an ESP-NOW callback).

  With that fixed, ran the complete provisioning handshake end-to-end
  twice: once via raw pyserial against both the gateway's and node's ports
  directly (asked the user for a real test Wi-Fi SSID/password rather than
  guessing/fabricating one), and once driving `desktop-app/`'s actual UI
  code (pending table -> Provision dialog, with `ProvisionDialog`
  monkey-patched to auto-fill/accept since this environment has no
  display) - sweep, hold, `provision` (real credentials), join,
  `device_announced`, `provision_confirmed`, and a `setting` push with its
  `setting_ack` all confirmed working, both at the protocol level and
  through the actual GUI. This is the first time any RTSNOW_ message has
  been exchanged with a second real device - previously only the gateway's
  own state machine had been verified.
- **2026-09-12 (later still)**: User raised a real deployment gap: some
  installs have no Wi-Fi network available at all, so a node must be able
  to power up, announce its type, and get recognized by the gateway
  automatically depending purely on the RTS-NOW (ESP-NOW) mesh - no Wi-Fi,
  no operator approval step. Two design questions asked via
  `AskUserQuestion` before touching any code, since getting wire-protocol
  semantics wrong is expensive to unwind later: (1) reuse the existing
  `RTSNOW_ANNOUNCE` broadcast for this (**chosen**) vs. add a new explicit
  join/pair handshake message pair; (2) make Wi-Fi-vs-mesh-only a fixed
  per-node-build choice (**chosen**) vs. something negotiated at runtime.
  The reuse choice works because the gateway's `note_known_device` already
  auto-adds *any* `RTSNOW_ANNOUNCE`/`RTSNOW_HEARTBEAT` sender to its
  known-devices table with no operator gate - that gate exists specifically
  on the Wi-Fi provisioning handshake because *that* one hands over a real
  secret (a Wi-Fi password); a mesh-only node never carries one, so there
  was nothing to protect and no reason to add a second handshake. Documented
  this reasoning in `PROTOCOL.md`'s new "Mesh-only pairing (no Wi-Fi)"
  section so a future session doesn't have to rediscover it.

  Implemented in `node-firmware/src/main.cpp`: a `kNeedsWifi` compile-time
  flag choosing between the existing Wi-Fi flow and a new
  `State::Paired`/`enter_paired_mesh_only()` path that broadcasts
  `RTSNOW_ANNOUNCE` (ip=0) immediately at boot, skipping Wi-Fi entirely.
  Confirmed on real hardware: node boots straight to `PAIRED`, gateway's
  `list` shows it immediately with no `ip` and no approval needed.

  Mid-turn, user separately asked for a 5-second button-hold factory reset
  on the node (GPIO41) specifically to test re-pairing repeatedly without
  reflashing - same effect as the existing `forget` serial command (erase
  stored Wi-Fi creds + reboot), just usable with no terminal attached.
  Added LED feedback (solid yellow while held, three white flashes right
  before rebooting) so a press without a serial monitor open is still
  legible. **Verified with an actual physical button press** (not
  simulated/scripted, unlike most of this session's other hardware
  verification) - confirmed reboot + immediate re-pairing, gateway's `list`
  showing the device again with `ageMs` reset to a few hundred ms.
- **2026-09-12 (even later)**: User asked about OTA over the RTS-NOW
  protocol itself, specifically for deployments with no Wi-Fi. Answered as
  an exploratory question first (feasible, but ESP-NOW's 250-byte payload
  cap means chunking/retry/integrity-check complexity and a meaningfully
  slower transfer than Wi-Fi OTA, plus the same unencrypted-in-transit
  posture as Wi-Fi credential provisioning) rather than starting to build
  anything. Follow-up question about updating "just the RTS-NOW protocol
  part" without touching a node's own application code led to explaining
  the real answer: ESP32 OTA replaces the whole app image, there's no
  partial/in-place patch - but factoring the protocol into its own
  versioned library (stable API, node's application code never has to
  change) gets the same practical benefit, and a binary-diff/delta scheme
  (`bsdiff`-style) could shrink the actual transferred bytes later if
  needed. User then explicitly deferred ESP-NOW OTA entirely and asked for
  **Wi-Fi OTA only**, manually triggered, plus an LED indicator during the
  update - a good example of scoping down from an ambitious ask to what's
  actually needed once the tradeoffs were visible.

  Implemented standard `ArduinoOTA` in `node-firmware/` (`begin_ota()`,
  started once per Wi-Fi join in `on_join_success()`) - confirmed the
  board's default partition table (`m5stack-atoms3`'s `default.csv`)
  already has `otadata`/`app0`/`app1`, so no partition changes were
  needed. Added a new highest-priority LED state (fast white blink,
  `s_ota_in_progress`) and disabled the factory-reset button check while
  an OTA is running, to avoid a stray press interrupting a flash.

  Verified for real: flipped `kNeedsWifi` to `true` temporarily, joined
  the node to the same `RISTOLA` network as the earlier provisioning test,
  confirmed `ArduinoOTA` came up (`rtsnow-node-<deviceID>.local`), then
  pushed a real update via `espota.py` against the node's IP with a small
  version-marker text change - watched the live `OTA: starting` ->
  `0%..100%` -> `complete - rebooting` sequence over serial, then
  confirmed the new banner text was actually printed post-reboot (proof
  the new code was running, not just a successful-looking reboot) and that
  the node auto-rejoined Wi-Fi. Reverted the test marker and `kNeedsWifi`
  back to `false` (the project's default) afterward.

  Hit a real, unrelated hardware snag restoring that default over USB:
  after several OTA-triggered soft-reboots, the AtomS3 Lite's native
  USB-CDC connection got into a state where `esptool` could connect and
  start its flasher stub but then lost the data channel ("No serial data
  received"), reproducibly, even at a lower baud rate - not a firmware
  bug, a known category of native-USB-CDC quirk after repeated resets.
  Asked the user to physically unplug/replug the board; a normal USB
  re-enumeration fixed it immediately and the reflash then succeeded on
  the first try. Worth remembering: if a USB upload to either board in
  this project starts failing with "No serial data received" after the
  board has been through several soft-resets (OTA, `ESP.restart()`, etc.),
  try a physical unplug/replug before spending time debugging esptool
  flags.
- **2026-09-12 (final)**: User asked to make `desktop-app/` "like
  DryerHealth" but for discovering/managing RTS-NOW nodes specifically -
  DryerHealth's app is a navigable multi-section shell (Dashboard, Dryer
  Network, etc.), not the one flat window `desktop-app/` had been so far.
  Asked two questions via `AskUserQuestion` before restructuring: sidebar
  nav with Dashboard/Node Network/Node Detail pages (**chosen**, over
  keeping one window with tabs), and whether to fold the OTA work from
  earlier into this pass as a real "push an update from the app" action
  (**chosen**, over leaving OTA as a manual `espota.py` step).

  Restructured `src/ui/` into `main_window.py` (now just the shell -
  connection bar, sidebar, stacked pages, persistent log, and all the
  `GatewayClient`/OTA-client plumbing) plus a new `pages/` package with
  one file per section. Node Detail's settings form no longer needs a
  manually-typed MAC (scoped to whichever device was clicked into) - a
  real usability improvement that fell out of the restructure, not
  something separately requested.

  Wrote `src/gateway/ota_client.py`'s `OtaPushClient` by reading the
  actual `espota.py` bundled with the ESP32 Arduino framework
  (`$PLATFORMIO/packages/framework-arduinoespressif32/tools/espota.py`)
  and reimplementing its exact wire protocol (UDP invite with
  `f"{cmd} {port} {size} {md5}\n"`, wait for "OK", accept a TCP
  connection, stream 1024-byte chunks each waiting for an "OK" ack)
  natively rather than shelling out to that script - gives real Qt
  `progress`/`finished` signals instead of parsing subprocess text.

  Headless testing caught a real bug immediately: the sidebar's
  `currentRowChanged` was connected and its initial row set *inside*
  `_build_sidebar()`, which ran before `_build_pages()` had created
  `self.pages` - fired `_on_sidebar_row_changed` with `self.pages` not
  existing yet, `AttributeError` on every launch. Fixed by moving the
  signal connection and initial `setCurrentRow(0)` to after both the
  sidebar and pages exist, in `_build_ui`. A good reminder that even
  "just a UI restructure" needs the same headless-first verification
  pass as firmware changes in this project - this one would have broken
  on first real-world launch otherwise.

  Verified end-to-end against real hardware twice: once against the
  gateway + the already-flashed mesh-only test node (Dashboard/Node
  Network populated correctly, Node Detail showed "mesh-only, no Wi-Fi"
  and correctly left the OTA button disabled), and once - since
  `OtaPushClient` was new code, not just a reused, already-proven
  script - a full live OTA push driven through the actual app: flipped
  the node back to `kNeedsWifi=true`, discovered it had already
  auto-rejoined Wi-Fi using credentials still stored in NVS from the
  earlier OTA testing (reflashing the app partition doesn't touch NVS),
  built a version-marker firmware, and pushed it via the app's real
  "Push Update" button - watched live progress (0%→99%→success) and,
  monitoring the node's serial throughout, saw the actual
  `OTA: 0%..100%` sequence and the new banner text after reboot. Two
  earlier attempts at this same test appeared to fail
  ("QThread: Destroyed while thread '' is still running", `SIGABRT`) -
  turned out to be the *test harness* quitting the Qt event loop before
  the background `OtaPushClient` thread had finished, not a real bug;
  fixed the test by waiting for the actual `finished` signal instead of
  a fixed timer before calling `app.quit()`. Reverted the node to its
  default mesh-only build afterward, confirmed back in `PAIRED` state.
- **2026-09-12 (last)**: Launched `desktop-app/` visibly for the user
  (not headless) for the first time this session - immediately surfaced a
  real usability problem: the user connected it to the node's port
  (`/dev/cu.usbmodem1101`) instead of the gateway's, and the Raw Serial
  log filled with `Unknown command "{"cmd":"..."` (the node's text parser
  choking on JSON it can't understand). User asked for an easier way to
  identify the AtomS3U, then - mid-turn, before that was finished - refined
  it into the real requirement: the app should only ever connect to an
  actual gateway.

  First tried the more obvious fix: a distinct USB product string per
  board (`USB_PRODUCT`/`USB_MANUFACTURER` build-time macros) so
  `pio device list`/the port dropdown would show something other than the
  generic "USB JTAG/serial debug unit" both boards share. Traced why it
  had zero effect after flashing: `m5stack-atoms3`'s own board manifest
  (`~/.platformio/platforms/espressif32/boards/m5stack-atoms3.json`) bakes
  in `-DARDUINO_USB_MODE=1`, which selects the ESP32-S3's fixed-function
  "USB Serial/JTAG" hardware peripheral (a simple debug/programming
  interface with a descriptor hardcoded in ROM/IDF) instead of the
  software TinyUSB CDC stack that `USB_PRODUCT`/the `ESPUSB` class's
  runtime API actually configure - confirmed by reading `USB.cpp`/`main.cpp`
  in the arduino-esp32 core: with `ARDUINO_USB_CDC_ON_BOOT=1` (which this
  project needs for Serial to attach at boot at all), the framework's own
  `app_main()` calls `Serial.begin()`+`USB.begin()` *before* our `setup()`
  runs, and separately, the JTAG/serial peripheral mode doesn't read those
  macros at all regardless of timing. Fixing this for real would mean
  switching to `ARDUINO_USB_MODE=0` - a real risk to already-hard-won
  USB flashing/reset stability (see the "No serial data received" incident
  earlier this session) - not attempted. Reverted the dead-end build flags
  from both `platformio.ini` files rather than leave inert config that
  looks like it fixes something it doesn't; documented the finding in
  both files' comments in case a future session wants to revisit switching
  USB modes deliberately.

  Pivoted fully to the protocol-level fix, which is both more robust and
  what the user actually asked for: `desktop-app/`'s `MainWindow` now
  sends `{"cmd":"list"}` immediately upon connecting and starts a 3-second
  verification timer; the first well-formed JSON event received (any
  event at all) confirms a real gateway, since node-firmware has no JSON
  emitter under any circumstance. No JSON within the timeout auto-
  disconnects with a clear `QMessageBox` warning instead of leaving the
  user staring at empty tables and a wall of "Unknown command" text.
  Verified both directions against real hardware: connecting to the
  node's port now auto-rejects within 3s; connecting to the gateway
  verifies almost immediately and works exactly as before. This is a
  good instance of the general lesson from `PROTOCOL.md`'s "why free-text
  identity, not a closed enum" and the OTA-callback-context bug earlier
  this session: don't trust what the OS/environment *reports* about a
  device's identity when the protocol itself can prove it authoritatively.

  Immediately after that, launching the real (now-fixed) app surfaced a
  second, related gap: Dashboard's Device ID/Channel/Protocol version
  fields stayed "-" forever. Root cause: `hello` had only ever been a
  boot-time-only, unprompted event (`gateway-firmware`'s `setup()`) - a
  client connecting after boot (the normal case, and now unavoidable since
  the app used to connect and immediately re-probe with `list`) had no way
  to ever see one. Fixed by factoring the boot-time `hello` construction
  into `send_hello_event()` and adding a matching `hello` *command* to
  `handle_json_line`, then switching the app's post-connect verification
  probe from `{"cmd":"list"}` to `{"cmd":"hello"}` - one message now both
  proves gateway-ness (same as before) and populates the Dashboard
  immediately, instead of leaving it permanently blank outside the
  fraction-of-a-second boot window. Verified on real hardware: fresh
  `hello` on demand, Dashboard populated correctly right after connecting.

  Also hit (and worked around, not a code bug) a real hardware snag mid-
  session: reflashing the gateway failed twice with `esptool` errors
  ("Packet content transfer stopped") - turned out to be the running
  desktop app itself holding `/dev/cu.usbmodem2101` open (the user had it
  connected live in the GUI while a reflash was attempted underneath it).
  `lsof /dev/cu.usbmodem2101` confirmed the app's PID; killing it freed
  the port and the reflash succeeded immediately after. Worth remembering
  for next time: if a gateway/node reflash fails with a serial-transfer
  error (as opposed to the earlier "No serial data received" USB-CDC-stale
  issue), check whether the desktop app - or anything else - has that port
  open first, before assuming it's a hardware/USB problem.
- **2026-09-12 (next)**: User asked how to add RTS-NOW to a real customer
  project - "M5 Tough... running the Customer Project/PolymerPak
  project" - found it at
  `../../Customer Projects/PolymerPak` (a solar-tracker controller,
  classic ESP32 via M5Unified, README says "Solar Panel Tracking
  Controller" though the project itself is branded PolymerPak in its UI -
  already Wi-Fi-connected under its own logic, confirmed live at
  `10.13.73.65` via its own status webpage). Two questions asked via
  `AskUserQuestion` before touching a live device's code: skip
  provisioning entirely since it manages its own Wi-Fi (**chosen**), and
  confirmed USB access - user's first answer here ("It is setup currently
  for OTA if you") was garbled/cut off, asked a follow-up, checked the
  actual source and the live device's own HTTP response directly (both
  matched, no OTA present) rather than going back and forth further - a
  good example of resolving an ambiguous answer by checking ground truth
  instead of re-asking indefinitely.

  Added `rtsnow_node.h`/`.cpp` (copied `rtsnow_protocol.h`) exposing just
  `rtsnowNodeBegin()`/`rtsnowNodeLoop()` - three lines added to `main.cpp`,
  zero changes to existing display/NTP/web-server logic. This is the
  first time RTSNOW_ has been added to a project outside
  RTS-ESPNOW-Gateway itself - a real test of whether the "small versioned
  library, node's own code doesn't have to change" design (discussed
  earlier, during the OTA conversation) actually holds up. It did.

  Mid-turn, user asked to also add Wi-Fi OTA "NOW" before physically
  retrieving the device, specifically to avoid a future retrieval trip -
  reused the exact `ArduinoOTA` pattern from `node-firmware`, adapted for
  a display instead of an LED (M5Tough has a screen, not an RGB LED) -
  `drawOtaScreen()` shows progress on-screen, `otaInProgress` short-
  circuits the normal dashboard redraw so it isn't clobbered.

  Hit a real, non-obvious bug while trying to verify the channel over
  Serial: zero bytes ever came back, over windows up to 90s, streamed
  live. Root-caused by reading M5Unified's own source
  (`M5Unified.cpp`/`.hpp`): `M5.config()`'s `serial_baudrate` defaults to
  `0`, and `M5.begin()` only calls `Serial.begin()` when that's nonzero -
  this project had never once used Serial (only ever `M5.Display`), so
  Serial had literally never been started, in this project's whole
  history, until our new `Serial.printf` calls tried to use it. Fixed
  with `cfg.serial_baudrate = 115200` in `main.cpp`'s existing
  `M5.config()` call (not a redundant standalone `Serial.begin()`, to
  stay inside M5Unified's own init flow). Even after the fix, direct
  Serial capture still returned nothing in this environment (tried
  pyserial at multiple window lengths, and `pio device monitor`, which
  hit the same headless-terminal limitation already known from other
  boards) - concluded this is an environment/driver quirk with this
  board's CH9102 chip specifically, not a firmware problem, since the
  device's own web server and (after the fix) real ESP-NOW visibility
  both worked correctly. Pivoted to a more reliable verification method:
  had the **gateway itself** scan channels 1-11 (cycling the `channel`
  command, dwelling ~11s per channel - longer than the node's 10s
  heartbeat interval, so it's guaranteed to catch a heartbeat if that's
  the right channel) instead of depending on the node's own Serial
  output at all. Found PolymerPak on channel 1 (the gateway's own
  default) on the very first try. Worth remembering: when a board's
  Serial output can't be trusted, the gateway's own `channel` command can
  be used as a channel-scanning tool against ESP-NOW traffic directly -
  more reliable than fighting a flaky serial connection.
- **2026-09-12 (last)**: User plugged in a genuinely new/blank board and
  asked for a way to onboard it - detect it's not yet running RTS-NOW and
  flash it - directly from the desktop app. Two design questions via
  `AskUserQuestion`: esptool-based MAC read (works with zero firmware
  installed - talks to the ROM bootloader) cross-checked against the
  gateway's `known_devices` (**chosen** over any alternative), and a new
  4th "Flash Node" sidebar page (**chosen** over folding it into Node
  Network).

  Built `src/gateway/node_flasher.py` (`ChipMacReader`, `NodeFlasherClient`
  running `pio run -t upload` as a subprocess and parsing esptool's own
  progress percentage lines) and `FlashNodePage`. One safety property
  baked in from the start, not bolted on after: the gateway's own
  connected port is excluded from every scan and can never be flashed -
  esptool's MAC read resets whatever's on the other end of a port, which
  would have knocked the live gateway connection offline.

  Verified completely end-to-end against the real new board (a
  different AtomS3-family unit, distinct MAC from every previously-known
  device): scan correctly flagged it unprovisioned (and gracefully
  produced "unreadable" for random unrelated macOS serial pseudo-ports
  like Bluetooth-Incoming-Port, rather than erroring), flashing via the
  app's own "Flash RTS-NOW Node Firmware" button ran cleanly to 100%, and
  the board came up in `PAIRED (mesh-only)` and was immediately visible in
  the gateway's `known_devices` - the full loop, from "unrecognized USB
  device" to "known RTS-NOW node," entirely through the app's UI with no
  manual `pio`/`esptool` commands needed. One test-harness lesson worth
  keeping: an early attempt at this same verification appeared to hang
  with the progress bar stuck, which turned out to be the *test script*
  clicking Flash before the scan had actually finished (an un-stubbed
  `QMessageBox.information` on "no row selected" silently ate the
  headless click) - not a real app bug. Scanning every available port
  (including irrelevant ones like Bluetooth) is slow in practice, since
  each dead end has to time out against esptool's own retry logic before
  moving to the next port.
- **2026-09-12 (final)**: With two test nodes now running side by side
  (the original one plus the one just onboarded via Flash Node), user
  spotted a real, user-facing naming problem from a screenshot of the
  Known Devices table: both rows showed `Project: GatewayTest` and an
  identical `Name: AtomS3-Lite-01`. Initial diagnosis before seeing the
  screenshot was wrong - guessed it was about port-dropdown ambiguity
  (gateway vs. node ports being visually identical, which is also real,
  see the "USB descriptor naming" entry above) and started building a
  `GatewayProbe` class in `serial_client.py` to proactively label port
  entries. The screenshot showed the actual complaint was simpler and
  different: `node-firmware`'s hardcoded `projectName` field literally
  says `"GatewayTest"`, which reads in the UI as if these nodes *are*
  gateways - only the AtomS3U should be. Removed the not-actually-
  requested `GatewayProbe` code rather than leave it half-wired-in once
  the real ask became clear via the screenshot - a good example of not
  guessing ahead of the evidence, and not being precious about work
  already started once it turns out to solve the wrong problem.

  Fixed the actual issue in `make_identity()`: `projectName` "GatewayTest"
  -> "RTSNOW-Test", and `friendlyName` "AtomS3-Lite-01" (hardcoded,
  identical on every unit) -> `"TestNode-XXXX"` (low 16 bits of
  `deviceID`, unique per board). Reflashed the one test node that
  happened to be on USB at the time and confirmed the corrected identity
  live in the gateway's `known_devices`; the *other* test node
  (`44:1B:F6:F6:9A:F8`) wasn't plugged in, so it's still running the old
  strings until next reflashed - tracked in `TASKS.md`.
- **2026-09-12 (still later)**: From that same screenshot, user asked to
  also show the gateway's MAC on the Dashboard, under Device ID. Realized
  this needs no firmware change at all: this hardware's native USB CDC
  reports its real MAC as the OS-level USB serial number (confirmed by
  noticing the existing deviceID, `0x7DCDE700`, is exactly the low 4
  bytes of the port's own `SER=50:78:7D:CD:E7:00`). Added `serial_number`
  to `SerialPortInfo` and wired it into the Dashboard on connect - a nice
  example of a requested feature turning out to be a pure app-side change
  once the underlying data was recognized as already available.
- **2026-09-12 (latest)**: User asked for node renaming from Node Detail -
  edit the friendly name, push it, have the node retain it. Recognized
  this maps directly onto the existing generic-settings mechanism (no new
  wire message needed) via a reserved `"friendlyName"` key, matching what
  `PROTOCOL.md`'s own struct comment had already used as an example
  before this feature existed. Implemented in `node-firmware`:
  `s_friendly_name` buffer, `load_friendly_name()`/`save_friendly_name()`
  (`Preferences`/NVS, same pattern as Wi-Fi credentials), and the
  `RTSNOW_SET_SETTING` handler now queues a rename (applied from `loop()`,
  same deferred-from-callback pattern as everything else) when it sees
  that key. App side: Node Detail's friendly name field became editable
  with a "Rename" button.

  This session's persistence-testing attempts had left the test node
  intermittently unresponsive (see below) - worked around it by testing
  rename over raw JSON first (worked, confirmed via a fresh `list`), then
  discovered via a genuine, user-initiated unplug/replug that the name
  really had persisted in NVS across an actual power cycle (the node's
  boot banner printed the saved name) - the definitive proof, arrived at
  by accident rather than by design, but no less solid for it.

  That same recovery cycle surfaced a real, separate gap: a *second*
  rename attempt (sent while the node was already stuck/unresponsive)
  still came back with `ok:true` from the gateway. Traced this to the
  gateway's `setting` command ack only confirming the ESP-NOW send call
  succeeded locally, not that the node actually received or applied
  anything - the real confirmation is the independent `setting_ack`
  event, which never arrived for that attempt. This is exactly the kind
  of gap that's easy to miss without a flaky node handy to expose it - a
  case where a real-world hardware hiccup made the software more
  correct, not just more tested. Fixed by having the app track
  `setting_ack` with its own 5s timeout (`MainWindow._pending_renames`)
  instead of trusting the immediate ack, with distinct UI states ("Sent -
  awaiting node confirmation...", "Confirmed by node.", "No confirmation
  received - node may be offline."). Documented the ack-vs-confirmation
  distinction in both protocol docs so it's not rediscovered later.
  Verified the full corrected flow twice more on real (now-healthy)
  hardware: once over raw JSON, once through the actual app UI end-to-end
  showing the "Confirmed by node." state.

  Also now have a fuller explanation for the recurring "node stops
  responding on USB and stops heartbeating" pattern seen a few times this
  session: a *failed* `pio run -t upload` attempt still resets the board
  into the ROM bootloader as step one (that's how esptool always starts,
  succeed or fail) - so several failed attempts in a row plausibly left
  the chip sitting in the bootloader, not running any application at all,
  explaining both the silent Serial and the frozen ESP-NOW heartbeats
  simultaneously. Not a new failure mode, just a better-understood one -
  the fix is still the same physical unplug/replug.
- **2026-09-12 (next day)**: User reported rename "not working" across
  all three known devices - turned out to be three unrelated causes, not
  one bug, and untangling them was the actual work:
  1. `AtomS3-Lite-01` (old test node) - still running firmware from
     before the rename feature existed. Expected; just needs a reflash
     (blocked on the same USB-wedge issue - couldn't get a clean upload
     even though the device was clearly alive and heartbeating fine over
     ESP-NOW, confirming the wedge is USB-flashing-specific, not a sign
     the device itself is unhealthy).
  2. `PolymerPak Tracker` - a real, previously-unnoticed gap: its own
     `rtsnow_node.cpp` (written earlier this session) never actually
     implemented the friendlyName convention - the comment even said so
     ("Not yet wired to anything") - it just ack'd every setting without
     applying one. Fixed to match `node-firmware`'s exact approach, and
     - since PolymerPak already had OTA from earlier - pushed the fix
     over Wi-Fi instead of needing physical access to a customer's
     already-deployed device again. This is the first time this
     project's own OTA capability was used to fix something, rather than
     just to prove OTA itself worked.
  3. A separate, correctly-working node (MAC `...6F:78`) whose rename
     really did work earlier - the user likely hadn't waited a full ~10s
     heartbeat cycle for the change to show up in a subsequent `list`,
     which is a real, if minor, UX rough edge worth remembering: renaming
     appears to "do nothing" for up to one heartbeat interval after the
     `setting_ack` confirms it, because `known_devices` is a snapshot
     that only updates when a fresh announce/heartbeat arrives, not
     synchronously with the ack.

  Also delivered a `desktop-app/RTS ESP-NOW Gateway.app` launcher (plain
  `Info.plist` + a shell-script `CFBundleExecutable`, no PyInstaller/
  py2app needed) so the app can be double-clicked/Dock-launched - a
  genuinely different kind of request from this session's usual
  firmware/protocol work, handled with the simplest thing that actually
  satisfies "clickable launch item" on macOS rather than reaching for
  heavier packaging tooling.
