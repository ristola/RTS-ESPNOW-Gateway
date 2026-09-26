#pragma once

// Reusable "simple node" RTS-NOW integration: announce/heartbeat,
// friendlyName rename+persistence, and acking generic settings. No Wi-Fi
// provisioning dance - it assumes the host project already manages its
// own Wi-Fi connection (or has none at all and is mesh-only), and just
// wants to show up in RTS-ESPNOW-Gateway's known-devices table. This is
// the pattern generalized out of PolymerPak's own rtsnow_node.cpp - see
// ../../../PROTOCOL.md's "Mesh-only pairing" section for the design this
// mirrors.
//
// Not what RTS-ESPNOW-Gateway's own node-firmware uses - that project
// implements the fuller channel-sweep/hold/provision state machine
// in-tree (it exists specifically to exercise that handshake), which is
// a different, more involved mode this simple helper does not attempt to
// cover.
//
// Usage from a consuming project's setup()/loop() (after Wi-Fi, if any,
// is already up):
//
//   RTSNowNodeConfig cfg{};
//   cfg.projectName = "PolymerPak";
//   cfg.deviceTypeName = "SolarTracker";
//   cfg.defaultFriendlyName = "PolymerPak Tracker";
//   cfg.firmwareVersionMajor = 0;
//   cfg.firmwareVersionMinor = 1;
//   rtsnowNodeBegin(cfg);
//   ...
//   void loop() { rtsnowNodeLoop(); }

#include <cstdint>

#include "rtsnow_protocol.h"

struct RTSNowNodeConfig
{
    const char *projectName = "";         // truncated to 15 chars (RTSNOW_DeviceIdentity.projectName)
    const char *deviceTypeName = "";      // truncated to 15 chars
    const char *defaultFriendlyName = ""; // used only until a rename is saved; truncated to 23 chars
    // Optional: physical hardware variant (e.g. "AtomS3Lite", "M5Tough" -
    // see RTSNow-SPI-CCP's BOARD_NAME build flag), sent in every heartbeat
    // (RTSNOW_Heartbeat.boardName) - truncated to 15 chars. Left "" (the
    // default) reports nothing, same as any project that hasn't opted in.
    const char *boardName = "";
    uint8_t firmwareVersionMajor = 0;
    uint8_t firmwareVersionMinor = 0;
    uint8_t firmwareVersionPatch = 0;
    uint32_t heartbeatIntervalMs = 10000;

    // Optional: called from rtsnowNodeLoop() (never from the ESP-NOW receive
    // callback - same discipline as the rename/setting-ack handling below)
    // when a gateway/desktop-app RTSNOW_REQUEST_REGISTERS arrives, so the
    // host project can fill outBlock with its own current register values
    // (e.g. straight from its Modbus holding-register table). Left null
    // (the default) on a node that doesn't have any register table to
    // report - the request is then just silently not answered, same as an
    // unrecognized RTSNOW_SET_SETTING key.
    using RegisterBlockProvider = void (*)(RTSNOW_RegisterBlock &outBlock);
    RegisterBlockProvider registerBlockProvider = nullptr;

    // Optional: called once, synchronously, right before rtsnowNodeLoop()
    // calls ESP.restart() in response to a RTSNOW_REBOOT - a chance for the
    // host project to give a clear visible "rebooting now" signal on its
    // own hardware (blink an LED, show a screen message) before the reset
    // actually happens, since ESP.restart() itself gives no such warning on
    // its own. Free to block (e.g. with delay()) - there is nothing else
    // left to do after this returns. Left null (the default) restarts
    // immediately with no signal, same as every other ESP.restart() call
    // site in this codebase before this hook existed.
    using RebootHook = void (*)();
    RebootHook onBeforeReboot = nullptr;

    // Optional: called every time this node builds its RTSNOW_DeviceIdentity
    // (announce/heartbeat) to get its current IPv4 address (host byte
    // order), instead of the default WiFi.status()==WL_CONNECTED ?
    // WiFi.localIP() : 0. Needed by any host project whose IP connectivity
    // doesn't come from WiFi.localIP() at all - e.g. a board using wired
    // Ethernet instead, where WiFi.status() is never WL_CONNECTED (the
    // radio stays on, unassociated, purely so ESP-NOW still works) but the
    // node is very much reachable, just on a different interface. Left
    // null (the default) uses the WiFi-based check every project before
    // this hook existed already relied on.
    using Ipv4AddressProvider = uint32_t (*)();
    Ipv4AddressProvider ipv4AddressProvider = nullptr;

