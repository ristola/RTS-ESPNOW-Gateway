#pragma once

#include <cstdint>

// RTS ESP-NOW Gateway protocol - shared wire format between the gateway
// (an AtomS3U running RTS-ESPNOW-Gateway/gateway-firmware) and any node
// belonging to any RTS project. See ../../../PROTOCOL.md at the
// RTS-ESPNOW-Gateway repo root for the full design rationale - this file
// is the data model only.
//
// This is the canonical copy. Every consuming project (gateway-firmware,
// node-firmware, PolymerPak, and any future port) pulls this in via
// PlatformIO's lib_extra_dirs pointing at this library folder, instead of
// keeping its own copy - previously this drifted into three
// hand-maintained duplicates (comments only, thankfully, not the actual
// struct layouts) before being consolidated here.
//
// Deliberately project-neutral naming (RTSNOW_, not "ShackMate"/SM_ from
// CYD-4.3-FN-Tester's earlier, single-project protocol) - this gateway is
// meant to serve every RTS ESP project, not just one.

enum RTSNOW_MessageType : uint8_t
{
    // Discovery
    RTSNOW_DISCOVER = 0x01,  // broadcast: "who's out there?"
    RTSNOW_ANNOUNCE = 0x02,  // broadcast/reply: RTSNOW_DeviceIdentity
    RTSNOW_HEARTBEAT = 0x03, // periodic broadcast: RTSNOW_Heartbeat

    // Wi-Fi provisioning - see PROTOCOL.md's "Wi-Fi provisioning" section
    RTSNOW_PROVISION_REQUEST = 0x10,     // broadcast, while channel-sweeping: RTSNOW_ProvisionRequest
    RTSNOW_PROVISION_HOLD = 0x11,        // unicast, gateway -> node: RTSNOW_ProvisionNonce
    RTSNOW_PROVISION_CREDENTIALS = 0x12, // unicast, gateway -> node: RTSNOW_WifiCredentials
    RTSNOW_PROVISION_REJECTED = 0x13,    // unicast, gateway -> node: RTSNOW_ProvisionNonce

    // Settings - see PROTOCOL.md's "Generic settings" section
    RTSNOW_SET_SETTING = 0x20, // unicast, gateway -> node: RTSNOW_SettingPayload
    RTSNOW_SETTING_ACK = 0x21, // unicast, node -> gateway: RTSNOW_SettingAck

    // Generic
    RTSNOW_ACK = 0x30, // RTSNOW_AckPayload - acknowledges a message by sequence #

    // Remote control - see PROTOCOL.md's "Remote control" section
    RTSNOW_REBOOT = 0x31,             // unicast, gateway -> node: no payload - node restarts shortly after receiving
    RTSNOW_REQUEST_REGISTERS = 0x32,  // unicast, gateway -> node: no payload - "send me your current register block"
    RTSNOW_REGISTER_VALUES = 0x33,    // unicast, node -> gateway: RTSNOW_RegisterBlock
    // Write one register - see RTSNOW_WriteRegister's own comment for why
    // this is a real, physical-equipment-affecting command (e.g. starting/
    // stopping a dryer), not just a config value like RTSNOW_SET_SETTING.
    RTSNOW_WRITE_REGISTER = 0x34,     // unicast, gateway -> node: RTSNOW_WriteRegister
    RTSNOW_WRITE_REGISTER_ACK = 0x35, // unicast, node -> gateway: RTSNOW_WriteRegisterAck

