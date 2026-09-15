# RTS ESP-NOW Gateway Protocol

Wire protocol between the **RTS ESP-NOW Gateway** (an M5Stack AtomS3U
plugged into a laptop over USB) and any ESP32/ESP8266 "node" belonging to
**any** RTS project (CYD-4.3-FN-Tester's FN pod, DryerHealth's dryer nodes,
and future projects not yet started). The gateway is project-agnostic: it
doesn't hardcode any project's device taxonomy, only a generic identity +
provisioning + settings model every project's nodes speak the same way.

This document covers the **ESP-NOW side** (gateway <-> node, over the air).
See `README.md` for the gateway's human-typeable USB serial text commands,
and `SERIAL_PROTOCOL.md` for the JSON-lines protocol `desktop-app/` speaks
to the gateway over that same USB serial connection - neither is part of
this ESP-NOW wire protocol.

## Design goals

- **One gateway, many projects.** A node identifies which project it
  belongs to and what kind of device it is as free text, not a closed
  enum the gateway firmware has to know about in advance. Adding a new
  project or device type never requires touching this repo.
- **Real Wi-Fi provisioning.** Unlike CYD-4.3-FN-Tester's earlier
  ShackMate protocol (which only ever handed over an ESP-NOW channel
  number, deliberately never touching Wi-Fi), this gateway's whole point
  is to hand a fresh node real Wi-Fi SSID/password credentials over
  ESP-NOW, so it can join the plant/lab network without ever being typed
  in by hand on the node itself.
- **Extensible settings, not a fixed schema.** "Many other settable
  settings a particular project needs" (the actual ask this protocol was
  built for) means a generic typed key/value push, not per-project fields
  baked into this protocol.
- **Reuse proven lessons, not proven code.** CYD-4.3-FN-Tester's
  ShackMate protocol (`../CYD-4.3-FN-Tester/ESPNOW_PROTOCOL.md`) spent a
  long debugging session learning real hardware lessons - broadcast
  delivery is reliable, synchronous unicast from an ESP-NOW receive
  callback often isn't; a nonce is needed so a slow human decision doesn't
  race a stale reply; a dedicated "channel committed" flag is needed so a
  timeout-driven sweep loop can't fire concurrently with a handshake still
  in flight. Those lessons are carried over here. The wire format and
  naming are not: this protocol is a fresh, project-neutral design (no
  `SM_`/"ShackMate" naming), since this gateway is meant to outlive any
  one product.

## Transport