    // Optional: called from rtsnowNodeLoop() (never from the ESP-NOW
    // receive callback) for any RTSNOW_SET_SETTING key other than the
    // reserved "friendlyName" (which this library always handles itself).
    // Lets a host project define its own settable keys - e.g. SPI-IM
    // pushes "equipmentType"/"model"/"spiAddress"/"spiBaudRate" this way -
    // without needing its own separate esp_now_register_recv_cb (ESP-NOW
    // only supports one at a time, and this library already owns it).
    // Return true if the key was recognized and the value applied, false
    // otherwise - this becomes the RTSNOW_SETTING_ACK's `accepted` field,
    // so a client can tell a real rejection apart from a value that just
    // never arrived. Left null (the default) accepts every key
    // unconditionally without applying anything, same as every project's
    // behavior before this hook existed.
    using GenericSettingHandler = bool (*)(const RTSNOW_SettingPayload &setting);
    GenericSettingHandler onGenericSetting = nullptr;

    // Optional: called from rtsnowNodeLoop() whenever this node's ESP-NOW
    // OTA transfer becomes active or inactive - true once an OTA_START is
    // accepted, false once OTA_END or OTA_ABORT resolves it (success or
    // failure either way). Lets a host project drive the same "update in
    // progress" visual signal its WiFi OTA already shows via ArduinoOTA's
    // own onStart/onError (e.g. blinking an LED), without this library
    // needing to know anything about LEDs/displays itself - mirrors
    // onBeforeReboot's role for reboots. Only ever called on an actual
    // true/false transition, never redundantly every loop() tick. Left null
    // (the default) - no signal, same as every project before this hook
    // existed.
    using OtaActiveHook = void (*)(bool active);
    OtaActiveHook onOtaActiveChanged = nullptr;
};

void rtsnowNodeBegin(const RTSNowNodeConfig &config);
void rtsnowNodeLoop();

// The node's current friendlyName (post-rename, if any) - lets a host
// project display it on its own UI/logs without keeping a second copy.
const char *rtsnowNodeFriendlyName();

// Public ceiling on how many peers rtsnowNodePeers() can ever report -
// rtsnow_node.cpp's own internal peer table size is defined as this
// constant, not a separate number, so the two can never drift apart.
// Generous vs. RTSNOW_MAX_NEIGHBORS (the wire-format heartbeat's own,
// much smaller cap - see rtsnow_protocol.h) since a caller here isn't
// limited by one ESP-NOW frame's payload budget.
constexpr uint8_t RTSNOW_NODE_MAX_PEERS = 16;

// One peer this node currently has any discovery data for - unlike
// RTSNOW_NeighborInfo (the wire-format heartbeat's own neighbor entry,
// deviceID+rssi only), this includes the MAC: a raw deviceID means
// nothing to a human looking at a status page, a MAC is what they
// actually recognize. rssi/lastSeenMs stay at RTSNOW_RSSI_UNKNOWN/0 until
// this node's WiFi-promiscuous RSSI tap has actually heard this peer at
// least once.
struct RTSNowPeerSnapshot
{
    uint32_t deviceID = 0;
    uint8_t mac[6] = {0};
    int8_t rssi = RTSNOW_RSSI_UNKNOWN;
    uint32_t lastSeenMs = 0; // millis() timestamp; meaningless (0) while rssi == RTSNOW_RSSI_UNKNOWN
};

// Snapshots this node's current neighbor-discovery table for a host
// project's own read-only diagnostics (e.g. a status page) - deliberately
// NOT the same view this file's own buildNeighborList() computes for the
// wire-format heartbeat: that one drops any peer whose sniffed RSSI is
// unknown or older than its own staleness window, since the gateway's
// routing math has no use for a reading it can't trust as current. A
// status page is a different consumer with a different need - it can
// reasonably want to show a neighbor's last known RSSI, labeled with its
// own age, even if a few minutes stale, rather than have the neighbor
// just vanish. So this function applies no staleness or RSSI-known
// filtering at all; every in-use table entry is reported as-is, staleness
// left entirely as the caller's own display decision. Fills outPeers
// (capacity maxOut) and returns the count actually written.
uint8_t rtsnowNodePeers(RTSNowPeerSnapshot *outPeers, uint8_t maxOut);