    // Firmware update over ESP-NOW, for a node with no usable WiFi-OTA
    // path (no IP at all, or a link too unreliable for a sustained TCP
    // transfer) and no easy physical USB access either. Slow (each chunk
    // is a full request/ack round trip, driven and retried by the
    // desktop app - see RTS-ESPNOW-Gateway's own docs) but safe: the
    // receiving node writes through ESP32's own Update library
    // (identical to what ArduinoOTA itself uses under the hood) with an
    // expected MD5 set before the first byte is written, so a failed or
    // interrupted transfer is simply never marked bootable - the
    // device's current firmware keeps running untouched, same
    // dual-OTA-partition safety as any other ESP32 update path. Reuses
    // this same protocol's destinationID-based single-hop relay (see
    // that field's own comment) transparently - a chunk addressed to a
    // node that isn't directly reachable can be routed through another
    // known device exactly like RTSNOW_SET_SETTING already is.
    RTSNOW_OTA_START = 0x40,     // gateway -> node: RTSNOW_OtaStart
    RTSNOW_OTA_START_ACK = 0x41, // node -> gateway: RTSNOW_OtaAck
    RTSNOW_OTA_CHUNK = 0x42,     // gateway -> node: RTSNOW_OtaChunk
    RTSNOW_OTA_CHUNK_ACK = 0x43, // node -> gateway: RTSNOW_OtaChunkAck
    RTSNOW_OTA_END = 0x44,       // gateway -> node: no payload - "that was every chunk, verify and apply"
    RTSNOW_OTA_END_ACK = 0x45,   // node -> gateway: RTSNOW_OtaAck - node reboots itself right after sending this, if ok
    RTSNOW_OTA_ABORT = 0x46,     // either direction: no payload - cancel an in-progress transfer
};

// A node that doesn't yet have Wi-Fi credentials sweeps this channel
// range, dwelling briefly on each one and broadcasting
// RTSNOW_PROVISION_REQUEST, until the gateway - sitting on one fixed
// channel of its own choosing (see the gateway firmware's `channel`
// serial command) - hears it and replies.
constexpr uint8_t RTSNOW_PROVISIONING_CHANNEL_MIN = 1;
constexpr uint8_t RTSNOW_PROVISIONING_CHANNEL_MAX = 11;

#pragma pack(push, 1)
struct RTSNOW_Header
{
    uint8_t version;        // protocol version, currently 1
    uint8_t messageType;    // RTSNOW_MessageType

    uint32_t sourceID;      // sender's persistent device ID
    uint32_t destinationID; // recipient's device ID, or 0xFFFFFFFF for broadcast

    uint16_t sequence;      // sender-assigned, increments per message
    uint16_t payloadLength; // bytes of payload following this header
};
#pragma pack(pop)

// A node's identity. projectName/deviceTypeName are free text, not a
// shared enum (unlike ShackMate's SM_DeviceType) - this is what lets a
// brand-new RTS project's nodes show up in this gateway's device table
// with zero changes to this repo. See PROTOCOL.md's "Device identity"
// section.
#pragma pack(push, 1)
struct RTSNOW_DeviceIdentity
{
    uint32_t deviceID;           // persistent, derived from the node's Wi-Fi station MAC (last 4 bytes)
    char     projectName[16];    // e.g. "FN-Tester", "DryerHealth" - null-terminated, null-padded
    char     deviceTypeName[16]; // e.g. "FN-2Wire-Pod", "Dryer-Node" - project-defined, null-terminated, null-padded
    char     friendlyName[24];   // e.g. "FN-POD-01", "Dryer 3 Node" - null-terminated, null-padded
    uint8_t  firmwareVersionMajor;
    uint8_t  firmwareVersionMinor;
    uint8_t  firmwareVersionPatch;
    uint32_t ipv4Address;        // this node's Wi-Fi station IP (host byte order), or 0 if not currently Wi-Fi-joined
};
#pragma pack(pop)

// Sentinel for "no WiFi RSSI reading available" (not yet WiFi-joined, or
// this heartbeat came from firmware built before wifiRssi existed) -
// chosen below any real WiFi RSSI (which never gets anywhere near the
// int8_t floor), so a caller can tell "no reading" apart from a
// legitimately terrible-but-real signal.
constexpr int8_t RTSNOW_RSSI_UNKNOWN = -128;

