# Tasks - RTS ESP-NOW Gateway

Working checklist for this project. Not end-user documentation (see
`README.md` for that, once it exists) - this tracks what's done and what's
next across sessions.

## Done

- [x] Decided this should be its own standalone, project-neutral repo
      (not folded into CYD-4.3-FN-Tester or DryerHealth) - it's meant to
      serve *every* RTS ESP project, present and future.
- [x] Designed the RTSNOW_ wire protocol (`PROTOCOL.md`): discovery,
      real Wi-Fi credential provisioning (not just a channel number, per
      explicit request), and a generic typed key/value settings push.
      Free-text `projectName`/`deviceTypeName` in `RTSNOW_DeviceIdentity`
      instead of a closed enum - the load-bearing choice that lets new
      projects show up with zero changes to this repo.
- [x] Wrote `gateway-firmware/src/rtsnow_protocol.h` matching the spec.

- [x] Wrote `gateway-firmware/platformio.ini` (board: M5Stack AtomS3U -
      no dedicated `m5stack-atoms3u` PlatformIO board exists,
      `m5stack-atoms3` is the correct substitute, confirmed pin-compatible
      - see the file's own header comment).
- [x] Wrote `gateway-firmware/src/main.cpp`: fixed operator-chosen
      ESP-NOW channel persisted via NVS (default 1), discovery table,
      multi-slot pending-provisioning tracking by MAC, full serial command
      interface (`list`, `pending`, `provision`, `reject`, `setting`,
      `channel`, `discover`, `help`), and LED status (blue idle, cyan
      blink = pending, red/green flicker = tx/rx).
- [x] Build-verified (`pio run -e m5stack-atoms3u`) - SUCCESS, 14.4% RAM /
      21.6% flash. Flashed to real AtomS3U hardware over
      `/dev/cu.usbmodem2101` (VID:PID `303A:1001`, confirms native USB
      JTAG/serial) and smoke-tested over raw pyserial (PlatformIO's bundled
      penv has pyserial; system Python doesn't) - **2026-09-12**:
      `help`/`list`/`pending`/`discover`/`channel <n>` all confirmed
      working over real Serial. `pio device monitor` still doesn't work
      headless in this environment - see CYD-4.3-FN-Tester's
      `feedback_hardware_debugging` memory note; use raw pyserial instead.
- [x] Wrote this repo's top-level `README.md` (concept, setup, full
      serial command reference, hardware notes).

- [x] In `CYD-4.3-FN-Tester`: removed the superseded `AtomS3U-ESPNOW-Bridge/`
      folder and reverted the `SM_DEVICE_USB_ESPNOW_BRIDGE` enum addition
      (confirmed - folder no longer exists, that repo's own
      `PROJECT_MEMORY.md` documents the pivot in detail). Kept the two
      decoupled fixes (`SM_DeviceIdentity.ipv4Address`, generalized pairing-
      dialog title) as intended.
- [x] Designed and built a JSON-lines USB serial protocol
      (`SERIAL_PROTOCOL.md`) alongside the existing human text commands in
      `gateway-firmware/src/main.cpp` (detects `{` as the first
      non-whitespace character to route a line to the JSON parser instead
      of the text one) - `list`/`pending` snapshots, `provision`/`reject`/
      `setting`/`channel`/`discover` commands with `id`-echoing `ack`
      replies, and unsolicited events (`hello`, `device_announced`,
      `provision_request`/`_expired`, `provision_confirmed`/
      `_confirm_timeout`, `setting_ack`, `channel_changed`). Added
      `bblanchon/ArduinoJson@^7.2` as a dependency. Refactored the
      provision/reject/setting/channel command bodies into shared
      `do_*` core functions so both interfaces call the same logic.
      Build-verified and smoke-tested on real hardware (`/dev/cu.usbmodem2101`) -
      **2026-09-12**: snapshots, acks (success and failure paths),
      `channel_changed`, malformed-JSON handling, and the legacy text
      interface (still works unaffected) all confirmed.
- [x] Scaffolded `desktop-app/` - a PySide6 GUI (same stack as
      `../DryerHealth`) speaking the JSON-lines protocol: port picker,
      connect/disconnect, known-devices and pending-requests tables (2s
      poll + push-event-triggered refresh), provision dialog (SSID/
      password, no whitespace restriction unlike the text interface),
      reject confirmation, generic settings-push form, channel control,
      discover button, and an activity log + raw-serial log. Verified
      headless (`QT_QPA_PLATFORM=offscreen`) against the real gateway on
      `/dev/cu.usbmodem2101` - **2026-09-12**: connect, `list`/`pending`
      polling, `discover`, and `channel` apply (with `channel_changed`
      round-trip) all confirmed working end-to-end through the app.

- [x] Built `node-firmware/` - a minimal RTSNOW_-speaking test node (M5Stack
      AtomS3 Lite, `/dev/cu.usbmodem1101`) - **not** a template for a real
      project's node, purely to test the gateway against. State machine:
      Sweeping (broadcasts `RTSNOW_PROVISION_REQUEST` cycling channels
      1-11) -> Held (gateway replied `_HOLD`) -> Joining (received
      `_CREDENTIALS`, `WiFi.begin()`) -> Joined (`RTSNOW_ANNOUNCE` then
      periodic `RTSNOW_HEARTBEAT`; accepts `RTSNOW_SET_SETTING`, always
      replies `RTSNOW_SETTING_ACK`). Wi-Fi credentials persist in NVS
      across reboots; `forget` (serial command) erases them for repeat
      testing. Copied `rtsnow_protocol.h` rather than sharing it (matches
      CYD-4.3-FN-Tester's own precedent of multiple copies).
- [x] **Full end-to-end provisioning handshake verified on real hardware**
      (gateway + test node + `desktop-app/`) - **2026-09-12**: sweep -> hold
      -> `provision` command (with real Wi-Fi credentials the user
      supplied) -> node joins -> `device_announced` + `provision_confirmed`
      both fire correctly, and a `setting` push is accepted by the node
      (`setting_ack` fires). Verified twice: once via raw pyserial against
      both ports directly, once driving the actual `desktop-app/` UI code
      (pending table -> Provision dialog -> provisioned device appears in
      Known Devices with its real IP).
- [x] **Found and fixed a real bug during this testing**: the gateway's
      ESP-NOW receive callback runs in the Wi-Fi driver's own FreeRTOS task,
      not `loop()`'s - calling `Serial.print*` directly from there
      (`device_announced`/`provision_request`/`provision_confirmed`/
      `setting_ack`) intermittently corrupted output on real native-USB-CDC
      hardware (bytes silently dropped mid-line, first noticed as garbled
      JSON the desktop app's client couldn't parse). `Serial.flush()` right
      after each print did **not** fix it, ruling out a simple buffering
      explanation. Fix: a small mutex-protected queue (`queue_output`/
      `drain_output_queue` in `gateway-firmware/src/main.cpp`) - the
      receive callback only ever enqueues a pre-rendered line; only
      `loop()` ever actually writes to `Serial`. Confirmed clean output
      across multiple repeated full-handshake runs after the fix.
- [x] **Mesh-only pairing** (`node-firmware/`'s `kNeedsWifi = false` mode) -
      explicit requirement: some deployments have no Wi-Fi network to hand
      out at all, so a node must still power up, announce itself, and be
      recognized by the gateway automatically over ESP-NOW alone. Design
      decisions locked in via `AskUserQuestion`: reuse `RTSNOW_ANNOUNCE`
      as-is rather than a new join/pair handshake (the gateway already
      auto-accepts any announcing device, no operator step - see
      `PROTOCOL.md`'s new "Mesh-only pairing" section), and make
      Wi-Fi-vs-mesh-only a fixed per-build choice, not runtime-negotiated.
      Added `State::Paired` (mesh-only's equivalent of `Joined`) and
      `enter_paired_mesh_only()` (broadcasts `RTSNOW_ANNOUNCE` immediately
      at boot, no Wi-Fi attempt at all). **2026-09-12**: confirmed on real
      hardware - node boots straight to `PAIRED`, gateway's `list` shows it
      immediately with no `ip` field and no approval needed.
- [x] **Button-hold factory reset** (GPIO41, held 5s) on `node-firmware/` -
      erases stored Wi-Fi credentials and reboots, same effect as the
      `forget` serial command but usable with no terminal attached; added
      specifically to test re-pairing/re-provisioning repeatedly. LED
      turns solid yellow while held (feedback that the press registered)
      and flashes white 3x right before rebooting. **2026-09-12**:
      confirmed with a real physical button press (not simulated) - node
      rebooted and immediately re-paired, gateway's `list` showed it again
      with `ageMs` reset to a few hundred ms.
- [x] **OTA updates over Wi-Fi** (`node-firmware/`, `kNeedsWifi = true`
      only) - discussed OTA over ESP-NOW itself first (for mesh-only
      nodes with no Wi-Fi); user chose to scope this pass down to
      Wi-Fi-only, manually-triggered OTA, deferring ESP-NOW OTA (see "Not
      yet designed" below). Standard `ArduinoOTA` in `begin_ota()`,
      started once in `on_join_success()`; this board's default partition
      table already has `otadata`/`app0`/`app1` so no partition changes
      were needed. Added an LED state (`s_ota_in_progress`, checked first
      in `update_led()` - fast white blink) and disabled the factory-reset
      button check while an OTA is in progress (don't let a stray press
      interrupt a flash). **2026-09-12**: verified with a real OTA push
      via `espota.py` against the node's real IP - watched the full
      `OTA: starting` -> `OTA: 0%..100%` -> `OTA: complete - rebooting`
      sequence live over serial, then confirmed the **new** firmware
      (a version-marker banner change) was actually running post-reboot
      and had automatically rejoined Wi-Fi. Reverted the test marker and
      `kNeedsWifi` back to `false` afterward and reflashed over USB to
      restore the node's default mesh-only state - hit a real, unrelated
      snag doing that reflash (USB-CDC got into a stale state after
      several OTA-triggered reboots, `esptool` connected but then lost the
      data channel even at a lower baud) - resolved by a physical
      unplug/replug, not a firmware fix.
- [x] **Restructured `desktop-app/` into a DryerHealth-style navigable
      shell** (sidebar + stacked pages, not one flat window), per explicit
      request via `AskUserQuestion`: **Dashboard** (connection/gateway
      status, node counts), **Node Network** (known-devices/pending
      tables, provision/reject, drill into a device), **Node Detail**
      (one device's full identity, settings push scoped to it - no more
      hand-typing a MAC - and an OTA push). New files:
      `src/ui/pages/{dashboard,node_network,node_detail}_page.py` and
      `src/gateway/ota_client.py` (`OtaPushClient(QThread)` - a native
      reimplementation of the Arduino/ESP32 tooling's own `espota.py`
      UDP-invite + TCP-stream protocol, studied from that script directly,
      not a subprocess wrapper around it - gives real Qt progress signals
      instead of parsing text output). `main_window.py` is now the shell:
      owns the `GatewayClient`/poll timer/OTA client, dispatches events to
      whichever page needs them.
      **2026-09-12**: found and fixed a real init-order bug during headless
      testing (the sidebar fired its selection-changed signal, wired to
      switch pages, before `self.pages` existed yet - `AttributeError` on
      first launch) - fixed by wiring the signal only after both the
      sidebar and the stacked pages exist. Verified headless against real
      hardware: Dashboard/Node Network populated correctly from the real
      gateway, double-click navigation into Node Detail showed correct
      identity (including "mesh-only, no Wi-Fi" for our `PAIRED` test
      node), and the new `OtaPushClient` itself (not just the underlying
      protocol) was verified with a real end-to-end app-driven OTA push -
      flipped the node to `kNeedsWifi=true` temporarily, provisioned it,
      pushed a version-marker build through the app's own Node Detail
      "Push Update" button, watched 0%→100% progress and a success result,
      and confirmed the new banner text was actually running post-reboot.
      Reverted the node to its default mesh-only build afterward.
- [x] **Gateway vs. node port confusion** - real mix-up hit in practice
      (connected `desktop-app/` to the node's port, got a wall of
      `Unknown command "{"cmd":"..."` in Raw Serial since the node has no
      JSON parser at all). Tried a distinct USB product string per board
      first (`USB_PRODUCT`/`USB_MANUFACTURER` build-time macros) - **doesn't
      work** on the `m5stack-atoms3` board: its manifest bakes in
      `ARDUINO_USB_MODE=1`, which routes USB through the ESP32-S3's
      fixed-function "USB Serial/JTAG" peripheral (descriptor hardcoded in
      ROM/IDF, no app-level override) rather than the software TinyUSB CDC
      stack those macros actually configure - would need
      `ARDUINO_USB_MODE=0` to fix for real, a real risk to the
      already-hard-won-stable flashing/reset behavior, not attempted.
      Reverted that dead-end change (see both `platformio.ini`'s comments).
      Fixed properly at the protocol level instead, per explicit request
      ("make it so our Desktop App will only connect to our Gateway
      Device"): `desktop-app/` now sends `{"cmd":"list"}` immediately on
      connect and starts a 3s verification timer - any well-formed JSON
      event proves it's a real gateway (a node can never produce one); no
      JSON within the timeout auto-disconnects with a clear warning dialog
      instead of silently polling a device that'll never answer
      meaningfully. See `SERIAL_PROTOCOL.md`'s new "Identifying a gateway
      (vs. a node)" section. **2026-09-12**: verified both directions on
      real hardware - connecting to the node's port now auto-rejects
      within 3s with a warning dialog; connecting to the real gateway
      verifies almost immediately and behaves normally.
- [x] **`hello` is now also a command, not just a boot-time event** -
      launching the real app surfaced the gap: Dashboard's Device ID/
      Channel/Protocol version stayed "-" forever, because `hello` had
      only ever been emitted once, unprompted, at gateway boot - a client
      connecting afterward (the normal case) had no way to ever see one.
      Factored the boot-time `hello` construction into `send_hello_event()`
      in `gateway-firmware/src/main.cpp`, called both at boot and from a
      new `hello` command in `handle_json_line`. `desktop-app` now sends
      `{"cmd":"hello"}` (instead of `{"cmd":"list"}`) as its post-connect
      identity-verification probe, which doubles as populating the
      Dashboard immediately on every connect. **2026-09-12**: verified on
      real hardware - `{"cmd":"hello"}` returns a fresh `hello` event on
      demand, and the Dashboard now populates correctly right after
      connecting instead of showing "-" indefinitely.
- [x] **RTS-NOW added to a real customer project** (M5Stack Tough,
      "PolymerPak" solar-tracker controller,
      `../../Customer Projects/PolymerPak`) - the first time this
      protocol has been added to anything outside this repo. It already
      manages its own Wi-Fi independently, so the integration skips
      provisioning entirely: a new `rtsnow_node.h`/`.cpp` module (copied
      `rtsnow_protocol.h`) exposes just `rtsnowNodeBegin()`/
      `rtsnowNodeLoop()`, wired into `main.cpp` with three added lines
      total, zero changes to its existing display/NTP/web-server logic.
      Also added Wi-Fi OTA (`ArduinoOTA`, on-screen progress instead of an
      LED - this board has a display, not one) per explicit request, so
      it won't need to be physically retrieved again. Found and fixed a
      real bug in the process: this project never called `Serial.begin()`
      anywhere (M5Unified's `cfg.serial_baudrate` defaults to `0`, so
      `M5.begin()` silently never starts it) - our new debug prints were
      going nowhere; fixed via `cfg.serial_baudrate = 115200`. Verified
      real discovery from the gateway (found on channel 1, matching the
      gateway's default) after Serial monitoring turned out to be
      unreliable for this board in this environment (unrelated
      driver/environment quirk, not a firmware bug - the web server and
      real ESP-NOW discovery both confirmed the firmware works).
- [x] **"Flash Node" - USB-based node onboarding from the desktop app**,
      per explicit request after plugging in a genuinely new/blank board:
      `src/gateway/node_flasher.py` adds `ChipMacReader` (reads a
      connected board's chip MAC via `esptool read_mac` - works with zero
      firmware installed, talks to the ROM bootloader directly) and
      `NodeFlasherClient` (runs `pio run -t upload` as a subprocess,
      parsing esptool's own `(NN %)` progress lines - deliberately not a
      raw esptool reimplementation like `ota_client.py`, since `pio`
      already correctly handles the multi-region bootloader+partitions+
      app write a truly blank chip needs). New `FlashNodePage` (4th
      sidebar section): scans all ports except the gateway's own
      (touching it would reset the live connection), flags any MAC not in
      `known_devices` as unprovisioned, flashes on demand. **2026-09-12**:
      fully verified end-to-end on a real, previously-untouched board -
      scanned and correctly flagged unprovisioned (distinct MAC from every
      known device), flashed via the app's own button (0%->100% progress),
      confirmed booting into `PAIRED (mesh-only)` and appearing in the
      gateway's `known_devices` immediately after.
- [x] **Fixed confusing test-node identity strings** - having two test
      nodes running side by side (the original one plus the freshly
      onboarded one) surfaced a real naming problem: `node-firmware`'s
      hardcoded `projectName` was literally `"GatewayTest"`, which reads
      in the desktop app's device table as if these nodes *are* gateways
      - they aren't, only the AtomS3U is. Its `friendlyName` was also a
      hardcoded `"AtomS3-Lite-01"` shared by every instance, so two real
      devices showed up as identical, indistinguishable rows. Changed
      `projectName` to `"RTSNOW-Test"` and `friendlyName` to
      `"TestNode-XXXX"` (low 16 bits of `deviceID`, so it's unique per
      board) in `make_identity()`. **2026-09-12**: reflashed the
      currently-connected test node and confirmed the corrected identity
      live in the gateway's `known_devices` - the other test node wasn't
      plugged in at the time, so it's still running the old identity
      strings until it's reflashed too.
- [x] **Dashboard now shows the gateway's MAC** under Device ID - no
      firmware change needed: this hardware's native USB CDC reports its
      real MAC as the OS-level USB serial number (exactly why the
      deviceID it already sends matches - deviceID is the low 4 bytes of
      that same MAC). Added `serial_number` to `SerialPortInfo`
      (`src/gateway/ports.py`) and `DashboardPage.set_mac()`, populated
      immediately on connect (no round trip to the gateway needed, unlike
      the other Gateway fields). **2026-09-12**: verified against real
      hardware - shows `50:78:7D:CD:E7:00`, matching `pio device list`.
- [x] **Node renaming** - Node Detail's friendly name is now editable:
      "Rename" pushes it as a regular `setting` command with the reserved
      key `friendlyName` (documented in `PROTOCOL.md`'s "Generic
      settings"). `node-firmware` specifically recognizes this key -
      updates `s_friendly_name` and persists it via `Preferences`/NVS
      (`save_friendly_name`/`load_friendly_name` in `main.cpp`), so it
      survives a reboot. **2026-09-12**: verified twice on real hardware -
      once via raw JSON (renamed, confirmed in a fresh `list`), and once
      through the actual app UI end-to-end.

      Along the way, found and fixed a real gap: the gateway's `ack` for
      a `setting` command only confirms it was *sent*, not that the node
      *received* it - a node that had gone silent (see below) still got
      `ok:true` back. The desktop app now tracks the separate, unsolicited
      `setting_ack` event with its own 5s timeout instead of trusting the
      immediate ack (`MainWindow._pending_renames`,
      `NodeDetailPage.set_rename_status`) - shows "Sent - awaiting node
      confirmation...", then either "Confirmed by node." or "No
      confirmation received - node may be offline." Documented the
      ack-vs-confirmation distinction in both `PROTOCOL.md` and
      `SERIAL_PROTOCOL.md` so it isn't rediscovered the hard way again.

      Also hit, and finally got a clean root-cause read on, the recurring
      "node goes silent on USB and stops heartbeating" issue from earlier
      in this session: it's very likely the node's chip getting left in
      the ROM bootloader by a *failed* `pio run -t upload` attempt (which
      still resets the board into download mode as its first step, even
      though the failed attempt never got far enough to flash and exit
      that mode) - not a new bug, just the same known "No serial data
      received" flakiness with a more complete explanation now. Fixed the
      same way as before: a physical unplug/replug. Worth remembering:
      after any *failed* upload attempt (not just successful ones), treat
      the board as possibly stuck until its next confirmed response.
- [x] **Fixed: Rename button stuck disabled.** User hit this immediately
      after the rename feature shipped - `show_device()` re-enables
      `send_setting_btn` when a device is selected but had been missed
      for `rename_btn`, which only gets touched by the connection-state
      callback (`set_enabled`, called at connect time when no device is
      selected yet, i.e. `self._mac is None` - permanently disabling it).
      One-line fix: `show_device()` now also enables `rename_btn`.
      **2026-09-12**: verified headless by replicating the exact
      sequence (connect, then select a device).
- [x] **Fixed: PolymerPak's rename didn't actually apply.** User tried
      renaming all three known devices after the rename feature shipped;
      two failed for two different, unrelated reasons. `PolymerPak
      Tracker`'s own `../../Customer Projects/PolymerPak/src/rtsnow_node.cpp`
      never actually implemented the friendlyName convention - its
      `RTSNOW_SET_SETTING` handler just ack'd every setting unconditionally
      without applying or persisting anything (a placeholder from when it
      was first added, per its own comment). Fixed to match
      `node-firmware`'s approach exactly (`s_friendlyName`,
      `loadFriendlyName()`/`saveFriendlyName()` via `Preferences`).
      **Pushed via OTA, not USB** - the OTA support added earlier this
      session for exactly this reason meant no physical access was needed
      for a firmware fix. **2026-09-12**: verified live - renamed to
      "PolymerPak Field 3" over the real gateway, confirmed via a fresh
      `list` once its next heartbeat landed (took a full ~10s heartbeat
      cycle to show up - the immediate next `list` right after the ack
      still showed the old name, since heartbeats aren't instant).
- [x] **Desktop app launcher** - `desktop-app/RTS ESP-NOW Gateway.app`, a
      minimal macOS `.app` bundle (`Contents/Info.plist` +
      `Contents/MacOS/launch`, a shell script resolving `desktop-app/`
      relative to its own location and running `.venv/bin/python3
      main.py`) so the app can be double-clicked or Dock-launched instead
      of run from a terminal. Tied to staying inside `desktop-app/` (runs
      the venv/source alongside it, not bundled inside) - the whole repo
      can move as a unit, but the `.app` shouldn't be copied out on its
      own. **2026-09-12**: verified via `open` (same mechanism Finder
      uses for a double-click) - launches cleanly.
- [x] **Shared `RTSNow` PlatformIO library** (`libraries/RTSNow/`) -
      `rtsnow_protocol.h` and a generalized `rtsnow_node.h`/`.cpp` (the
      "simple node" helper, parameterized via `RTSNowNodeConfig` instead
      of `PolymerPak`'s hardcoded identity strings) consolidated out of
      three hand-maintained copies (`gateway-firmware`, `node-firmware`,
      `PolymerPak`) into one canonical source, pulled in by each
      consuming project via PlatformIO's `lib_extra_dirs` instead of
      copy-paste. Prompted by starting a third port (the SPI-IM ESP
      M5 Tough -> AtomS3 Lite work below) that would otherwise have been
      a fourth copy. `PolymerPak` refactored to call
      `rtsnowNodeBegin(RTSNowNodeConfig{...})`; its local
      `rtsnow_node.h`/`.cpp`/`rtsnow_protocol.h` deleted.
      `gateway-firmware`/`node-firmware` similarly deleted their local
      `rtsnow_protocol.h` in favor of the shared one (their own
      provisioning-handshake node logic stays in-tree for now - see
      `PROTOCOL.md`'s "Adding RTS-NOW to a new project" section for why).
      All three projects build-verified clean (`pio run`) after the
      switch; nothing reflashed yet since no behavior changed, only
      where the source lives. **2026-09-12**.

## Next up (in order)

- [ ] Reflash the other two test nodes (MAC `44:1B:F6:F6:9A:F8` and
      `44:1B:F6:F6:6F:78`) with the current firmware (identity fix +
      friendlyName persistence) - both are alive and pairing fine over
      ESP-NOW but their USB connections are currently wedged for flashing
      (the same recurring "No serial data received" quirk); need a
      physical unplug/replug first.

## Not yet designed / open questions

- **OTA over ESP-NOW itself**, for mesh-only nodes with no Wi-Fi at all -
  raised, discussed (feasible: ESP-NOW's 250-byte payload cap means
  chunking + per-chunk retry + a final integrity check, transferred
  meaningfully slower than Wi-Fi OTA, and unencrypted-in-transit like the
  existing Wi-Fi credential provisioning unless a signing step is added),
  and explicitly deferred - Wi-Fi-only OTA was built instead this pass.
  Revisit if a mesh-only deployment actually needs field updates.
  Related idea raised and also deferred: a delta/binary-diff OTA (only
  transmit changed bytes, e.g. via `bsdiff`) to make an ESP-NOW OTA (or
  any slow-link OTA) cheaper when only the RTSNOW_ protocol code changed
  and a node's own application code didn't - real technique, adds real
  build-pipeline complexity, not needed while updates are Wi-Fi/USB-only.
- A "lost heartbeat" / connectivity-health LED indicator was raised, then
  paused before implementation - the open design question: some nodes may
  intentionally never re-pair with a gateway once done, so a blanket
  "blink red/green if no heartbeat" would misrepresent a standalone node
  as faulty. Needs a per-node "do I even care about gateway connectivity"
  flag alongside `kNeedsWifi`/mesh-only-vs-Wi-Fi, not just a timeout.
- `RTSNOW_GET_SETTING`/`RTSNOW_SETTING_VALUE` (read-back of a node's
  current settings - only the push direction is designed so far).
- Real encryption for `RTSNOW_PROVISION_CREDENTIALS` (see `PROTOCOL.md`'s
  security note - Wi-Fi passwords currently cross the air unencrypted
  during the brief provisioning window).
- DryerHealth integration itself (`src/hardware/` gateway client, wiring
  the "ESP-NOW Gateway" UI page, and deciding whether it imports/vendors
  `desktop-app/src/gateway/` or reimplements a client against
  `SERIAL_PROTOCOL.md` independently) - deliberately deferred until this
  gateway's firmware/protocol is solid and the provisioning handshake is
  verified end-to-end.
