# Gateway USB Serial Protocol (JSON-lines)

The laptop &lt;-&gt; gateway side (not to be confused with `PROTOCOL.md`'s
gateway &lt;-&gt; node ESP-NOW wire protocol). This is what `desktop-app/`
speaks to `gateway-firmware/` over USB serial at 115200 baud.

## Framing

One JSON object per line (newline-delimited), both directions, UTF-8, no
embedded newlines. Coexists on the same serial stream as the gateway's
human-typeable text commands (`list`, `provision <mac> <ssid> <password>`,
etc. - see the root [`README.md`](README.md)): a line is JSON if its first
non-whitespace character is `{`, otherwise it's routed to the legacy text
command parser. A client should simply ignore any line that doesn't parse
as JSON - the gateway's plain-text boot banner/help output is unaffected
and still appears on the same stream for a human at a terminal.

Unlike the text interface (naive whitespace-tokenized, so SSIDs/keys/values
can't contain spaces), JSON string fields have no such restriction.

## Identifying a gateway (vs. a node)

The gateway and any RTSNOW_ node (e.g. `node-firmware/`) are indistinguishable
at the OS/USB level - same VID:PID, same generic "USB JTAG/serial debug unit"
description on this hardware (see `TASKS.md` for why a distinct USB product
string turned out not to be possible on the `m5stack-atoms3` board). A user
picking the wrong port in a serial terminal or `desktop-app/`'s port dropdown
is a real, easy mistake - it happened during development.

The reliable signal is protocol-level, not OS-level: **only a gateway ever
emits JSON** on this serial stream. A node's serial interface (e.g.
`node-firmware/`'s `status`/`forget`/`help`) has no JSON parser or emitter at
all - sent a JSON-lines command, it can only reply with its own plain-text
"Unknown command" error, never anything starting with `{`. `desktop-app/`
uses this: right after connecting, it sends `{"cmd":"list"}` and starts a
short timer; if any well-formed JSON event arrives before the timer expires,
the connection is confirmed as a real gateway; if not, it disconnects
automatically with a clear error rather than silently polling a device that
will never answer meaningfully.

## Commands (app -> gateway)

Every command is `{"cmd": "<name>", ...}`. An optional `"id"` (any JSON
value - the app uses an incrementing integer) is echoed back verbatim on
the corresponding response, so a client can correlate a reply to the
request that caused it without relying on ordering.

| Command | Fields | Replies with |
|---|---|---|
| `hello` | - | `hello` (on demand - see "Events" below; also used as the identity-verification probe) |
| `list` | - | `known_devices` |
| `pending` | - | `pending_requests` |
| `provision` | `mac`, `ssid`, `password` | `ack` |
| `reject` | `mac` | `ack` |
| `setting` | `mac`, `key`, `valueType` (`string`\|`int`\|`float`\|`bool`), `value` | `ack` |
| `reboot` | `mac` | `ack` (real confirmation is the node's next `device_announced`/heartbeat after it comes back - see `PROTOCOL.md`'s "Remote control" section) |
| `poll_registers` | `mac` | `ack` (+ unsolicited `register_values` if the node supports it and is reachable) |
| `channel` | `value` (1-11) | `ack` (+ unsolicited `channel_changed`) |
| `discover` | - | `ack` |

`setting`'s `valueType` is explicit, not sniffed from the value's JSON type
(unlike the text interface's `cmd_setting`, which guesses from the typed-in
string) - the client always knows what it means to send, so there's no
ambiguity to resolve. `value` is a native JSON string/number/bool matching
`valueType`. `key: "friendlyName"` (string) is a reserved convention for
renaming a node - see `PROTOCOL.md`'s "Generic settings" section.

```json
{"cmd": "provision", "mac": "AA:BB:CC:DD:EE:FF", "ssid": "Lab-WiFi", "password": "hunter2", "id": 7}
{"cmd": "setting", "mac": "AA:BB:CC:DD:EE:FF", "key": "reportIntervalSec", "valueType": "int", "value": 30, "id": 8}
```

## Events (gateway -> app)

Every event is `{"event": "<name>", ...}`.

**Replies to a query command** - include the request's `id` if it had one:

- `known_devices` - `{"event":"known_devices","devices":[{...}],"id":1}`
- `pending_requests` - `{"event":"pending_requests","requests":[{...}],"id":2}`

A device object: `deviceID`, `projectName`, `deviceTypeName`,
`friendlyName`, `mac`, `ip` (omitted if not Wi-Fi-joined), `ageMs`
(milliseconds since last seen). A pending-request object is the same
identity fields plus `ageMs` (since the request arrived), no `ip`.

**Replies to an action command:**

- `ack` - `{"event":"ack","cmd":"provision","ok":true,"id":7}`, or on
  failure `{"event":"ack","cmd":"provision","ok":false,"error":"...","id":7}`.
  **Important**: for `setting`, this only confirms the gateway accepted
  the command and sent it over ESP-NOW - not that the node received or
  applied it (the node might be offline, out of range, etc.). The real
  confirmation is the separate `setting_ack` event below; a client that
  needs to know whether a setting actually took effect (e.g. a rename)
  should track that event with its own timeout, not just this ack.

**Unsolicited** (no `id` - not a reply to anything in particular) **or on-demand** (echoes `id` if the `hello` command had one):

- `hello` - once unprompted at boot, and again any time a client sends the `hello` command (added so a client connecting after boot - the common case - isn't stuck with no gateway identity): `{"event":"hello","protocolVersion":1,"deviceID":...,"channel":1}`
- `device_announced` - a device seen for the first time
- `provision_request` - a new pending provisioning request arrived
- `provision_expired` - a pending request timed out unanswered
- `provision_confirmed` - a node announced on the network after receiving credentials
- `provision_confirm_timeout` - no such announcement within the confirm window
- `setting_ack` - a node accepted/rejected a pushed setting: `{"event":"setting_ack","mac":"...","key":"...","accepted":true}`
- `register_values` - a node replied to `poll_registers`: `{"event":"register_values","mac":"...","startRegister":40001,"values":[...]}` - what each register number means is entirely project-defined (see that project's own register map), this gateway doesn't interpret them
- `channel_changed` - the gateway's own channel changed (from either interface): `{"event":"channel_changed","channel":5}`

## Status

**Implemented and verified end-to-end on real hardware**
(`gateway-firmware/src/main.cpp` + `node-firmware/`, a minimal test node):
`list`/`pending` snapshots, `provision`/`reject`/`setting`/`channel`/
`discover` commands with `ack` responses and `id` echoing, and every
unsolicited event including `provision_confirmed` (a real node completing
the full sweep -> hold -> credentials -> join -> announce handshake) and
`setting_ack` (a real node accepting a pushed setting). `provision_expired`/
`provision_confirm_timeout` are implemented but not yet deliberately
exercised (they're timeout paths - straightforward to trigger by leaving
a request/awaiting-confirm slot untouched past its timeout, just not done
as part of this pass).

One real bug found and fixed during this testing: the gateway's
ESP-NOW receive callback runs in the Wi-Fi driver's own task context, not
`loop()`'s - calling `Serial.print*` directly from there (for
`device_announced`, `provision_request`, `provision_confirmed`, and
`setting_ack`) intermittently corrupted output on real native-USB-CDC
hardware (bytes silently dropped mid-line). `Serial.flush()` did not fix
it (confirming a scheduling/context problem, not a buffering one) - the
actual fix was a small mutex-protected queue (`queue_output`/
`drain_output_queue` in `main.cpp`) so the receive callback only ever
enqueues a pre-rendered line, and only `loop()` ever writes to `Serial`.

**Client**: `desktop-app/` (PySide6) - see its own structure under
`desktop-app/src/gateway/` (serial client, port discovery, data model) and
`desktop-app/src/ui/` (main window, dialogs). Verified driving the real
gateway+node pair through the actual UI code (not just the underlying
client library): connect, live device/pending tables, the Provision
dialog accepting real Wi-Fi credentials and the provisioned node showing
up in Known Devices with its real IP, and a settings push accepted by the
node.