// wifiRssi is appended at the end, deliberately NOT inside
// RTSNOW_DeviceIdentity itself: that struct is also the entire payload of
// RTSNOW_ANNOUNCE and is nested inside other structs across projects
// (PolymerPak, node-firmware) that won't all be rebuilt/reflashed the
// moment this field is added. Growing it in place would shift every byte
// offset after it for any struct embedding it, silently misreading older
// firmware's still-shorter announce/heartbeat payloads instead of just
// missing one field. Appending only to the tail of RTSNOW_Heartbeat
// keeps identity's own layout (and this struct's own leading
// identity/uptimeSeconds bytes) byte-for-byte unchanged, so an older
// node's shorter heartbeat is simply a valid prefix of this one - see
// gateway-firmware's on_espnow_recv() RTSNOW_HEARTBEAT case, which
// accepts that shorter prefix and defaults wifiRssi to
// RTSNOW_RSSI_UNKNOWN rather than requiring every node on the mesh to be
// reflashed in lockstep.
// boardName follows the exact same tail-append reasoning as wifiRssi just
// above (same struct, same backward-compat mechanism in
// gateway-firmware's on_espnow_recv() - one shared "minimum accepted
// size" check against the pre-wifiRssi base, then a length-clamped copy
// that leaves any not-yet-present trailing field at whatever default the
// caller pre-set). It's a physical hardware variant (e.g. "AtomS3Lite",
// "AtomS3POE", "M5Tough" - see RTSNow-SPI-CCP's BOARD_NAME build flag),
// distinct from RTSNOW_DeviceIdentity.deviceTypeName, which describes the
// node's software role instead (e.g. "FN-MAIN-Node") - the same
// deviceTypeName can run on more than one physical board. Empty string
// means "not reported" (older firmware, or a project that hasn't set
// RTSNowNodeConfig::boardName), same convention as every other
// optional/unknown field in this protocol.
// One entry in a node's own view of which other nodes it can currently
// hear over ESP-NOW, and how well - see RTSNOW_Heartbeat.neighbors below.
// Keyed by deviceID rather than MAC to keep this small (a neighbor table
// entry is 5 bytes here vs. 9 for a MAC + rssi pair) - every consumer
// (gateway, desktop app) already has deviceID<->MAC mappings from each
// node's own RTSNOW_DeviceIdentity, so nothing is lost by not repeating
// the MAC here too.
#pragma pack(push, 1)
struct RTSNOW_NeighborInfo
{
    uint32_t deviceID;
    int8_t rssi; // RTSNOW_RSSI_UNKNOWN is never valid here - see neighborCount below instead
};
#pragma pack(pop)

// Small enough to keep RTSNOW_Heartbeat comfortably under ESP-NOW's
// 250-byte total frame cap (header + payload) even with every other
// field already appended, generous enough for any realistic node count
// on one of these deployments (a handful of dryers, not hundreds).
constexpr uint8_t RTSNOW_MAX_NEIGHBORS = 8;