ESP-NOW, same as ShackMate: connectionless, point-to-multipoint, rides the
2.4GHz Wi-Fi radio, needs only a matching **channel** - no AP association,
no IP address, for the *ESP-NOW hop itself*. Every packet is an
`RTSNOW_Header` immediately followed by `payloadLength` bytes of a
message-specific payload. **250-byte payload limit** (ESP-NOW's hard cap).

```c
#pragma pack(push, 1)
struct RTSNOW_Header
{
    uint8_t  version;        // protocol version, currently 1
    uint8_t  messageType;    // RTSNOW_MessageType
    uint32_t sourceID;       // sender's persistent device ID
    uint32_t destinationID;  // recipient's device ID, or 0xFFFFFFFF for broadcast
    uint16_t sequence;       // sender-assigned, increments per message
    uint16_t payloadLength;  // bytes of payload following this header
};
#pragma pack(pop)
```

## Device identity

```c
#pragma pack(push, 1)
struct RTSNOW_DeviceIdentity
{
    uint32_t deviceID;           // persistent, derived from the node's Wi-Fi station MAC (last 4 bytes)
    char     projectName[16];    // e.g. "FN-Tester", "DryerHealth" - which RTS project this node belongs to
    char     deviceTypeName[16]; // e.g. "FN-2Wire-Pod", "Dryer-Node" - free text, project-defined, not a shared enum
    char     friendlyName[24];   // e.g. "FN-POD-01", "Dryer 3 Node"
    uint8_t  firmwareVersionMajor;
    uint8_t  firmwareVersionMinor;
    uint8_t  firmwareVersionPatch;
    uint32_t ipv4Address;        // this node's Wi-Fi station IP (host byte order), or 0 if not currently Wi-Fi-joined
};
#pragma pack(pop)
```

`projectName`/`deviceTypeName` being free text (not an enum, unlike
ShackMate's `SM_DeviceType`) is the load-bearing design choice that makes
"one gateway, many projects" possible - a brand-new project's nodes show
up in the gateway's device table with no firmware or protocol change here,
as long as they speak this identity struct and the message types below.

## Mesh-only pairing (no Wi-Fi)

Some deployments have no Wi-Fi network to hand out at all - those nodes
depend on the ESP-NOW mesh alone. For them, "pairing" is just broadcasting
`RTSNOW_ANNOUNCE` (`ipv4Address = 0`) at boot: the gateway already
auto-adds *any* announcing device to its known-devices table with **no
operator approval step** (see `note_known_device` in
`gateway-firmware/src/main.cpp`) - the same auto-accept behavior a
Wi-Fi-joined node's post-provisioning `RTSNOW_ANNOUNCE` already relies on.
No new message type exists for this and none is needed: the Wi-Fi
provisioning handshake (`RTSNOW_PROVISION_REQUEST`/`_HOLD`/`_CREDENTIALS`/
`_REJECTED`) exists specifically to hand over a real secret (a Wi-Fi
password), which is why *that* flow is operator-gated - a mesh-only node
never carries or receives any secret, so there's nothing to gate.

Whether a given node needs Wi-Fi or is mesh-only is a **fixed, per-build**
decision (see `node-firmware/src/main.cpp`'s `kNeedsWifi`), not something
negotiated at runtime - a deployment either has a Wi-Fi network available
for that node type or it doesn't.

## Adding RTS-NOW to a new project

`libraries/RTSNow/` (this repo) is the canonical, shared home for
`rtsnow_protocol.h` (the wire format above) and `rtsnow_node.h`/`.cpp` (a
reusable "simple node" helper: announce/heartbeat, `friendlyName`
rename+persistence, generic-setting acking). It used to be a hand-copied
file duplicated into every consuming project (`gateway-firmware`,
`node-firmware`, `PolymerPak`) - drifted comments (never struct layouts,
thankfully) were the first sign that was going to be a problem, so it was
consolidated here before a real divergence happened.

A new project pulls it in via PlatformIO's `lib_extra_dirs`, pointing at
this folder by relative path (e.g. `../../RTS-ESPNOW-Gateway/libraries`
for a sibling-of-a-sibling project like `PolymerPak`, `../libraries` for
something living inside this repo like `node-firmware`) - no copying, no
package registry, no version pin; the next build just picks up whatever
is currently in this folder. See `PolymerPak/platformio.ini` for a
working example.

Two ways to consume it, depending on what the node needs:

- **Self-managed or no Wi-Fi, no provisioning needed** (the common
  case - matches `PolymerPak`, and any future "headless" node without a
  screen to run `node-firmware`'s pairing UI): call `rtsnowNodeBegin()`
  with an `RTSNowNodeConfig` (project/device-type/default-friendly names,
  firmware version) once Wi-Fi is up (or immediately, if the node is
  mesh-only - `rtsnow_node.cpp` doesn't require `WL_CONNECTED`, it just
  reports `ipv4Address = 0` until it is), then `rtsnowNodeLoop()` every
  `loop()` iteration. See `rtsnow_node.h`'s own header comment for the
  full contract.
- **Needs the gateway to hand it Wi-Fi credentials, or the full
  channel-sweep/hold/provision handshake** (matches `node-firmware`
  itself): there's no shared helper for this yet - `node-firmware`'s
  state machine (`Sweeping`/`Held`/`Joining`/`Joined`/`Paired` in its
  `main.cpp`) is still implemented in-tree, not in the shared library.
  Copy its pattern rather than `rtsnow_node.cpp`'s if a new project needs
  this mode; it'd be a reasonable follow-up to extract this into the
  shared library too once a second project actually needs it.

Either way, only `rtsnow_protocol.h`'s structs need to stay
byte-identical across every project that speaks this protocol - that's
the one piece that must never fork per-project.

## Message types

```c
enum RTSNOW_MessageType : uint8_t
{
    // Discovery
    RTSNOW_DISCOVER = 0x01,          // broadcast: "who's out there?"
    RTSNOW_ANNOUNCE = 0x02,          // broadcast/reply: RTSNOW_DeviceIdentity
    RTSNOW_HEARTBEAT = 0x03,         // periodic broadcast: RTSNOW_Heartbeat

    // Wi-Fi provisioning
    RTSNOW_PROVISION_REQUEST = 0x10, // broadcast, while channel-sweeping: RTSNOW_ProvisionRequest - "I have no Wi-Fi credentials yet, provision me"
    RTSNOW_PROVISION_HOLD = 0x11,    // unicast, gateway -> node: RTSNOW_ProvisionNonce - "I see you - stop sweeping and wait here"
    RTSNOW_PROVISION_CREDENTIALS = 0x12, // unicast, gateway -> node: RTSNOW_WifiCredentials - "here's your Wi-Fi SSID/password"
    RTSNOW_PROVISION_REJECTED = 0x13,    // unicast, gateway -> node: RTSNOW_ProvisionNonce - "declined - resume sweeping"

    // Settings
    RTSNOW_SET_SETTING = 0x20,       // unicast, gateway -> node: RTSNOW_SettingPayload
    RTSNOW_SETTING_ACK = 0x21,       // unicast, node -> gateway: RTSNOW_SettingAck

    // Generic
    RTSNOW_ACK = 0x30,               // RTSNOW_AckPayload - acknowledges a message by sequence #
};
```

```c
#pragma pack(push, 1)
struct RTSNOW_Heartbeat
{
    RTSNOW_DeviceIdentity identity;
    uint32_t uptimeSeconds;
};
#pragma pack(pop)
```

## Wi-Fi provisioning

A factory-fresh (or factory-reset) node has no Wi-Fi credentials and no
ESP-NOW channel to reach the gateway on. Mirrors ShackMate's channel
handshake shape, but hands over real credentials instead of just a channel
number:

1. **Node sweeps.** It sweeps `RTSNOW_PROVISIONING_CHANNEL_MIN..MAX`
   (channels 1-11), dwelling briefly on each and broadcasting
   `RTSNOW_PROVISION_REQUEST` (its identity + a nonce fixed for this
   attempt), until it happens to land on whatever channel the gateway is
   sitting on.

   ```c
   #pragma pack(push, 1)
   struct RTSNOW_ProvisionRequest
   {
       RTSNOW_DeviceIdentity identity;
       uint32_t nonce;
   };
   #pragma pack(pop)
   ```

2. **Gateway holds it.** Unlike the CYD (which only ever passively
   listened on its own already-joined Wi-Fi channel), this gateway has no
   AP of its own to sit on - it just picks and stays on one ESP-NOW
   channel (default channel 1, changeable via the `channel <n>` serial
   command - see `README.md`). On hearing a request, it
   immediately replies `RTSNOW_PROVISION_HOLD` (echoing the nonce) so the
   node stops sweeping and waits, then surfaces the pending request to
   whatever's listening on the USB serial control interface (a human
   typing commands, or eventually a desktop app) as a `pending` line - see
   `README.md`'s `list`/`pending` commands.

   ```c
   #pragma pack(push, 1)
   struct RTSNOW_ProvisionNonce { uint32_t nonce; };
   #pragma pack(pop)
   ```

3. **Operator decides.** Via the serial interface: `provision <mac> <ssid>
   <password>` to accept, `reject <mac>` to decline.

4. **On accept - gateway sends `RTSNOW_PROVISION_CREDENTIALS`:**

   ```c
   #pragma pack(push, 1)
   struct RTSNOW_WifiCredentials
   {
       uint32_t nonce;      // must match the request's nonce - the node ignores this message otherwise
       char     ssid[32];   // matches IEEE 802.11's own 32-byte SSID limit
       char     password[64]; // matches WPA2-PSK's own 64-character max
   };
   #pragma pack(pop)
   ```

   **On decline - gateway sends `RTSNOW_PROVISION_REJECTED`** (echoed
   nonce), so the node resumes sweeping immediately instead of waiting out
   its own hold timeout.

5. **Node joins Wi-Fi** with the received SSID/password, then broadcasts
   `RTSNOW_ANNOUNCE` on whatever channel it lands on once associated
   (`WiFi.channel()` after a successful connection). **This broadcast, not
   an ACK to the credentials message, is what the gateway treats as
   confirmation** - see "Why broadcast, not ACK" below.

If nothing happens within the node's own hold timeout, or the gateway's
own pending-request timeout, each side quietly resumes independently
(exact values are a firmware/implementation detail, not part of the wire
protocol - see `gateway-firmware/src/main.cpp`).

### Why broadcast, not ACK

CYD-4.3-FN-Tester's ShackMate protocol spent a real debugging session
(documented in that project's `PROJECT_MEMORY.md`, 2026-08-27 entries)
discovering that a synchronous unicast ACK sent from inside an ESP-NOW
receive callback - reached directly from the packet that triggered it -
consistently never got a confirmed delivery on real ESP32-S3 hardware, no
matter what timing/ordering bugs were fixed around it, while every
broadcast ever sent got a confirmed `SUCCESS`. This protocol designs
around that finding from the start rather than rediscovering it: the
node's own post-join `RTSNOW_ANNOUNCE` broadcast is the gateway's *primary*
signal that provisioning actually completed, not a unicast ACK to
`RTSNOW_PROVISION_CREDENTIALS`. (This protocol has no
`RTSNOW_PROVISION_ACK` message at all, unlike ShackMate's
`SM_PROVISION_ACK` - that message existed there only as an unreliable
secondary signal once the broadcast-based fix landed, so it isn't worth
carrying forward here.)

### Security note - credentials are sent in the clear

`RTSNOW_PROVISION_CREDENTIALS` is **not encrypted**. This is the same
tradeoff ShackMate made and documented (`ESPNOW_PROTOCOL.md`'s
"Encryption - reverted"): ESP-NOW's per-peer AES encryption requires both
sides to have already registered each other as an encrypted peer before
the first encrypted packet arrives, which isn't possible for a gateway and
a node that have never talked before. ShackMate could shrug this off once
its handshake stopped carrying real secrets (just a channel number). **This
protocol still carries a real Wi-Fi password**, so the exposure is real,
not hypothetical - anyone with an ESP-NOW-capable radio within range during
the brief provisioning window could capture it. Mitigations actually in
place: the window is brief (one hold-timeout's worth, then it closes) and
requires an explicit operator action (`provision <mac> <ssid> <password>`)
during that window, not something that happens automatically. **Not yet
designed**: a real pre-shared-key or out-of-band key exchange so this can
be encrypted properly. Treat this like any other unencrypted-provisioning
IoT setup (e.g. WPS) - fine for a physically-controlled lab/plant floor,
not something to expose to an untrusted RF environment.

## Generic settings

"Many other settable settings a particular project needs" - a typed
key/value push, so a project can define its own settable fields (a dryer
node's reporting interval, a pod's friendly name, anything) without this
protocol or the gateway firmware needing to know what they mean.

```c
enum RTSNOW_SettingValueType : uint8_t
{
    RTSNOW_SETTING_STRING = 0,
    RTSNOW_SETTING_INT32 = 1,
    RTSNOW_SETTING_FLOAT32 = 2,
    RTSNOW_SETTING_BOOL = 3,
};

#pragma pack(push, 1)
struct RTSNOW_SettingPayload
{
    char    key[24];         // project-defined, e.g. "reportIntervalSec", "friendlyName"
    uint8_t valueType;       // RTSNOW_SettingValueType
    char    stringValue[32]; // used when valueType == RTSNOW_SETTING_STRING
    union
    {
        int32_t intValue;
        float   floatValue;
        bool    boolValue;
    };
};
#pragma pack(pop)

#pragma pack(push, 1)
struct RTSNOW_SettingAck
{
    char    key[24];  // echoed back
    uint8_t accepted; // 0/1 - a node can reject an unrecognized key or an out-of-range value
};
#pragma pack(pop)
```

### Reserved key: `"friendlyName"`

Not a new message type - just a convention, first exercised by
`node-firmware`: a node that wants to support being renamed from the
gateway/desktop app honors a setting with `key = "friendlyName"` and
`valueType = RTSNOW_SETTING_STRING` by updating the `friendlyName` it
reports in its own `RTSNOW_DeviceIdentity` from then on, and persisting
the new value (NVS or equivalent) so it survives a reboot. Nothing in the
protocol *requires* a node to honor this key - same as any other
setting, a node can reply `RTSNOW_SETTING_ACK{accepted=0}` if it doesn't
support renaming.

**A real gotcha discovered while building this**: the gateway's JSON
`ack` for a `setting` command (`SERIAL_PROTOCOL.md`) only confirms the
gateway accepted the command and forwarded it over ESP-NOW - it does
*not* wait for the node's actual `RTSNOW_SETTING_ACK` before answering.
A node that's gone silent (crashed, out of range, powered off) still
gets an `ok:true` JSON ack back, because the ESP-NOW send call itself
succeeded even though nothing ever received it. The real confirmation
that a node actually got and applied a setting is the separate,
unsolicited `setting_ack` event - a client that cares whether a setting
actually landed (like a rename) needs to track that event (with its own
timeout, since it may never arrive) rather than trust the immediate ack
alone. `desktop-app`'s rename UI does this.

`stringValue` and the numeric union are both always present in the fixed
struct (rather than only sending the bytes actually needed) - matches
every other struct in this protocol and ShackMate before it:
fixed-size, `memcpy`-able payloads are simpler and safer than manual
variable-length packing given the tiny (250-byte) size budget involved
either way.

**Not yet designed**: a matching `RTSNOW_GET_SETTING`/`RTSNOW_SETTING_VALUE`
pair for reading a node's current settings back (this pass only covers the
gateway pushing settings *to* a node, per the explicit scope this protocol
was built for) - reusing `RTSNOW_SettingPayload`'s shape for the reply
would be the natural extension when a project actually needs it.

## Remote control

Two generic, project-neutral actions any node can support - added for
DryerHealth/SPI-IM's need to remotely reboot a dryer-monitor node and pull
its current Modbus register table on demand, but neither message says
anything DryerHealth-specific, so they live here rather than as a
one-off SPI-IM extension.

```c
RTSNOW_REBOOT = 0x31,             // unicast, gateway -> node: no payload
RTSNOW_REQUEST_REGISTERS = 0x32,  // unicast, gateway -> node: no payload
RTSNOW_REGISTER_VALUES = 0x33,    // unicast, node -> gateway: RTSNOW_RegisterBlock
```

`RTSNOW_REBOOT` has no ack - same reasoning as settings (see "Why broadcast,
not ACK" above): the gateway's `ack` for the `reboot` command only confirms
it sent the ESP-NOW packet, not that the node received it or actually
restarted. A rebooted node's own post-boot `RTSNOW_ANNOUNCE` is the real
signal it came back, same pattern as Wi-Fi provisioning's confirmation.

`RTSNOW_REQUEST_REGISTERS` asks a node to reply once with its current
register block. A node with nothing to report (most nodes - this is opt-in
via `rtsnow_node.h`'s `RTSNowNodeConfig::registerBlockProvider`) just never
replies, same as an unrecognized `RTSNOW_SET_SETTING` key.

```c
struct RTSNOW_RegisterBlock
{
    uint16_t startRegister;  // first register number this block covers, e.g. 40001
    uint16_t registerCount;  // how many of `values` are valid, <= 64
    uint16_t values[64];
};
```

Deliberately shaped like a generic Modbus holding-register read (start +
count + values), not DryerHealth's specific 40001-40041 layout - what each
register number *means* is left to the owning project's own documentation
(e.g. SPI-IM's `ModbusRegisterMap.h`), the same "generic key/value, not a
fixed schema" philosophy as `RTSNOW_SettingPayload`. 64 registers (128
bytes) comfortably fits any project seen so far in one packet, well under
the 250-byte ESP-NOW payload cap even with the header included.

## Generic ack

```c
#pragma pack(push, 1)
struct RTSNOW_AckPayload { uint16_t acknowledgedSequence; };
#pragma pack(pop)
```

## Provisioning channel range

```c
constexpr uint8_t RTSNOW_PROVISIONING_CHANNEL_MIN = 1;
constexpr uint8_t RTSNOW_PROVISIONING_CHANNEL_MAX = 11; // 2.4GHz channels legal in every regulatory domain
```

## Status

**Implemented and verified end-to-end on real hardware**
(`gateway-firmware/` + `node-firmware/`, a minimal reference/test node -
not a real project's node, see its own header comment): discovery
(`RTSNOW_DISCOVER`/`RTSNOW_ANNOUNCE`/`RTSNOW_HEARTBEAT`), the full Wi-Fi
provisioning handshake (`RTSNOW_PROVISION_REQUEST`/`_HOLD`/`_CREDENTIALS`/
`_REJECTED`, sweep -> hold -> credentials -> join -> announce, including
the gateway's `provision_confirmed` detection), and generic settings push
(`RTSNOW_SET_SETTING`/`RTSNOW_SETTING_ACK`) - confirmed on a real M5Stack
AtomS3U gateway and AtomS3 Lite test node, driven through `desktop-app/`.
Mesh-only pairing (no Wi-Fi - see above) also confirmed: the test node
boots straight into a paired state via `RTSNOW_ANNOUNCE` alone, the
gateway lists it immediately with no operator step, and a 5-second
button hold (or the `forget` serial command) erases its state and
re-triggers pairing on reboot - confirmed with a live physical button
press, not just a simulated one.
**Not yet built**: a real project's own production node firmware (this
repo's `node-firmware/` is a minimal test article, not a template meant
for reuse as-is) - CYD-4.3-FN-Tester's FN pod and DryerHealth's planned
dryer nodes still don't speak RTSNOW_.

## Not yet designed

- `RTSNOW_GET_SETTING`/`RTSNOW_SETTING_VALUE` (see "Generic settings"
  above).
- Real encryption for `RTSNOW_PROVISION_CREDENTIALS` (see "Security note"
  above).
- Any equivalent of ShackMate's capability announcement
  (`SM_CAPABILITIES`) or value subscription (`SM_SUBSCRIBE`/`SM_VALUE`) -
  out of scope for this pass; a project needing live telemetry values
  (e.g. DryerHealth's dryer readings) will need its own message types on
  top of this base, or its own separate application-level protocol
  carried over ESP-NOW once a node is provisioned - this protocol only
  covers *identity, provisioning, and settings*, not general telemetry.
- Firmware-update-over-ESP-NOW.

(A structured, newline-delimited-JSON USB serial protocol for a desktop app
was on this list - it's now built; see `SERIAL_PROTOCOL.md` and `desktop-app/`.)
