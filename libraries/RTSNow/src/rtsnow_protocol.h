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

#pragma pack(push, 1)
struct RTSNOW_Heartbeat
{
    RTSNOW_DeviceIdentity identity;
    uint32_t uptimeSeconds;
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