// boardName follows the exact same tail-append reasoning as wifiRssi just
// above (same struct, same backward-compat mechanism in
// gateway-firmware's on_espnow_recv() - one shared "minimum accepted
// size" check against the pre-wifiRssi base, then a length-clamped copy
// that leaves any not-yet-present trailing field at whatever default the
// caller pre-set). It's a physical hardware variant (e.g. "AtomS3Lite",
// "AtomS3POE", "M5Tough" - see RTSNow-SPI-CCP's BOARD_NAME build flag),
// distinct from RTSNOW_DeviceIdentity.deviceTypeName, which describes the
// node's software role instead (e.g. "FN-MAIN-Node") - the same
// deviceTypeName can run on more than one physical board. Empty string
// means "not reported" (older firmware, or a project that hasn't set
// RTSNowNodeConfig::boardName), same convention as every other
// optional/unknown field in this protocol.
//
// neighbors/neighborCount: this node's own, locally-sniffed view of which
// OTHER nodes it can currently hear over ESP-NOW (see rtsnow_node.cpp's
// onWifiPromiscuousRx and the peer table it feeds) - diagnostic only,
// there is no relaying/routing built on top of this yet. Same tail-append
// treatment as wifiRssi/boardName: neighborCount defaults to 0 (and stays
// 0, harmlessly, for a shorter heartbeat from firmware that pre-dates
// this field) rather than requiring every node to be reflashed in
// lockstep.
#pragma pack(push, 1)
struct RTSNOW_Heartbeat
{
    RTSNOW_DeviceIdentity identity;
    uint32_t uptimeSeconds;
    int8_t wifiRssi;     // this node's own WiFi.RSSI() to its AP, or RTSNOW_RSSI_UNKNOWN
    char boardName[16];  // e.g. "AtomS3Lite" - null-terminated, null-padded; "" = not reported
    uint8_t neighborCount; // 0..RTSNOW_MAX_NEIGHBORS, how many of neighbors[] are valid
    RTSNOW_NeighborInfo neighbors[RTSNOW_MAX_NEIGHBORS];
};
#pragma pack(pop)

// Payload for RTSNOW_PROVISION_REQUEST: identity plus a nonce fixed for
// one continuous pairing attempt (regenerated only if the node reboots or
// gives up and starts a fresh attempt). Every gateway reply echoes it
// back so the node can tell a reply belongs to its current attempt.
#pragma pack(push, 1)
struct RTSNOW_ProvisionRequest
{
    RTSNOW_DeviceIdentity identity;
    uint32_t nonce;
};
#pragma pack(pop)

// Payload for RTSNOW_PROVISION_HOLD and RTSNOW_PROVISION_REJECTED - just
// the echoed nonce from the RTSNOW_ProvisionRequest being responded to.
#pragma pack(push, 1)
struct RTSNOW_ProvisionNonce
{
    uint32_t nonce;
};
#pragma pack(pop)

// Sent by the gateway to a node in reply to its RTSNOW_PROVISION_REQUEST,
// once an operator has approved it. nonce must match that request's nonce
// - the node ignores this message otherwise. NOT ENCRYPTED - see
// PROTOCOL.md's "Security note - credentials are sent in the clear."
#pragma pack(push, 1)
struct RTSNOW_WifiCredentials
{
    uint32_t nonce;
    char     ssid[32];     // matches IEEE 802.11's own 32-byte SSID limit
    char     password[64]; // matches WPA2-PSK's own 64-character max
};
#pragma pack(pop)

// What kind of value a generic setting carries. Append-only, same
// discipline as any other wire-format enum in this protocol.
enum RTSNOW_SettingValueType : uint8_t
{
    RTSNOW_SETTING_STRING = 0,
    RTSNOW_SETTING_INT32 = 1,
    RTSNOW_SETTING_FLOAT32 = 2,
    RTSNOW_SETTING_BOOL = 3,
};

// A single project-defined key/value setting pushed to a node - see
// PROTOCOL.md's "Generic settings" section for why this is a typed,
// fixed-size struct rather than per-project fields baked into this
// protocol. stringValue and the numeric union are both always present
// (unused bytes ignored per valueType) rather than variably packed -
// simpler and safer given the small size budget either way.
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

#pragma pack(push, 1)
struct RTSNOW_AckPayload
{
    uint16_t acknowledgedSequence;
};
#pragma pack(pop)

// A generic block of Modbus-style 16-bit holding registers, sent in reply
// to RTSNOW_REQUEST_REGISTERS - see PROTOCOL.md's "Remote control" section.
// Project-neutral by design (like RTSNOW_SettingPayload): the wire shape is
// just {start, count, values}, with what each register number actually
// means left entirely to the owning project's own documentation (e.g.
// DryerHealth/SPI-IM's ModbusRegisterMap.h) - this protocol doesn't need to
// know or care. 64 registers (128 bytes) comfortably covers every project
// using this so far while staying well under ESP-NOW's 250-byte payload cap
// even with the header and the two length fields included.
#pragma pack(push, 1)
struct RTSNOW_RegisterBlock
{
    uint16_t startRegister;  // first register number this block covers, e.g. 40001
    uint16_t registerCount;  // how many of `values` are valid, <= 64
    uint16_t values[64];
};
#pragma pack(pop)

// Requests a single register write - unlike RTSNOW_SET_SETTING (a named
// config key like "spiAddress"), this is project-neutral by register
// NUMBER, same as RTSNOW_RegisterBlock, and is meant for real physical-
// equipment-affecting writes (e.g. SPI-IM's Machine Status register 40014
// starts/stops an actual dryer) - a receiving node should apply exactly
// the same validation/safety path a locally-connected client (e.g. Modbus
// TCP) would use for the same register, not a shortcut around it. value
// is a float (not the raw uint16 RTSNOW_RegisterBlock reports) to match
// SPI-IM's own EquipmentModel::writeRegister(uint16_t, float) signature
// exactly - some registers are physically 4-byte float writes over
// SPI-CCP, not scaled integers, so passing a float through unchanged
// avoids reinventing per-register scaling here.
#pragma pack(push, 1)
struct RTSNOW_WriteRegister
{
    uint16_t reg;
    float value;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct RTSNOW_WriteRegisterAck
{
    uint16_t reg;   // echoed back
    uint8_t ok;     // 0 if this node doesn't know how to write this register, or the write itself failed
};
#pragma pack(pop)

// ---- Firmware update over ESP-NOW - see RTSNOW_OTA_START's own comment ----

// Opens the transfer. totalSize/totalChunks let the receiving node
// pre-validate it has room and know when the last chunk has arrived;
// md5 (32 lowercase hex chars + null terminator, matching what
// Update::setMD5() expects verbatim) is checked by Update.end() itself
// after every chunk is written, so a corrupted transfer is caught by the
// same trusted mechanism ArduinoOTA already relies on rather than a
// bespoke checksum.
#pragma pack(push, 1)
struct RTSNOW_OtaStart
{
    uint32_t totalSize;
    uint32_t totalChunks;
    char md5[33];
};
#pragma pack(pop)

// One piece of the firmware image. Sent and acknowledged one at a time
// (strict stop-and-wait, driven and retried entirely by the sender - see
// RTS-ESPNOW-Gateway's desktop app) rather than any kind of sliding
// window: Update::write() must be called in strict sequential order, and
// ESP-NOW's real-world loss/reordering rate (confirmed repeatedly on
// real hardware elsewhere in this protocol's own history) makes
// reordering-tolerant buffering more complexity than it's worth for a
// transfer that's already expected to be slow. data[220] keeps the
// whole struct, plus RTSNOW_Header, comfortably under ESP-NOW's 250-byte
// total frame cap (14 + 4 + 2 + 220 = 240).
#pragma pack(push, 1)
struct RTSNOW_OtaChunk
{
    uint32_t index;    // 0-based; the receiver only accepts this if it equals the next expected index
    uint16_t length;   // valid bytes in data[] - less than sizeof(data) for the final chunk
    uint8_t data[220];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct RTSNOW_OtaChunkAck
{
    uint32_t index;
    uint8_t ok; // 0 if this exact index wasn't accepted (wrong sequence, no transfer in progress, or the flash write itself failed) - the sender retries the same chunk
};
#pragma pack(pop)

// Reply to both RTSNOW_OTA_START (did Update.begin() succeed) and
// RTSNOW_OTA_END (did the complete image verify and get marked
// bootable) - same shape for both since both are just "did the thing
// you asked for work, and if not, why."
#pragma pack(push, 1)
struct RTSNOW_OtaAck
{
    uint8_t ok;
    char message[32]; // short human-readable reason on failure; empty on success
};
#pragma pack(pop)
