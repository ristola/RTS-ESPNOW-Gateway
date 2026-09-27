// RTS ESP-NOW Gateway - M5Stack AtomS3U firmware.
//
// A project-agnostic ESP-NOW gateway: discovers nodes from ANY RTS
// project, provisions a fresh node's real Wi-Fi credentials over the air,
// and pushes generic per-project settings - all driven by human-typeable
// commands over USB serial (no touchscreen on this hardware, unlike
// CYD-4.3-FN-Tester's CYD). See ../PROTOCOL.md for the full wire-format
// design and rationale, and ../PROJECT_MEMORY.md for how this project
// came to exist and the design decisions below.
//
// Unlike CYD-4.3-FN-Tester's CYD (which passively listens on whatever
// channel its own Wi-Fi AP happens to use), this gateway has no AP of its
// own to inherit a channel from - it just picks and stays on one fixed
// ESP-NOW channel (persisted, default channel 1; change with the
// `channel <n>` command). This gateway itself never joins Wi-Fi at any
// point - see platformio.ini's header comment.

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <Update.h>
#include <esp_now.h>
#include <esp_system.h>  // esp_reset_reason() - System Information page
#include <esp_wifi.h>

#if defined(BOARD_GATEWAY_ATOMS3_POE)
#include <M5_Ethernet.h>
#include <SPI.h>
#include <utility/w5100.h>  // for W5100.init() directly - see gateway_ethernet_begin()'s own comment
#include "secrets.h"         // GATEWAY_WIFI_SSID/PASSWORD - WiFi fallback only, gitignored
#include <ESPmDNS.h>          // WiFi-fallback mDNS only - see kMdnsHostname's own comment for the Ethernet story
#include "rtslogo.h"          // RTS logo, served at /logo.gif by the HTTP status page below
#endif

#include "rtsnow_protocol.h"

// ---- JSON-lines serial protocol (for a desktop app; see ../../SERIAL_PROTOCOL.md) ----
//
// Coexists with the human-typeable text commands below on the same USB
// serial stream: an incoming line is JSON if its first non-whitespace
// character is '{', otherwise it's the legacy text command parser. Every
// JSON-line message (both directions) is exactly one JsonDocument
// serialized to a single line - never split across lines, never mixed
// with the plain-text banner/help output a human sees (that stays
// unchanged) - so a client just ignores any line that doesn't start
// with '{'.

namespace
{
    // M5Stack AtomS3U onboard hardware - see platformio.ini's header
    // comment for the "no dedicated board definition" note.
    constexpr uint8_t kRgbLedPin = 35;
    constexpr uint8_t kButtonPin = 41; // not used by this firmware yet - reserved

    constexpr const char *kPrefsNamespace = "rtsnow_gw";
    constexpr const char *kChannelKey = "channel";
    constexpr uint8_t kDefaultChannel = 1;

    constexpr uint32_t kPendingTimeoutMs = 60000;  // a pending provisioning request this stale without a refresh is assumed abandoned
    constexpr uint32_t kConfirmTimeoutMs = 30000;  // how long to wait for a node's post-credentials RTSNOW_ANNOUNCE before giving up
    constexpr uint32_t kTrafficFlashMs = 60;

    const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // Single onboard WS2812 (unlike the AtomS3 Lite's 4-chained ones).
    constexpr uint16_t kLedCount = 1;
    Adafruit_NeoPixel s_led(kLedCount, kRgbLedPin, NEO_GRB + NEO_KHZ800);

    uint8_t s_channel = kDefaultChannel;
    uint16_t s_next_sequence = 0;
    uint32_t s_tx_flash_until_ms = 0;
    uint32_t s_rx_flash_until_ms = 0;

    struct KnownDevice
    {
        uint32_t deviceID;
        char projectName[16];
        char deviceTypeName[16];
        char friendlyName[24];
        uint8_t mac[6];
        uint32_t ipv4Address;
        uint32_t lastSeenMs;
        // Carried in every RTSNOW_DeviceIdentity announce/heartbeat (see
        // PROTOCOL.md) but never surfaced to the desktop app until now -
        // refreshed on every heartbeat below, not just first announce, so
        // an OTA-updated node's new version shows up without needing to
        // re-pair.
        uint8_t firmwareVersionMajor;
        uint8_t firmwareVersionMinor;
        uint8_t firmwareVersionPatch;
        // wifiRssi: this node's own WiFi.RSSI() to its AP, self-reported in
        // RTSNOW_Heartbeat (see rtsnow_protocol.h's RTSNOW_RSSI_UNKNOWN
        // comment for why it's a tail-appended field, and why older
        // firmware's shorter heartbeat just leaves this at the sentinel
        // instead of being rejected outright). espNowRssi: the RSSI this
        // gateway itself measured on this device's most recent ESP-NOW
        // frame - a completely different physical link (node<->gateway,
        // not node<->router) - see on_wifi_promiscuous_rx() below, the
        // only writer of this field.
        int8_t wifiRssi = RTSNOW_RSSI_UNKNOWN;
        int8_t espNowRssi = RTSNOW_RSSI_UNKNOWN;
        // Physical hardware variant (e.g. "AtomS3Lite", "M5Tough"), self-
        // reported in RTSNOW_Heartbeat.boardName - same tail-appended-field
        // treatment as wifiRssi above. Empty until a heartbeat actually
        // reports one (older firmware, or a project that hasn't set
        // RTSNowNodeConfig::boardName).
        char boardName[16] = "";
        // This node's own view of which OTHER nodes it can currently hear
        // over ESP-NOW (see rtsnow_node.cpp's neighbor-discovery section) -
        // feeds recompute_routing() below. neighborCount 0 covers both
        // "genuinely has none" and "older firmware that doesn't report
        // this" identically - same as every other tail-appended heartbeat
        // field, there's no way (or need) to tell those two apart.
        uint8_t neighborCount = 0;
        RTSNOW_NeighborInfo neighbors[RTSNOW_MAX_NEIGHBORS];
        // 0 = send directly to this device (the default, and the only
        // option before recompute_routing() ever runs) - otherwise the
        // deviceID of another known device to relay outbound commands
        // through instead, because it has a meaningfully better combined
        // path to this device than the gateway does directly. See
        // recompute_routing()'s own comment for exactly how "meaningfully
        // better" is decided, and rtsnow_node.cpp's onEspNowRecv for how
        // the chosen relay actually forwards the frame on.
        uint32_t routeViaDeviceId = 0;
        // True only when THIS device's own most recent heartbeat was
        // provably long enough on the wire to include the relay-capable
        // rtsnow_node.cpp's neighbor-discovery/relay-forwarding logic -
        // see the RTSNOW_HEARTBEAT case in on_espnow_recv, the only
        // writer of this field, which has the actual received
        // payloadLen on hand to check this precisely. Deliberately NOT
        // inferred from neighborCount/wifiRssi/boardName being
        // zero/empty - those are all "0 might mean legitimately none, or
        // might mean not reported" by design (see their own comments),
        // and inferring capability from an ambiguous default very nearly
        // shipped a real bug here: recompute_routing() picked a relay
        // candidate that turned out to be running firmware with no
        // relay-forwarding code at all, since it happened to also read
        // as "0 neighbors" instead of "capability unknown."
        // recompute_routing() must never pick a candidate with this
        // false - an old-firmware "relay" doesn't understand
        // destinationID at all, so a message unicast to it addressed to
        // someone else wouldn't get silently dropped, it would get
        // wrongly processed as if addressed to itself (e.g. a
        // friendlyName rename meant for the real target applying to the
        // chosen non-relay-capable "relay" instead).
        bool supportsRelay = false;
    };
    constexpr int kMaxKnownDevices = 32;
    KnownDevice s_known_devices[kMaxKnownDevices];
    int s_known_device_count = 0;

    struct PendingRequest
    {
        uint8_t mac[6];
        uint32_t deviceID;
        char projectName[16];
        char deviceTypeName[16];
        char friendlyName[24];
        uint32_t nonce;
        uint32_t receivedMs;
        bool inUse;
    };
    constexpr int kMaxPendingRequests = 8;
    PendingRequest s_pending[kMaxPendingRequests];

    struct AwaitingConfirm
    {
        uint32_t deviceID;
        uint8_t mac[6];
        char ssid[32];
        uint32_t sentMs;
        bool inUse;
    };
    constexpr int kMaxAwaitingConfirm = 8;
    AwaitingConfirm s_awaiting_confirm[kMaxAwaitingConfirm];

    uint32_t local_device_id()
    {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        return (static_cast<uint32_t>(mac[2]) << 24) | (static_cast<uint32_t>(mac[3]) << 16) |
               (static_cast<uint32_t>(mac[4]) << 8) | static_cast<uint32_t>(mac[5]);
    }

    void mac_to_str(const uint8_t mac[6], char *out, size_t outSize)
    {
        snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    bool parse_mac(const char *str, uint8_t out[6])
    {
        unsigned int b[6];
        if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
            return false;
        for (int i = 0; i < 6; i++)
            out[i] = static_cast<uint8_t>(b[i]);
        return true;
    }

#if defined(BOARD_GATEWAY_ATOMS3_POE)
    // Defined much later in this file (with the rest of the Ethernet/TCP
    // server code) - forward-declared here since drain_output_queue()
    // below needs to call it well before its own definition.
    void broadcast_line_to_tcp_clients(const String &line);
#endif

    // ---- Deferred Serial output ----
    //
    // esp_now_register_recv_cb's callback (on_espnow_recv and everything it
    // calls) runs in the Wi-Fi/ESP-NOW driver's own task context, not
    // loop()'s. Writing to Serial (this board's native USB CDC) directly
    // from there was observed on real hardware to corrupt output - bytes
    // silently dropped mid-line, sometimes across both a human-readable
    // line and the JSON line right after it. Adding Serial.flush() did not
    // fix it (confirming this is a scheduling/context problem, not a
    // buffering one) - the actual fix is to never call Serial.print* from
    // that context at all. Anything the receive callback wants to log
    // (plain text or a JSON event) is queued here instead; only loop()
    // (via drain_output_queue) ever writes to Serial for these paths.
    portMUX_TYPE s_output_mux = portMUX_INITIALIZER_UNLOCKED;
    constexpr int kOutputQueueSize = 24;
    String s_output_queue[kOutputQueueSize];
    int s_output_queue_head = 0;
    int s_output_queue_count = 0;

    void queue_output(const String &line)
    {
        portENTER_CRITICAL(&s_output_mux);
        if (s_output_queue_count < kOutputQueueSize)
        {
            int idx = (s_output_queue_head + s_output_queue_count) % kOutputQueueSize;
            s_output_queue[idx] = line;
            s_output_queue_count++;
        }
        // else: queue full - drop rather than block: this is diagnostic
        // output, never protocol-critical (the ESP-NOW/Wi-Fi state changes
        // it describes have already happened regardless of whether this
        // line makes it out).
        portEXIT_CRITICAL(&s_output_mux);
    }

    void drain_output_queue()
    {
        while (true)
        {
            String line;
            bool has = false;
            portENTER_CRITICAL(&s_output_mux);
            if (s_output_queue_count > 0)
            {
                line = s_output_queue[s_output_queue_head];
                s_output_queue_head = (s_output_queue_head + 1) % kOutputQueueSize;
                s_output_queue_count--;
                has = true;
            }
            portEXIT_CRITICAL(&s_output_mux);
            if (!has)
                break;
            Serial.println(line);
#if defined(BOARD_GATEWAY_ATOMS3_POE)
            // events/acks are already ID-tagged or inherently public data
            // (hello/known_devices/heartbeats) - a TCP client seeing an ack
            // that isn't its own and ignoring it is no different from two
            // USB clients existing today, so no protocol change needed to
            // broadcast the same line to every connected TCP client too.
            broadcast_line_to_tcp_clients(line);
#endif
        }
    }

    // ---- JSON event emission (see the header comment above) ----

    // Copies the "id" field from an incoming command's JsonDocument onto an
    // outgoing response, if the command had one - lets a client correlate
    // a response to the request that caused it. No-op (response has no
    // "id") if the command didn't include one.
    void echo_id(JsonDocument &out, const JsonDocument &inDoc)
    {
        JsonVariantConst id = inDoc["id"];
        if (!id.isNull())
            out["id"] = id;
    }

    // Queues rather than writes directly - safe to call from both loop()
    // and the ESP-NOW receive callback context (see "Deferred Serial
    // output" above).
    void emit_json(JsonDocument &doc)
    {
        String out;
        serializeJson(doc, out);
        queue_output(out);
    }

    void send_ack(const char *cmd, const JsonDocument *inDoc, bool ok, const char *error = nullptr)
    {
        JsonDocument doc;
        doc["event"] = "ack";
        doc["cmd"] = cmd;
        doc["ok"] = ok;
        if (error != nullptr)
            doc["error"] = error;
        if (inDoc != nullptr)
            echo_id(doc, *inDoc);
        emit_json(doc);
    }

    // Deliberately unencrypted - see PROTOCOL.md's "Security note" for why
    // (ESP-NOW's per-peer AES needs both sides pre-registered before the
    // first encrypted packet, impossible for a gateway and a node that
    // have never talked before - the same wall CYD-4.3-FN-Tester's
    // ShackMate protocol hit and reverted from).
    void ensure_peer(const uint8_t mac[6], uint8_t channel = 0)
    {
        if (esp_now_is_peer_exist(mac))
            return;

        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = channel;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    // destinationID defaults to broadcast (0xFFFFFFFF) for every existing
    // call site, which is also what a genuinely direct, non-relayed send
    // needs: the ESP-NOW hardware itself only ever delivers a unicast
    // frame to its actual physical destination MAC regardless of what
    // this logical field says, so an accurate destinationID only starts
    // to matter for the one new case that actually sets it explicitly -
    // do_setting_send/do_reboot_send/do_request_registers_send routing a
    // command through a relay (see recompute_routing()), where `mac`
    // here is the RELAY's address but destinationID names the real,
    // final target so that relay's own onEspNowRecv (see
    // rtsnow_node.cpp) knows who to forward it to.
    bool send_to(const uint8_t mac[6], RTSNOW_MessageType type, const void *payload, uint16_t payloadLen,
                 uint32_t destinationID = 0xFFFFFFFF)
    {
        uint8_t buf[250];
        if (sizeof(RTSNOW_Header) + payloadLen > sizeof(buf))
            return false;

        RTSNOW_Header header;
        header.version = 1;
        header.messageType = type;
        header.sourceID = local_device_id();
        header.destinationID = destinationID;
        header.sequence = s_next_sequence++;
        header.payloadLength = payloadLen;

        memcpy(buf, &header, sizeof(header));
        if (payloadLen > 0)
            memcpy(buf + sizeof(header), payload, payloadLen);

        bool ok = esp_now_send(mac, buf, sizeof(header) + payloadLen) == ESP_OK;
        s_tx_flash_until_ms = millis() + kTrafficFlashMs;
        return ok;
    }

    bool send_broadcast(RTSNOW_MessageType type, const void *payload, uint16_t payloadLen)
    {
        return send_to(kBroadcastMac, type, payload, payloadLen);
    }

    // ---- Known-device table (RTSNOW_ANNOUNCE / RTSNOW_HEARTBEAT) ----

    // Distinct from a real "0 neighbors" (a heartbeat whose sender
    // genuinely can't currently hear any peer) - see note_known_device's
    // neighborCount handling, which needs to tell "this caller has no
    // neighbor data at all" (RTSNOW_ANNOUNCE) apart from "this caller
    // affirmatively reports zero" (a RTSNOW_HEARTBEAT with an empty
    // neighbor list).
    constexpr uint8_t kNeighborCountNotProvided = 0xFF;

    // wifiRssi defaults to RTSNOW_RSSI_UNKNOWN, boardName to "", and
    // neighborCount to kNeighborCountNotProvided for callers
    // (RTSNOW_ANNOUNCE) that have none of these - only ever overwrites
    // the stored value when the caller actually has a fresh one
    // (RTSNOW_HEARTBEAT), so an announce arriving after a heartbeat
    // doesn't blank out already-known-good data.
    void note_known_device(const uint8_t mac[6], const RTSNOW_DeviceIdentity &identity,
                            int8_t wifiRssi = RTSNOW_RSSI_UNKNOWN, const char *boardName = "",
                            uint8_t neighborCount = kNeighborCountNotProvided,
                            const RTSNOW_NeighborInfo *neighbors = nullptr)
    {
        for (int i = 0; i < s_known_device_count; i++)
        {
            if (s_known_devices[i].deviceID == identity.deviceID)
            {
                strncpy(s_known_devices[i].projectName, identity.projectName, sizeof(s_known_devices[i].projectName) - 1);
                strncpy(s_known_devices[i].deviceTypeName, identity.deviceTypeName, sizeof(s_known_devices[i].deviceTypeName) - 1);
                strncpy(s_known_devices[i].friendlyName, identity.friendlyName, sizeof(s_known_devices[i].friendlyName) - 1);
                memcpy(s_known_devices[i].mac, mac, 6);
                s_known_devices[i].ipv4Address = identity.ipv4Address;
                s_known_devices[i].lastSeenMs = millis();
                s_known_devices[i].firmwareVersionMajor = identity.firmwareVersionMajor;
                s_known_devices[i].firmwareVersionMinor = identity.firmwareVersionMinor;
                s_known_devices[i].firmwareVersionPatch = identity.firmwareVersionPatch;
                if (wifiRssi != RTSNOW_RSSI_UNKNOWN)
                    s_known_devices[i].wifiRssi = wifiRssi;
                if (boardName[0] != '\0')
                {
                    strncpy(s_known_devices[i].boardName, boardName, sizeof(s_known_devices[i].boardName) - 1);
                    s_known_devices[i].boardName[sizeof(s_known_devices[i].boardName) - 1] = '\0';
                }
                if (neighborCount != kNeighborCountNotProvided)
                {
                    uint8_t n = neighborCount < RTSNOW_MAX_NEIGHBORS ? neighborCount : RTSNOW_MAX_NEIGHBORS;
                    s_known_devices[i].neighborCount = n;
                    memcpy(s_known_devices[i].neighbors, neighbors, n * sizeof(RTSNOW_NeighborInfo));
                    // A heartbeat long enough to carry a real
                    // neighborCount is proof (not inference) that this
                    // device's firmware includes rtsnow_node.cpp's
                    // relay-forwarding logic - see supportsRelay's own
                    // comment. Only ever set true here, never reset to
                    // false by an announce/old-style-heartbeat call
                    // (neighborCount == kNeighborCountNotProvided for
                    // both, so this branch simply doesn't run for them).
                    s_known_devices[i].supportsRelay = true;
                }
                return;
            }
        }

        int slot;
        if (s_known_device_count < kMaxKnownDevices)
        {
            slot = s_known_device_count;
            s_known_device_count++;
        }
        else
        {
            slot = 0;
            for (int i = 1; i < kMaxKnownDevices; i++)
                if (s_known_devices[i].lastSeenMs < s_known_devices[slot].lastSeenMs)
                    slot = i;
        }

        s_known_devices[slot].deviceID = identity.deviceID;
        strncpy(s_known_devices[slot].projectName, identity.projectName, sizeof(s_known_devices[slot].projectName) - 1);
        s_known_devices[slot].projectName[sizeof(s_known_devices[slot].projectName) - 1] = '\0';
        strncpy(s_known_devices[slot].deviceTypeName, identity.deviceTypeName, sizeof(s_known_devices[slot].deviceTypeName) - 1);
        s_known_devices[slot].deviceTypeName[sizeof(s_known_devices[slot].deviceTypeName) - 1] = '\0';
        strncpy(s_known_devices[slot].friendlyName, identity.friendlyName, sizeof(s_known_devices[slot].friendlyName) - 1);
        s_known_devices[slot].friendlyName[sizeof(s_known_devices[slot].friendlyName) - 1] = '\0';
        memcpy(s_known_devices[slot].mac, mac, 6);
        s_known_devices[slot].ipv4Address = identity.ipv4Address;
        s_known_devices[slot].lastSeenMs = millis();
        s_known_devices[slot].firmwareVersionMajor = identity.firmwareVersionMajor;
        s_known_devices[slot].firmwareVersionMinor = identity.firmwareVersionMinor;
        s_known_devices[slot].firmwareVersionPatch = identity.firmwareVersionPatch;
        s_known_devices[slot].wifiRssi = wifiRssi;
        s_known_devices[slot].espNowRssi = RTSNOW_RSSI_UNKNOWN;
        strncpy(s_known_devices[slot].boardName, boardName, sizeof(s_known_devices[slot].boardName) - 1);
        s_known_devices[slot].boardName[sizeof(s_known_devices[slot].boardName) - 1] = '\0';
        s_known_devices[slot].supportsRelay = (neighborCount != kNeighborCountNotProvided);
        if (neighborCount != kNeighborCountNotProvided)
        {
            uint8_t n = neighborCount < RTSNOW_MAX_NEIGHBORS ? neighborCount : RTSNOW_MAX_NEIGHBORS;
            s_known_devices[slot].neighborCount = n;
            memcpy(s_known_devices[slot].neighbors, neighbors, n * sizeof(RTSNOW_NeighborInfo));
        }
        else
        {
            s_known_devices[slot].neighborCount = 0;
        }

        char logLine[128];
        snprintf(logLine, sizeof(logLine), "New device: id=0x%08X project=\"%s\" type=\"%s\" name=\"%s\"",
                 identity.deviceID, identity.projectName, identity.deviceTypeName, identity.friendlyName);
        queue_output(logLine); // called from the ESP-NOW callback - see "Deferred Serial output"

        JsonDocument doc;
        doc["event"] = "device_announced";
        JsonObject dev = doc["device"].to<JsonObject>();
        char macStr[18];
        mac_to_str(mac, macStr, sizeof(macStr));
        dev["deviceID"] = identity.deviceID;
        dev["projectName"] = identity.projectName;
        dev["deviceTypeName"] = identity.deviceTypeName;
        dev["friendlyName"] = identity.friendlyName;
        dev["mac"] = macStr;
        if (identity.ipv4Address != 0)
            dev["ip"] = IPAddress(identity.ipv4Address).toString();
        char verStr[16];
        snprintf(verStr, sizeof(verStr), "%u.%u.%u", identity.firmwareVersionMajor,
                 identity.firmwareVersionMinor, identity.firmwareVersionPatch);
        dev["firmwareVersion"] = verStr;
        if (wifiRssi != RTSNOW_RSSI_UNKNOWN)
            dev["wifiRssi"] = wifiRssi;
        if (boardName[0] != '\0')
            dev["boardName"] = boardName;
        emit_json(doc);
    }

    int find_known_device_by_mac(const uint8_t mac[6])
    {
        for (int i = 0; i < s_known_device_count; i++)
            if (memcmp(s_known_devices[i].mac, mac, 6) == 0)
                return i;
        return -1;
    }

    int find_known_device_by_device_id(uint32_t deviceID)
    {
        for (int i = 0; i < s_known_device_count; i++)
            if (s_known_devices[i].deviceID == deviceID)
                return i;
        return -1;
    }

    // Resolves the MAC string to report for an incoming message's TRUE
    // origin (header.sourceID), not the physical sender - those differ
    // whenever the message was relayed (see rtsnow_node.cpp's single-hop
    // forwarding), and reporting the relay's own MAC instead of the real
    // originator's would misattribute the event to the wrong device -
    // exactly the class of bug already found and fixed once for routing
    // decisions (see KnownDevice.supportsRelay's own history). Falls back
    // to the physical `mac` only if this deviceID isn't recognized at
    // all, which shouldn't normally happen for a device that just sent an
    // OTA ack (it must have announced/heartbeated to be routable in the
    // first place).
    void resolve_true_mac_str(uint32_t sourceDeviceId, const uint8_t physicalMac[6], char *out, size_t outSize)
    {
        int idx = find_known_device_by_device_id(sourceDeviceId);
        mac_to_str(idx >= 0 ? s_known_devices[idx].mac : physicalMac, out, outSize);
    }

    // Require a candidate relay path to beat a direct send by a real
    // margin, not just any positive difference - RSSI naturally jitters
    // a few dB between readings even with nothing physically changing,
    // and flipping routing back and forth on that noise would make
    // behavior harder to reason about for no real benefit. 10dB is a
    // clearly-audible difference in link quality, not a rounding error.
    constexpr int8_t kRelayImprovementThresholdDb = 10;

    // Recomputes every known device's routeViaDeviceId (see that field's
    // own comment) from the same espNowRssi/neighbors data already
    // collected for the Hardware List table and Mesh page - no new data
    // collection here, purely a decision made from what's already known.
    // Deliberately called only from loop() (see its own call site), never
    // from note_known_device()/on_espnow_recv() directly - this walks
    // every known device's full neighbor list, which is more work than
    // belongs inside the ESP-NOW receive callback's own task context
    // (see "Deferred Serial output" above for why that context stays
    // cheap on purpose).
    //
    // For each target device, considers every OTHER device it reports as
    // a neighbor as a candidate relay - but only one this gateway also
    // knows directly (a candidate relay is useless if the gateway itself
    // can't reach it either). A candidate path's quality is its weakest
    // single hop (gateway->candidate, candidate->target) - a route is
    // never better than its worst leg. The best candidate replaces direct
    // sending only if it beats direct by kRelayImprovementThresholdDb;
    // ties or small improvements aren't worth the added complexity of a
    // relay hop (more latency, one more thing that can fail).
    void recompute_routing()
    {
        for (int i = 0; i < s_known_device_count; i++)
        {
            KnownDevice &target = s_known_devices[i];
            int8_t directQuality = target.espNowRssi;

            int8_t bestRelayQuality = RTSNOW_RSSI_UNKNOWN;
            int bestRelayIdx = -1;
            for (uint8_t ni = 0; ni < target.neighborCount; ni++)
            {
                int candidateIdx = find_known_device_by_device_id(target.neighbors[ni].deviceID);
                if (candidateIdx < 0 || candidateIdx == i)
                    continue;
                if (!s_known_devices[candidateIdx].supportsRelay)
                    continue; // confirmed relay-capable firmware only - see supportsRelay's own comment for why this check exists at all
                int8_t gatewayToCandidate = s_known_devices[candidateIdx].espNowRssi;
                if (gatewayToCandidate == RTSNOW_RSSI_UNKNOWN)
                    continue; // gateway can't reach this candidate directly either - useless as a relay
                int8_t candidateToTarget = target.neighbors[ni].rssi;
                int8_t pathQuality = gatewayToCandidate < candidateToTarget ? gatewayToCandidate : candidateToTarget;
                if (bestRelayQuality == RTSNOW_RSSI_UNKNOWN || pathQuality > bestRelayQuality)
                {
                    bestRelayQuality = pathQuality;
                    bestRelayIdx = candidateIdx;
                }
            }

            bool shouldRelay = bestRelayIdx >= 0 &&
                                (directQuality == RTSNOW_RSSI_UNKNOWN ||
                                 bestRelayQuality > directQuality + kRelayImprovementThresholdDb);
            uint32_t newRouteVia = shouldRelay ? s_known_devices[bestRelayIdx].deviceID : 0;

            if (newRouteVia != target.routeViaDeviceId)
            {
                char logLine[160];
                if (newRouteVia != 0)
                    snprintf(logLine, sizeof(logLine),
                             "Routing to \"%s\" via \"%s\" now (direct %d dBm, via %d dBm)", target.friendlyName,
                             s_known_devices[bestRelayIdx].friendlyName, directQuality, bestRelayQuality);
                else
                    snprintf(logLine, sizeof(logLine), "Routing to \"%s\" direct now (was relayed)",
                             target.friendlyName);
                queue_output(logLine);
                target.routeViaDeviceId = newRouteVia;
            }
        }
    }

    uint32_t s_last_routing_recompute_ms = 0;
    constexpr uint32_t kRoutingRecomputeIntervalMs = 5000; // routing doesn't need to react instantly - a node's default 10s heartbeat interval already bounds how fresh the underlying RSSI data ever is anyway

    // Debug/test-only escape hatch (force_route JSON command below) - lets a
    // relay path be proven out on real hardware even when live RSSI sits
    // right at kRelayImprovementThresholdDb's boundary rather than clearly
    // past it. While true, recompute_routing_if_due() skips entirely so the
    // forced routeViaDeviceId isn't immediately overwritten by the very
    // next 5s recompute tick. Not exposed in the desktop app UI - only
    // reachable by hand-sending the JSON command.
    bool s_routingOverridden = false;

    void recompute_routing_if_due()
    {
        if (s_routingOverridden)
            return;
        uint32_t now = millis();
        if (now - s_last_routing_recompute_ms < kRoutingRecomputeIntervalMs)
            return;
        s_last_routing_recompute_ms = now;
        recompute_routing();
    }

    // Broadcasts this gateway's own minimal identity, reusing
    // RTSNOW_ANNOUNCE/RTSNOW_DeviceIdentity exactly like any regular node
    // does. Not to make the gateway show up as "a device" in its own
    // known_devices table - on_espnow_recv's own header.sourceID ==
    // local_device_id() check already discards this right back out
    // before that could ever happen, same guard every node's own
    // onEspNowRecv already has for its own broadcasts too. The actual
    // reason: every node's existing passive peer-discovery (notePeer() in
    // rtsnow_node.cpp, already built for node<->node neighbor discovery)
    // learns the gateway's deviceID<->MAC mapping for free from this, the
    // same way it already learns any other device's. That's what lets a
    // relay's existing, symmetric destinationID-based forwarding
    // correctly route a reply addressed back to the gateway (e.g. an
    // OTA-over-ESP-NOW chunk ack - see do_ota_*_send) through itself -
    // without this, no node would ever have a route to "the gateway" at
    // all, since it's otherwise deliberately silent/announce-less.
    uint32_t s_last_gateway_announce_ms = 0;
    constexpr uint32_t kGatewayAnnounceIntervalMs = 10000; // matches rtsnow_node.cpp's own default heartbeatIntervalMs

#if defined(BOARD_GATEWAY_ATOMS3_POE)
    // Forward declarations only - defined with the rest of the Ethernet/
    // WiFi-fallback state further down this file (with
    // gateway_ethernet_begin() and friends); needed here since
    // send_gateway_announce_if_due() reads them and is defined earlier in
    // the file than that section. Mutually exclusive by construction -
    // see setup()'s own comment - never both true at once.
    extern bool s_eth_connected;
    extern bool s_wifi_connected;
    // Forward declaration only too - defined with the rest of the TCP
    // client-array state further down this file; self_ota_begin() below
    // calls this (guarded the same way) since starting a new OTA transfer
    // should always get the gateway's full attention, not compete with
    // whatever else happens to be connected over the same transport.
    void drop_other_tcp_clients();
#endif

    void send_gateway_announce_if_due()
    {
        uint32_t now = millis();
        if (now - s_last_gateway_announce_ms < kGatewayAnnounceIntervalMs)
            return;
        s_last_gateway_announce_ms = now;

        RTSNOW_DeviceIdentity id{};
        id.deviceID = local_device_id();
        strncpy(id.projectName, "RTSNow", sizeof(id.projectName) - 1);
        // Hyphen-free to match EthernetGateway's own naming style (see
        // node_atoms3_poe's main_atom_node.cpp) now that there are two
        // kinds of gateway on the mesh - the desktop app's device table
        // maps both back to a hyphenated display label. Which literal
        // string depends on which gateway hardware this binary was built
        // for (BOARD_GATEWAY_ATOMS3_POE only defined for the
        // m5stack-atoms3_poe_gateway env - see this file's own Ethernet
        // section) - both share this function/file, so the string can't be
        // a single compile-time constant.
#if defined(BOARD_GATEWAY_ATOMS3_POE)
        strncpy(id.deviceTypeName, "EthernetGateway", sizeof(id.deviceTypeName) - 1);
#else
        strncpy(id.deviceTypeName, "UsbGateway", sizeof(id.deviceTypeName) - 1);
#endif
        strncpy(id.friendlyName, "Gateway", sizeof(id.friendlyName) - 1);
        id.firmwareVersionMajor = 1;
#if defined(BOARD_GATEWAY_ATOMS3_POE)
        // This variant genuinely has an IP (Ethernet/W5500) unlike the USB
        // dongle - report it so the desktop app's Known Devices table shows
        // something real instead of a permanent blank, once s_eth_connected
        // - or, lacking that, whatever WiFi fallback landed (see setup()'s
        // own comment; the two are never both true at once).
        if (s_eth_connected)
            id.ipv4Address = static_cast<uint32_t>(Ethernet.localIP());
        else if (s_wifi_connected)
            id.ipv4Address = static_cast<uint32_t>(WiFi.localIP());
        else
            id.ipv4Address = 0;
#else
        id.ipv4Address = 0; // this gateway has no IP of its own in the RTS-NOW sense - it's the ESP-NOW side, not a node with its own Modbus/HTTP surface
#endif
        send_broadcast(RTSNOW_ANNOUNCE, &id, sizeof(id));
    }

    // Populates one device's fields onto a JsonObject - shared by the
    // `device_announced` push event above and the `known_devices` snapshot
    // event below.
    void fill_device_json(JsonObject dev, const KnownDevice &d)
    {
        char macStr[18];
        mac_to_str(d.mac, macStr, sizeof(macStr));
        dev["deviceID"] = d.deviceID;
        dev["projectName"] = d.projectName;
        dev["deviceTypeName"] = d.deviceTypeName;
        dev["friendlyName"] = d.friendlyName;
        dev["mac"] = macStr;
        if (d.ipv4Address != 0)
            dev["ip"] = IPAddress(d.ipv4Address).toString();
        dev["ageMs"] = millis() - d.lastSeenMs;
        char verStr[16];
        snprintf(verStr, sizeof(verStr), "%u.%u.%u", d.firmwareVersionMajor, d.firmwareVersionMinor,
                 d.firmwareVersionPatch);
        dev["firmwareVersion"] = verStr;
        // Two distinct physical links, not two views of the same number -
        // wifiRssi is this node's link to its own Wi-Fi router, espNowRssi
        // is this node's link to this gateway specifically. Either can be
        // RTSNOW_RSSI_UNKNOWN (no reading yet) - omitted from the JSON
        // entirely rather than sent as a misleading number, same
        // "omit, don't fake" convention as ip above.
        if (d.wifiRssi != RTSNOW_RSSI_UNKNOWN)
            dev["wifiRssi"] = d.wifiRssi;
        if (d.espNowRssi != RTSNOW_RSSI_UNKNOWN)
            dev["espNowRssi"] = d.espNowRssi;
        if (d.boardName[0] != '\0')
            dev["boardName"] = d.boardName;
        // Diagnostic-only node<->node visibility (see KnownDevice's own
        // neighborCount comment) - keyed by deviceID since that's what's
        // on the wire; the desktop app already has deviceID<->MAC for
        // every device from this same known_devices snapshot, so it can
        // resolve these itself rather than this also doing MAC lookups
        // in the hot ESP-NOW callback path.
        if (d.neighborCount > 0)
        {
            JsonArray neighbors = dev["neighbors"].to<JsonArray>();
            for (uint8_t i = 0; i < d.neighborCount; i++)
            {
                JsonObject neighbor = neighbors.add<JsonObject>();
                neighbor["deviceId"] = d.neighbors[i].deviceID;
                neighbor["rssi"] = d.neighbors[i].rssi;
            }
        }
        // 0 (the field's own default) means "direct" - omitted here for
        // the same "omit, don't send a misleading value" reason as every
        // other optional field above, rather than sending a literal 0
        // that would need its own special-case meaning on the receiving
        // end. See recompute_routing() for how this actually gets decided.
        if (d.routeViaDeviceId != 0)
            dev["routeViaDeviceId"] = d.routeViaDeviceId;
    }

    void send_known_devices_event(const JsonDocument *inDoc)
    {
        JsonDocument doc;
        doc["event"] = "known_devices";
        JsonArray devices = doc["devices"].to<JsonArray>();
        for (int i = 0; i < s_known_device_count; i++)
            fill_device_json(devices.add<JsonObject>(), s_known_devices[i]);
        if (inDoc != nullptr)
            echo_id(doc, *inDoc);
        emit_json(doc);
    }

    void print_known_devices()
    {
        uint32_t now = millis();
        Serial.println();
        Serial.println("=========================================== Known Devices ===========================================");
        Serial.printf("%-14s %-14s %-24s %-15s %-17s %s\n", "Project", "Type", "Name", "IP Address", "MAC Address", "Last Seen");
        Serial.println("-------------------------------------------------------------------------------------------------------");
        if (s_known_device_count == 0)
        {
            Serial.println("(none yet - waiting for RTSNOW_ANNOUNCE/RTSNOW_HEARTBEAT traffic...)");
        }
        else
        {
            for (int i = 0; i < s_known_device_count; i++)
            {
                const KnownDevice &d = s_known_devices[i];
                char ipStr[16] = "-";
                if (d.ipv4Address != 0)
                {
                    IPAddress ip(d.ipv4Address);
                    ip.toString().toCharArray(ipStr, sizeof(ipStr));
                }
                char macStr[18];
                mac_to_str(d.mac, macStr, sizeof(macStr));
                Serial.printf("%-14s %-14s %-24s %-15s %-17s %us ago\n",
                              d.projectName, d.deviceTypeName, d.friendlyName, ipStr, macStr,
                              (now - d.lastSeenMs) / 1000);
            }
        }
        Serial.println("=======================================================================================================");
    }

    // ---- Pending provisioning requests ----

    int find_pending_by_mac(const uint8_t mac[6])
    {
        for (int i = 0; i < kMaxPendingRequests; i++)
            if (s_pending[i].inUse && memcmp(s_pending[i].mac, mac, 6) == 0)
                return i;
        return -1;
    }

    void print_pending()
    {
        uint32_t now = millis();
        Serial.println();
        Serial.println("=================================== Pending Provisioning Requests ===================================");
        int count = 0;
        for (int i = 0; i < kMaxPendingRequests; i++)
        {
            if (!s_pending[i].inUse)
                continue;
            count++;
            char macStr[18];
            mac_to_str(s_pending[i].mac, macStr, sizeof(macStr));
            Serial.printf("  %-17s project=\"%s\" type=\"%s\" name=\"%s\" (%us ago) - `provision %s <ssid> <password>` or `reject %s`\n",
                          macStr, s_pending[i].projectName, s_pending[i].deviceTypeName, s_pending[i].friendlyName,
                          (now - s_pending[i].receivedMs) / 1000, macStr, macStr);
        }
        if (count == 0)
            Serial.println("(none)");
        Serial.println("=======================================================================================================");
    }

    void fill_pending_json(JsonObject req, const PendingRequest &p)
    {
        char macStr[18];
        mac_to_str(p.mac, macStr, sizeof(macStr));
        req["mac"] = macStr;
        req["deviceID"] = p.deviceID;
        req["projectName"] = p.projectName;
        req["deviceTypeName"] = p.deviceTypeName;
        req["friendlyName"] = p.friendlyName;
        req["ageMs"] = millis() - p.receivedMs;
    }

    void send_pending_requests_event(const JsonDocument *inDoc)
    {
        JsonDocument doc;
        doc["event"] = "pending_requests";
        JsonArray requests = doc["requests"].to<JsonArray>();
        for (int i = 0; i < kMaxPendingRequests; i++)
            if (s_pending[i].inUse)
                fill_pending_json(requests.add<JsonObject>(), s_pending[i]);
        if (inDoc != nullptr)
            echo_id(doc, *inDoc);
        emit_json(doc);
    }

    // Gateway identity - emitted once unprompted at boot, and again on
    // demand via the `hello` command (see handle_json_line) since a client
    // that connects after boot would otherwise never see one - this was a
    // real gap: desktop-app's Dashboard page had no way to learn the
    // gateway's deviceID/channel/protocolVersion unless it happened to be
    // connected at the exact moment the gateway itself booted.
    void send_hello_event(const JsonDocument *inDoc)
    {
        JsonDocument doc;
        doc["event"] = "hello";
        doc["protocolVersion"] = 1;
        doc["deviceID"] = local_device_id();
        doc["channel"] = s_channel;
        if (inDoc != nullptr)
            echo_id(doc, *inDoc);
        emit_json(doc);
    }

    void on_provision_request(const uint8_t mac[6], const uint8_t *payload, int payloadLen)
    {
        if (payloadLen < static_cast<int>(sizeof(RTSNOW_ProvisionRequest)))
            return;
        RTSNOW_ProvisionRequest req;
        memcpy(&req, payload, sizeof(req));
        req.identity.projectName[sizeof(req.identity.projectName) - 1] = '\0';
        req.identity.deviceTypeName[sizeof(req.identity.deviceTypeName) - 1] = '\0';
        req.identity.friendlyName[sizeof(req.identity.friendlyName) - 1] = '\0';

        int slot = find_pending_by_mac(mac);
        bool isNew = slot < 0;
        if (isNew)
        {
            for (int i = 0; i < kMaxPendingRequests; i++)
            {
                if (!s_pending[i].inUse)
                {
                    slot = i;
                    break;
                }
            }
            if (slot < 0)
            {
                Serial.println("Pending-request table full - ignoring new RTSNOW_PROVISION_REQUEST (reject or provision an existing one first)");
                return;
            }
        }

        memcpy(s_pending[slot].mac, mac, 6);
        s_pending[slot].deviceID = req.identity.deviceID;
        strncpy(s_pending[slot].projectName, req.identity.projectName, sizeof(s_pending[slot].projectName) - 1);
        s_pending[slot].projectName[sizeof(s_pending[slot].projectName) - 1] = '\0';
        strncpy(s_pending[slot].deviceTypeName, req.identity.deviceTypeName, sizeof(s_pending[slot].deviceTypeName) - 1);
        s_pending[slot].deviceTypeName[sizeof(s_pending[slot].deviceTypeName) - 1] = '\0';
        strncpy(s_pending[slot].friendlyName, req.identity.friendlyName, sizeof(s_pending[slot].friendlyName) - 1);
        s_pending[slot].friendlyName[sizeof(s_pending[slot].friendlyName) - 1] = '\0';
        s_pending[slot].nonce = req.nonce;
        s_pending[slot].receivedMs = millis();
        s_pending[slot].inUse = true;

        ensure_peer(mac);
        RTSNOW_ProvisionNonce hold{req.nonce};
        send_to(mac, RTSNOW_PROVISION_HOLD, &hold, sizeof(hold));

        if (isNew)
        {
            char macStr[18];
            mac_to_str(mac, macStr, sizeof(macStr));
            char logLine[256];
            snprintf(logLine, sizeof(logLine),
                     "\nPending provisioning request from %s - project=\"%s\" type=\"%s\" name=\"%s\"\n"
                     "  -> `provision %s <ssid> <password>` to accept, `reject %s` to decline",
                     macStr, s_pending[slot].projectName, s_pending[slot].deviceTypeName, s_pending[slot].friendlyName,
                     macStr, macStr);
            queue_output(logLine); // called from the ESP-NOW callback - see "Deferred Serial output"

            JsonDocument doc;
            doc["event"] = "provision_request";
            fill_pending_json(doc["request"].to<JsonObject>(), s_pending[slot]);
            emit_json(doc);
        }
    }

    void expire_pending()
    {
        uint32_t now = millis();
        for (int i = 0; i < kMaxPendingRequests; i++)
        {
            if (s_pending[i].inUse && now - s_pending[i].receivedMs >= kPendingTimeoutMs)
            {
                char macStr[18];
                mac_to_str(s_pending[i].mac, macStr, sizeof(macStr));
                Serial.printf("Pending request from %s expired (no longer sweeping/refreshing)\n", macStr);
                s_pending[i].inUse = false;

                JsonDocument doc;
                doc["event"] = "provision_expired";
                doc["mac"] = macStr;
                emit_json(doc);
            }
        }
    }

    // ---- Post-credentials confirmation tracking ----
    // See PROTOCOL.md's "Why broadcast, not ACK" - a node's own
    // RTSNOW_ANNOUNCE after joining Wi-Fi is the trusted confirmation
    // signal, not an ACK to RTSNOW_PROVISION_CREDENTIALS (this protocol
    // doesn't even define one) - carried over from a real hardware lesson
    // learned the hard way in CYD-4.3-FN-Tester's ShackMate protocol.

    void track_awaiting_confirm(uint32_t deviceID, const uint8_t mac[6], const char *ssid)
    {
        int slot = -1;
        for (int i = 0; i < kMaxAwaitingConfirm; i++)
        {
            if (!s_awaiting_confirm[i].inUse)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            slot = 0; // table full - overwrite the oldest slot rather than dropping tracking entirely
        s_awaiting_confirm[slot].deviceID = deviceID;
        memcpy(s_awaiting_confirm[slot].mac, mac, 6);
        strncpy(s_awaiting_confirm[slot].ssid, ssid, sizeof(s_awaiting_confirm[slot].ssid) - 1);
        s_awaiting_confirm[slot].ssid[sizeof(s_awaiting_confirm[slot].ssid) - 1] = '\0';
        s_awaiting_confirm[slot].sentMs = millis();
        s_awaiting_confirm[slot].inUse = true;
    }

    void check_awaiting_confirm(uint32_t deviceID)
    {
        for (int i = 0; i < kMaxAwaitingConfirm; i++)
        {
            if (s_awaiting_confirm[i].inUse && s_awaiting_confirm[i].deviceID == deviceID)
            {
                char logLine[128];
                snprintf(logLine, sizeof(logLine), "Confirmed: device 0x%08X announced on the network after provisioning (SSID \"%s\")",
                         deviceID, s_awaiting_confirm[i].ssid);
                queue_output(logLine); // called from the ESP-NOW callback - see "Deferred Serial output"

                char macStr[18];
                mac_to_str(s_awaiting_confirm[i].mac, macStr, sizeof(macStr));
                JsonDocument doc;
                doc["event"] = "provision_confirmed";
                doc["deviceID"] = deviceID;
                doc["mac"] = macStr;
                doc["ssid"] = s_awaiting_confirm[i].ssid;
                emit_json(doc);

                s_awaiting_confirm[i].inUse = false;
            }
        }
    }

    void expire_awaiting_confirm()
    {
        uint32_t now = millis();
        for (int i = 0; i < kMaxAwaitingConfirm; i++)
        {
            if (s_awaiting_confirm[i].inUse && now - s_awaiting_confirm[i].sentMs >= kConfirmTimeoutMs)
            {
                char macStr[18];
                mac_to_str(s_awaiting_confirm[i].mac, macStr, sizeof(macStr));
                Serial.printf("No confirmation from %s within %us of sending credentials - it may not have received them, "
                              "or joined but hasn't been heard from yet\n",
                              macStr, kConfirmTimeoutMs / 1000);

                JsonDocument doc;
                doc["event"] = "provision_confirm_timeout";
                doc["mac"] = macStr;
                doc["ssid"] = s_awaiting_confirm[i].ssid;
                emit_json(doc);

                s_awaiting_confirm[i].inUse = false;
            }
        }
    }

    // ---- Command cores (shared by the text and JSON command interfaces) ----
    // Each returns nullptr on success, or a static error string on failure -
    // the text interface prints it with a "<cmd>: " prefix, the JSON
    // interface puts it verbatim in an `{"event":"ack",...,"error":...}`.

    const char *do_provision(const uint8_t mac[6], const char *ssid, const char *password)
    {
        int slot = find_pending_by_mac(mac);
        if (slot < 0)
            return "no pending request from that MAC - see `pending`";
        if (strlen(ssid) >= sizeof(RTSNOW_WifiCredentials::ssid) || strlen(password) >= sizeof(RTSNOW_WifiCredentials::password))
            return "SSID/password too long";

        ensure_peer(mac);
        RTSNOW_WifiCredentials creds{};
        creds.nonce = s_pending[slot].nonce;
        strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
        strncpy(creds.password, password, sizeof(creds.password) - 1);
        send_to(mac, RTSNOW_PROVISION_CREDENTIALS, &creds, sizeof(creds));

        track_awaiting_confirm(s_pending[slot].deviceID, mac, ssid);
        s_pending[slot].inUse = false;
        return nullptr;
    }

    const char *do_reject(const uint8_t mac[6])
    {
        int slot = find_pending_by_mac(mac);
        if (slot < 0)
            return "no pending request from that MAC - see `pending`";
        ensure_peer(mac);
        RTSNOW_ProvisionNonce rej{s_pending[slot].nonce};
        send_to(mac, RTSNOW_PROVISION_REJECTED, &rej, sizeof(rej));
        s_pending[slot].inUse = false;
        return nullptr;
    }

    // Removes an already-known device from s_known_devices entirely (not
    // to be confused with do_reject() above, which only ever touches
    // s_pending[] - a device asking to JOIN, not one already provisioned).
    // Purely local bookkeeping: doesn't tell the device anything (there's
    // no "you've been forgotten" message in this protocol, and no need
    // for one - it'll just reappear here on its next announce/heartbeat
    // if it's still alive). Swap-with-last removal since display order
    // was never guaranteed - the desktop app re-renders the whole list
    // fresh from `list` every time regardless.
    const char *do_forget(const uint8_t mac[6])
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        s_known_devices[idx] = s_known_devices[s_known_device_count - 1];
        s_known_device_count--;
        return nullptr;
    }

    // Sent kRemoteControlResendCount times rather than once, a beat
    // (kRemoteControlResendGapMs) apart - real-hardware testing found a
    // meaningfully high single-shot loss rate on this link (a single
    // reboot, register request, or setting going completely unanswered was
    // common, not rare), and there's no ack/retry anywhere in this protocol
    // layer to fall back on. Harmless if the node is duplicate-tolerant,
    // which is true of all three uses below: RTSNOW_REBOOT is a single
    // boolean flag, RTSNOW_REQUEST_REGISTERS's each resend just gets its
    // own independent reply, and RTSNOW_SET_SETTING's onRemoteSetting
    // handlers (equipmentType/model/spiAddress/spiBaudRate/friendlyName)
    // are idempotent re-applying the same value. Same "cheap insurance"
    // tradeoff rtsnow_node.cpp's own initial RTSNOW_ANNOUNCE already makes
    // (sent twice on boot).
    constexpr uint8_t kRemoteControlResendCount = 3;
    constexpr uint32_t kRemoteControlResendGapMs = 40;

    // Resolves the actual physical MAC to unicast to, and the logical
    // destinationID to put on the frame, for sending a command to known
    // device `idx` - either directly (routeViaDeviceId == 0, by far the
    // common case) or through its chosen relay (see recompute_routing()).
    // Falls back to direct if the chosen relay has since dropped out of
    // known_devices entirely (a stale decision that recompute_routing_if_due()
    // will correct on its own within kRoutingRecomputeIntervalMs anyway -
    // this is just extra safety against ever unicasting to a MAC this
    // gateway no longer actually has on file).
    struct SendTarget
    {
        const uint8_t *mac;
        uint32_t destinationID;
    };
    SendTarget resolve_send_target(int idx)
    {
        const KnownDevice &target = s_known_devices[idx];
        if (target.routeViaDeviceId != 0)
        {
            int relayIdx = find_known_device_by_device_id(target.routeViaDeviceId);
            if (relayIdx >= 0)
                return SendTarget{s_known_devices[relayIdx].mac, target.deviceID};
        }
        return SendTarget{target.mac, 0xFFFFFFFF};
    }

    // Sends an already-built setting payload (the text and JSON interfaces
    // each build one their own way - see cmd_setting and
    // handle_setting_command). Originally sent only once, unlike
    // do_reboot_send/do_request_registers_send below - found via real
    // hardware testing to be the odd one out still hitting the same
    // single-shot loss rate documented above (a renamed node's
    // friendlyName silently staying unchanged, no RTSNOW_SETTING_ACK ever
    // arriving) - the desktop app's pending-rename timeout tolerates more
    // than one ack for the same request fine, it just uses the first one.
    const char *do_setting_send(const uint8_t mac[6], const RTSNOW_SettingPayload &payload)
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        SendTarget dest = resolve_send_target(idx);
        ensure_peer(dest.mac);
        for (uint8_t i = 0; i < kRemoteControlResendCount; i++)
        {
            send_to(dest.mac, RTSNOW_SET_SETTING, &payload, sizeof(payload), dest.destinationID);
            if (i + 1 < kRemoteControlResendCount)
                delay(kRemoteControlResendGapMs);
        }
        return nullptr;
    }

    // Both reboot and register-request are fire-and-forget from the
    // gateway's point of view, same caveat as do_setting_send above: a
    // successful send here only means the ESP-NOW packet went out, not that
    // the node received it. See PROTOCOL.md's "Remote control" section for
    // how each side's real confirmation actually arrives (RTSNOW_ANNOUNCE
    // for reboot, RTSNOW_REGISTER_VALUES for a register request).
    const char *do_reboot_send(const uint8_t mac[6])
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        SendTarget dest = resolve_send_target(idx);
        ensure_peer(dest.mac);
        for (uint8_t i = 0; i < kRemoteControlResendCount; i++)
        {
            send_to(dest.mac, RTSNOW_REBOOT, nullptr, 0, dest.destinationID);
            if (i + 1 < kRemoteControlResendCount)
                delay(kRemoteControlResendGapMs);
        }
        return nullptr;
    }

    const char *do_request_registers_send(const uint8_t mac[6])
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        SendTarget dest = resolve_send_target(idx);
        ensure_peer(dest.mac);
        for (uint8_t i = 0; i < kRemoteControlResendCount; i++)
        {
            send_to(dest.mac, RTSNOW_REQUEST_REGISTERS, nullptr, 0, dest.destinationID);
            if (i + 1 < kRemoteControlResendCount)
                delay(kRemoteControlResendGapMs);
        }
        return nullptr;
    }

    // ---- Firmware update - RECEIVING side (this gateway being flashed,
    // not pushing to a node) ----
    //
    // Every RTS-NOW device should be reachable/updatable over the same
    // protocol, gateways included - not just nodes. Two independent entry
    // points share this same core (only one physical OTA partition
    // exists, so both must agree on the same in-progress-transfer state
    // or risk corrupting each other's writes):
    //   - ESP-NOW (below): ported directly from rtsnow_node.cpp's
    //     onEspNowRecv/rtsnowNodeLoop, the exact mechanism already proven
    //     live (including through a relay) for regular nodes. Any device
    //     already capable of pushing ESP-NOW OTA to a node (the existing
    //     do_ota_*_send functions below, driven by the desktop app's
    //     EspNowOtaTransfer) can push it to this gateway's MAC exactly
    //     the same way - no new desktop-app code needed for that path.
    //   - TCP (handle_ota_*_command, near handle_json_line): same
    //     Update.begin/write/end core, driven directly over the already-
    //     connected JSON-lines socket instead of ESP-NOW - useful when
    //     this gateway IS the connection a desktop app already has open,
    //     rather than something being reached through the mesh.
    bool s_selfOtaActive = false;
    uint32_t s_selfOtaTotalChunks = 0;
    uint32_t s_selfOtaNextExpectedIndex = 0;

    // Common core, called from both entry points. outError points at a
    // caller-owned buffer (RTSNOW_OtaAck::message for ESP-NOW, a local
    // buffer for TCP) - left untouched on success.
    bool self_ota_begin(uint32_t totalSize, uint32_t totalChunks, const char *md5, char *outError, size_t outErrorLen)
    {
        if (s_selfOtaActive)
            Update.abort();  // a fresh START while one was already active - trust the new one, not stale state
        bool began = Update.begin(totalSize);
        if (!began)
        {
            snprintf(outError, outErrorLen, "Update.begin failed");
            return false;
        }
        if (md5 != nullptr && strlen(md5) == 32)  // only trust it if it actually looks like a real MD5 hex string
            Update.setMD5(md5);
        s_selfOtaActive = true;
        s_selfOtaTotalChunks = totalChunks;
        s_selfOtaNextExpectedIndex = 0;
#if defined(BOARD_GATEWAY_ATOMS3_POE)
        // A fresh transfer just started - drop every other TCP connection
        // on whichever transport is active (keeping only the one that
        // issued this exact ota_start, if it came in over TCP - see
        // drop_other_tcp_clients()'s own comment) so nothing else
        // competes for CPU/socket resources while this gateway writes its
        // own flash. If this call came from the ESP-NOW entry point
        // instead (no TCP client involved at all), every TCP connection
        // gets dropped - there's no "self" to preserve in that case.
        drop_other_tcp_clients();
#endif
        Serial.printf("Self-OTA start accepted (size=%u, %u chunks)\n", totalSize, totalChunks);
        return true;
    }

    bool self_ota_write_chunk(uint32_t index, const uint8_t *data, uint16_t length)
    {
        if (!s_selfOtaActive || index != s_selfOtaNextExpectedIndex)
            return false;
        size_t written = Update.write(const_cast<uint8_t *>(data), length);
        bool ok = written == length;
        if (ok)
            s_selfOtaNextExpectedIndex++;
        return ok;
    }

    bool self_ota_end(char *outError, size_t outErrorLen)
    {
        if (!s_selfOtaActive || s_selfOtaNextExpectedIndex != s_selfOtaTotalChunks)
        {
            snprintf(outError, outErrorLen, "incomplete transfer");
            return false;
        }
        bool success = Update.end(true);
        if (!success)
            snprintf(outError, outErrorLen, "Update.end failed: %s", Update.errorString());
        else
        {
            Serial.println("Self-OTA complete - restarting");
            delay(100);  // let the ack/response actually get out before the restart
            ESP.restart();
            // ESP.restart() does not return - nothing below this point on
            // the success path.
        }
        return success;
    }

    void self_ota_abort()
    {
        if (s_selfOtaActive)
        {
            Update.abort();
            s_selfOtaActive = false;
            Serial.println("Self-OTA aborted by sender");
        }
    }

    // ---- ESP-NOW entry point - deferred processing ----
    //
    // on_espnow_recv() below just flags + copies the small fixed-size
    // payload for each message; service_self_ota() (called from loop())
    // does the actual self_ota_*() calls, keeping flash access out of the
    // ESP-NOW receive callback's task context - same deferred pattern
    // rtsnow_node.cpp uses for a node's own receiving side.
    bool s_selfOtaStartPending = false;
    RTSNOW_OtaStart s_pendingSelfOtaStart;
    uint8_t s_pendingSelfOtaStartMac[6];
    uint32_t s_pendingSelfOtaStartRequesterId = 0xFFFFFFFF;

    bool s_selfOtaChunkPending = false;
    RTSNOW_OtaChunk s_pendingSelfOtaChunk;
    uint8_t s_pendingSelfOtaChunkMac[6];
    uint32_t s_pendingSelfOtaChunkRequesterId = 0xFFFFFFFF;

    bool s_selfOtaEndPending = false;
    uint8_t s_pendingSelfOtaEndMac[6];
    uint32_t s_pendingSelfOtaEndRequesterId = 0xFFFFFFFF;

    bool s_selfOtaAbortPending = false;

    void service_self_ota()
    {
        if (s_selfOtaStartPending)
        {
            s_selfOtaStartPending = false;
            ensure_peer(s_pendingSelfOtaStartMac);
            char md5[33];
            memcpy(md5, s_pendingSelfOtaStart.md5, sizeof(md5));
            md5[32] = '\0';
            RTSNOW_OtaAck ack{};
            bool began = self_ota_begin(s_pendingSelfOtaStart.totalSize, s_pendingSelfOtaStart.totalChunks, md5,
                                        ack.message, sizeof(ack.message));
            ack.ok = began ? 1 : 0;
            send_to(s_pendingSelfOtaStartMac, RTSNOW_OTA_START_ACK, &ack, sizeof(ack), s_pendingSelfOtaStartRequesterId);
        }

        if (s_selfOtaChunkPending)
        {
            s_selfOtaChunkPending = false;
            RTSNOW_OtaChunkAck ack{};
            ack.index = s_pendingSelfOtaChunk.index;
            ack.ok = self_ota_write_chunk(s_pendingSelfOtaChunk.index, s_pendingSelfOtaChunk.data,
                                          s_pendingSelfOtaChunk.length)
                         ? 1
                         : 0;
            send_to(s_pendingSelfOtaChunkMac, RTSNOW_OTA_CHUNK_ACK, &ack, sizeof(ack), s_pendingSelfOtaChunkRequesterId);
        }

        if (s_selfOtaEndPending)
        {
            s_selfOtaEndPending = false;
            RTSNOW_OtaAck ack{};
            bool success = self_ota_end(ack.message, sizeof(ack.message));
            ack.ok = success ? 1 : 0;
            // self_ota_end() already restarted the chip on success - this
            // send only actually goes out on the failure path.
            send_to(s_pendingSelfOtaEndMac, RTSNOW_OTA_END_ACK, &ack, sizeof(ack), s_pendingSelfOtaEndRequesterId);
        }

        if (s_selfOtaAbortPending)
        {
            s_selfOtaAbortPending = false;
            self_ota_abort();
        }
    }

    // ---- Firmware update over ESP-NOW (see RTSNOW_OTA_START's own
    // comment in rtsnow_protocol.h) ----
    //
    // Single-shot sends, deliberately NOT resent here like
    // do_setting_send/do_reboot_send above - the desktop app drives this
    // entire transfer command-by-command over the same serial JSON
    // channel and already needs its own per-chunk ack-wait-and-retry loop
    // regardless (it must know a chunk succeeded before sending the next
    // one - strict stop-and-wait, see RTSNOW_OtaChunk's own comment), so
    // resending here too would just be a second, redundant retry layer
    // the desktop app can't see or control the timing of.
    const char *do_ota_start_send(const uint8_t mac[6], const RTSNOW_OtaStart &payload)
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        SendTarget dest = resolve_send_target(idx);
        ensure_peer(dest.mac);
        send_to(dest.mac, RTSNOW_OTA_START, &payload, sizeof(payload), dest.destinationID);
        return nullptr;
    }

    const char *do_ota_chunk_send(const uint8_t mac[6], const RTSNOW_OtaChunk &payload)
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        SendTarget dest = resolve_send_target(idx);
        ensure_peer(dest.mac);
        send_to(dest.mac, RTSNOW_OTA_CHUNK, &payload, sizeof(payload), dest.destinationID);
        return nullptr;
    }

    const char *do_ota_end_send(const uint8_t mac[6])
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        SendTarget dest = resolve_send_target(idx);
        ensure_peer(dest.mac);
        send_to(dest.mac, RTSNOW_OTA_END, nullptr, 0, dest.destinationID);
        return nullptr;
    }

    const char *do_ota_abort_send(const uint8_t mac[6])
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        SendTarget dest = resolve_send_target(idx);
        ensure_peer(dest.mac);
        send_to(dest.mac, RTSNOW_OTA_ABORT, nullptr, 0, dest.destinationID);
        return nullptr;
    }

    // Decodes a lowercase-or-uppercase hex string (must be an even number
    // of characters) into raw bytes - used for RTSNOW_OtaChunk.data, sent
    // over the JSON serial channel as hex rather than base64 since
    // ArduinoJson has no built-in binary encoding and hex has no
    // padding/alphabet edge cases to get wrong.
    bool hex_decode(const char *hex, uint8_t *out, size_t maxOutLen, size_t &outLen)
    {
        size_t hexLen = strlen(hex);
        if (hexLen % 2 != 0)
            return false;
        size_t n = hexLen / 2;
        if (n > maxOutLen)
            return false;
        for (size_t i = 0; i < n; i++)
        {
            unsigned int byte;
            if (sscanf(hex + i * 2, "%2x", &byte) != 1)
                return false;
            out[i] = static_cast<uint8_t>(byte);
        }
        outLen = n;
        return true;
    }

    void save_channel(uint8_t channel)
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/false);
        prefs.putUChar(kChannelKey, channel);
        prefs.end();
    }

    uint8_t load_channel()
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/true);
        uint8_t channel = prefs.getUChar(kChannelKey, kDefaultChannel);
        prefs.end();
        if (channel < RTSNOW_PROVISIONING_CHANNEL_MIN || channel > RTSNOW_PROVISIONING_CHANNEL_MAX)
            channel = kDefaultChannel;
        return channel;
    }

    void apply_channel(uint8_t channel)
    {
        s_channel = channel;
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }

    // Applies, persists, and announces (via `channel_changed`) a channel
    // change - callers have already range-checked `channel`.
    void do_channel(uint8_t channel)
    {
        apply_channel(channel);
        save_channel(channel);

        JsonDocument doc;
        doc["event"] = "channel_changed";
        doc["channel"] = channel;
        emit_json(doc);
    }

#if defined(BOARD_GATEWAY_ATOMS3_POE)
    // Defined later in this file (both parse a complete line the same way
    // regardless of which transport - USB serial or this TCP server - it
    // arrived on); forward-declared here since service_tcp_client() below
    // needs to call them well before their own definitions.
    void handle_command(char *line);
    void handle_json_line(char *line);

    // ---- Ethernet (W5500/Atomic PoE Base) + TCP JSON-lines server ----
    //
    // Reuses RTSNow-SPI-CCP's own NetworkManager.cpp bring-up sequence
    // (same board pairing, pins confirmed there against two independent
    // real-hardware sources), including the one real fix that took an
    // entire debugging session to find: EthernetClass::init(sspin) only
    // sets the CS pin - it does NOT probe the chip. hardwareStatus() only
    // ever returns a cached W5100.getChip() value, populated ONLY by
    // calling W5100.init() (a different, same-named function, W5100Class
    // not EthernetClass) - skipping that call means every hardwareStatus()
    // check reads stale/zeroed data, forever, even with the chip working
    // perfectly. No WiFi-AP fallback here at all (see gateway_ethernet_
    // begin()'s own call site in setup() for why not).
    constexpr gpio_num_t kEthSck = GPIO_NUM_5;
    constexpr gpio_num_t kEthMiso = GPIO_NUM_7;
    constexpr gpio_num_t kEthMosi = GPIO_NUM_8;
    constexpr gpio_num_t kEthCs = GPIO_NUM_6;
    bool s_eth_connected = false;
    bool s_wifi_connected = false;  // WiFi fallback only - set once at boot, see setup()

    bool gateway_ethernet_begin(uint32_t linkTimeoutMs = 15000)
    {
        uint8_t mac[6];
        WiFi.macAddress(mac);  // WiFi.mode(WIFI_STA) already ran earlier in setup()
        SPI.begin(kEthSck, kEthMiso, kEthMosi, -1);
        Ethernet.init(kEthCs);
        W5100.init();  // the actual detection - do not skip this, see comment above
        if (Ethernet.hardwareStatus() != EthernetW5500)
        {
            Serial.println("Ethernet: W5500 not detected - check PoE Base seating.");
            return false;
        }
        uint32_t start = millis();
        while (Ethernet.linkStatus() != LinkON && millis() - start < linkTimeoutMs)
            delay(100);
        if (Ethernet.linkStatus() != LinkON)
        {
            Serial.println("Ethernet: W5500 detected, but no link - check cable/switch.");
            return false;
        }
        if (Ethernet.begin(mac) == 0)
        {
            Serial.println("Ethernet: DHCP failed.");
            return false;
        }
        Serial.printf("Ethernet: connected, IP=%s\n", Ethernet.localIP().toString().c_str());
        return true;
    }

    // ---- mDNS - "RTSNow.local" resolves to whichever IP is currently
    // active, over either transport ----
    //
    // WiFi fallback uses the standard ESPmDNS library (ESP-IDF's mdns
    // component, bound to the ESP32's own native WiFi interface) - see
    // setup()'s own MDNS.begin() call. That library can NOT be used for
    // Ethernet: the W5500 runs its own hardware TCP/IP stack, entirely
    // invisible to ESP-IDF's own network stack (the same reason
    // ArduinoOTA/WiFiUDP can't see it either, per this file's own
    // comments elsewhere) - there is no esp_netif handle to bind mdns to.
    //
    // So this hand-rolls just enough of RFC 6762 to answer one specific
    // question over Ethernet: an A-record query for kMdnsHostname+".local"
    // gets this gateway's Ethernet IP back, multicast to 224.0.0.251:5353
    // (the same group every mDNS listener - avahi, macOS's mDNSResponder,
    // `dns-sd`, `ping foo.local` - already joined to send the query in
    // the first place). Nothing else in the spec (PTR/SRV/TXT records,
    // service discovery, conflict resolution, compressed names anywhere
    // but our own reply) is implemented - this exists purely so
    // `ping/ssh rtsnow.local` resolves from a command line without
    // needing to know the current DHCP-assigned IP.
    constexpr uint16_t kMdnsPort = 5353;
    const IPAddress kMdnsMulticastAddr(224, 0, 0, 251);
    constexpr const char *kMdnsHostname = "RTSNow";  // -> "rtsnow.local" (mDNS names are case-insensitive)
    EthernetUDP s_mdns_eth_udp;
    bool s_mdns_eth_started = false;

    // Decodes one DNS name starting at buf[offset] into outName (plain,
    // lowercased, dot-joined, e.g. "rtsnow.local"). Refuses (returns
    // false) on a compression pointer (a length byte >= 0xC0) - a real
    // query's own question name never uses one (there's nothing earlier
    // in the packet to point at), so anything that looks like one here
    // is either malformed or not a plain question we need to answer.
    // Also bails out on any length that would read past bufLen
    // (truncated/malformed packet) rather than trusting untrusted network
    // input. outConsumedLen is the number of bytes read on success,
    // including the terminating 0x00, so the caller can correctly skip
    // past QTYPE/QCLASS next.
    bool mdns_decode_name(const uint8_t *buf, size_t bufLen, size_t offset, char *outName, size_t outNameMax,
                          size_t &outConsumedLen)
    {
        size_t pos = offset;
        size_t nameLen = 0;
        outName[0] = '\0';
        while (true)
        {
            if (pos >= bufLen)
                return false;
            uint8_t len = buf[pos];
            if (len == 0)
            {
                pos++;
                break;
            }
            if (len >= 0xC0)
                return false;
            pos++;
            if (pos + len > bufLen)
                return false;
            if (nameLen > 0)
            {
                if (nameLen + 1 >= outNameMax)
                    return false;
                outName[nameLen++] = '.';
            }
            for (uint8_t i = 0; i < len; i++)
            {
                if (nameLen + 1 >= outNameMax)
                    return false;
                outName[nameLen++] = static_cast<char>(tolower(buf[pos + i]));
            }
            outName[nameLen] = '\0';
            pos += len;
        }
        outConsumedLen = pos - offset;
        return true;
    }

    void begin_mdns_ethernet()
    {
        if (s_mdns_eth_udp.beginMulticast(kMdnsMulticastAddr, kMdnsPort))
        {
            s_mdns_eth_started = true;
            Serial.printf("mDNS (Ethernet): responding to %s.local queries\n", kMdnsHostname);
        }
        else
        {
            Serial.println("mDNS (Ethernet): beginMulticast failed - no free socket?");
        }
    }

    void poll_mdns_ethernet()
    {
        if (!s_mdns_eth_started)
            return;
        int packetSize = s_mdns_eth_udp.parsePacket();
        if (packetSize <= 0)
            return;
        static uint8_t buf[512];
        if (static_cast<size_t>(packetSize) > sizeof(buf))
        {
            s_mdns_eth_udp.flush();
            return;
        }
        int readLen = s_mdns_eth_udp.read(buf, sizeof(buf));
        if (readLen < 12)
            return;  // shorter than a DNS header - malformed

        uint16_t flags = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
        uint16_t qdcount = (static_cast<uint16_t>(buf[4]) << 8) | buf[5];
        if ((flags & 0x8000) != 0)
            return;  // QR=1 - a response from someone else on this multicast group, not a query
        if (qdcount == 0)
            return;

        size_t nameStart = 12;
        char name[64];
        size_t consumed = 0;
        if (!mdns_decode_name(buf, static_cast<size_t>(readLen), nameStart, name, sizeof(name), consumed))
            return;
        size_t offset = nameStart + consumed;
        if (offset + 4 > static_cast<size_t>(readLen))
            return;
        uint16_t qtype = (static_cast<uint16_t>(buf[offset]) << 8) | buf[offset + 1];
        // QCLASS at buf[offset+2..3] (top bit is mDNS's "unicast response
        // requested" flag) - ignored, this always replies multicast
        // regardless, which every mDNS listener already accepts.

        char expectedName[32];
        snprintf(expectedName, sizeof(expectedName), "%s.local", kMdnsHostname);
        for (char *p = expectedName; *p != '\0'; p++)
            *p = static_cast<char>(tolower(*p));
        if (strcmp(name, expectedName) != 0)
            return;
        if (qtype != 1 && qtype != 255)  // A, or ANY
            return;

        uint8_t resp[128];
        size_t rlen = 0;
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x00;  // ID = 0 (conventional for unsolicited/multicast mDNS responses)
        resp[rlen++] = 0x84;
        resp[rlen++] = 0x00;  // flags: QR=1 (response), AA=1 (authoritative)
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x00;  // QDCOUNT = 0
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x01;  // ANCOUNT = 1
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x00;  // NSCOUNT = 0
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x00;  // ARCOUNT = 0
        memcpy(resp + rlen, buf + nameStart, consumed);  // reuse the query's own encoded name verbatim
        rlen += consumed;
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x01;  // TYPE = A
        resp[rlen++] = 0x80;
        resp[rlen++] = 0x01;  // CLASS = IN (0x0001) with the mDNS cache-flush bit (0x8000) set
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x78;  // TTL = 120s
        resp[rlen++] = 0x00;
        resp[rlen++] = 0x04;  // RDLENGTH = 4
        IPAddress ip = Ethernet.localIP();
        resp[rlen++] = ip[0];
        resp[rlen++] = ip[1];
        resp[rlen++] = ip[2];
        resp[rlen++] = ip[3];

        s_mdns_eth_udp.beginPacket(kMdnsMulticastAddr, kMdnsPort);
        s_mdns_eth_udp.write(resp, rlen);
        s_mdns_eth_udp.endPacket();
    }

    // M5_Ethernet's own M5_Ethernet.h now patches MAX_SOCK_NUM up to the
    // W5500's real max of 8 (see that file's own comment - upstream
    // hardcoded it to 2, which starved this gateway's two listening
    // servers against each other once the HTTP status site was added
    // alongside the JSON protocol server). Budget across all 8: JSON
    // listener(1) + these clients(1) + HTTP listener(1) + HTTP clients
    // (kMaxEthHttpClients, 3 - see that constant's own comment for why
    // it needs real headroom, unlike this one) + the Ethernet mDNS
    // responder's own UDP socket(1) + a spare for Ethernet.maintain()'s
    // transient DHCP-renewal socket(1) = 8 of 8, fully budgeted rather
    // than reserving slack neither transport actually needs.
    constexpr uint8_t kMaxEthTcpClients = 1;
    // WiFi (ESP32's native lwIP) has no equivalent hardware ceiling - this
    // used to share the Ethernet constant above (=1) for no real reason,
    // which meant the desktop app's own long-lived monitoring connection
    // silently starved out any other WiFi TCP client, including an OTA
    // push tool trying to connect at the same time (confirmed live: an
    // `ota_start` attempt got an immediate connection reset while the
    // desktop app was connected). Room for a few concurrent clients here
    // costs nothing real and avoids that entirely for the common case.
    constexpr uint8_t kMaxWifiTcpClients = 4;
    constexpr uint16_t kGatewayTcpPort = 5055;

    // Templated over EthernetClient/WiFiClient (both expose the same
    // available()/read()/write()/connected() shape in the Arduino core, no
    // common virtual base needed) - Ethernet and WiFi each get their own
    // listener + client slot(s) below, serviced by the same code either
    // way, active one at a time per gateway_ethernet_begin()'s own
    // Ethernet-priority-then-WiFi-fallback decision in setup().
    template <typename ClientT>
    struct TcpClientState
    {
        ClientT client;
        // Must fit an ota_chunk line: a 2048-byte chunk hex-encodes to
        // 4096 chars, plus ~60 bytes of JSON overhead - see
        // handle_ota_chunk_command()'s own comment on why this TCP path
        // isn't stuck at ESP-NOW's 220-byte/640-line sizing.
        char lineBuf[4300];
        size_t lineLen = 0;
        void reset() { lineLen = 0; }
    };
    EthernetServer s_tcp_server(kGatewayTcpPort);
    TcpClientState<EthernetClient> s_tcp_clients[kMaxEthTcpClients];
    bool s_tcp_server_started = false;

    WiFiServer s_wifi_tcp_server(kGatewayTcpPort);
    TcpClientState<WiFiClient> s_wifi_tcp_clients[kMaxWifiTcpClients];
    bool s_wifi_tcp_server_started = false;

    // Identifies which TCP client slot (if any) is currently having a line
    // dispatched from it - set/cleared around handle_json_line()/
    // handle_command() below, read by drop_other_tcp_clients() (called
    // from self_ota_begin()) to know which single connection to spare
    // when a fresh OTA transfer needs everything else out of the way.
    // Tagged by the TcpClientState slot's own address (unique and stable
    // for the object's lifetime) rather than the ClientT object itself,
    // since Ethernet and WiFi slots are two different C++ types with no
    // common base to point at generically. nullptr whenever nothing is
    // mid-dispatch - correctly means "no TCP client to spare" if an OTA
    // starts from the ESP-NOW entry point instead (see that comment).
    void *s_currentTcpClientTag = nullptr;

    template <typename ClientT>
    void service_tcp_client(TcpClientState<ClientT> &st)
    {
        while (st.client.available())
        {
            char c = static_cast<char>(st.client.read());
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                st.lineBuf[st.lineLen] = '\0';
                if (st.lineLen > 0)
                {
                    char *trimmed = st.lineBuf;
                    while (*trimmed == ' ' || *trimmed == '\t')
                        trimmed++;
                    s_currentTcpClientTag = static_cast<void *>(&st);
                    if (*trimmed == '{')
                        handle_json_line(trimmed);
                    else
                        handle_command(st.lineBuf);
                    s_currentTcpClientTag = nullptr;
                }
                st.lineLen = 0;
                continue;
            }
            if (st.lineLen + 1 < sizeof(st.lineBuf))
                st.lineBuf[st.lineLen++] = c;
            // else: line too long - silently drop the overflow byte rather
            // than corrupt framing, same policy as service_serial().
        }
    }

    // Shared Ethernet/WiFi polling body - which one ever actually runs is
    // decided by poll_tcp_server() below staying strictly if/else on
    // s_eth_connected vs s_wifi_connected, never both, matching
    // setup()'s own Ethernet-priority-then-WiFi-fallback decision (see
    // that section's own comment for why running both at once would be
    // a real correctness problem, not just wasteful).
    //
    // When every slot is already full, this EVICTS the oldest connection
    // (slot 0 - slots fill in order and this array is small/short-lived,
    // so a real LRU isn't worth the bookkeeping) rather than rejecting
    // the new one outright. Real problem this fixes: a long-lived client
    // (the desktop app's own monitoring connection) used to permanently
    // starve out any other same-transport connection attempt, including
    // an OTA push tool trying to connect - confirmed live, an `ota_start`
    // attempt got an immediate connection reset while the desktop app
    // happened to be connected. Whoever connects LAST now always
    // eventually gets in; an evicted client (e.g. the desktop app) just
    // needs to reconnect, same as if the gateway had rebooted.
    //
    // Matters most for Ethernet, where kMaxEthTcpClients is a genuine
    // hardware ceiling (M5_Ethernet's W5500 completes a TCP handshake in
    // hardware before the sketch ever calls accept(), so an incoming
    // connection nobody claims permanently occupies one of only 2 total
    // hardware sockets until it times out on its own - confirmed live
    // separately, a leaked never-serviced connection made that server
    // refuse everything for several minutes) - but applying the same
    // policy on WiFi too costs nothing and stays consistent.
    template <typename ServerT, typename ClientT>
    void poll_tcp_clients(ServerT &server, TcpClientState<ClientT> *clients, uint8_t clientCount,
                          bool &serverStarted, const char *transportLabel)
    {
        if (!serverStarted)
        {
            server.begin();
            serverStarted = true;
            Serial.printf("TCP JSON server (%s) listening on port %u\n", transportLabel, kGatewayTcpPort);
        }
        // Keeps accepting into every free slot, not just the first one -
        // see poll_http_listener()'s own comment for why a single
        // accept() per call silently lost every simultaneous connection
        // past the first, even with several slots configured.
        bool anyFree = false;
        for (uint8_t i = 0; i < clientCount; i++)
        {
            if (!clients[i].client || !clients[i].client.connected())
            {
                anyFree = true;
                ClientT incoming = server.accept();
                if (!incoming)
                    break;
                clients[i].client = incoming;
                clients[i].reset();
                Serial.printf("TCP client connected (%s).\n", transportLabel);
            }
        }
        if (!anyFree)
        {
            ClientT incoming = server.accept();
            if (incoming)
            {
                Serial.printf("TCP (%s): all %u slot(s) full - evicting the oldest connection for this new one\n",
                              transportLabel, clientCount);
                clients[0].client.stop();
                clients[0].client = incoming;
                clients[0].reset();
            }
        }

        for (uint8_t i = 0; i < clientCount; i++)
            if (clients[i].client && clients[i].client.connected())
                service_tcp_client(clients[i]);
    }

    // See self_ota_begin()'s own call site and s_currentTcpClientTag's own
    // comment - drops every TCP connection on whichever transport is
    // active except the one (if any) whose slot address matches the
    // current tag, so a fresh OTA transfer always gets the gateway's full
    // attention instead of competing with, say, the desktop app's own
    // long-lived monitoring connection.
    void drop_other_tcp_clients()
    {
        if (s_eth_connected)
        {
            for (uint8_t i = 0; i < kMaxEthTcpClients; i++)
            {
                if (static_cast<void *>(&s_tcp_clients[i]) == s_currentTcpClientTag)
                    continue;
                if (s_tcp_clients[i].client)
                {
                    s_tcp_clients[i].client.stop();
                    s_tcp_clients[i].reset();
                }
            }
        }
        else if (s_wifi_connected)
        {
            for (uint8_t i = 0; i < kMaxWifiTcpClients; i++)
            {
                if (static_cast<void *>(&s_wifi_tcp_clients[i]) == s_currentTcpClientTag)
                    continue;
                if (s_wifi_tcp_clients[i].client)
                {
                    s_wifi_tcp_clients[i].client.stop();
                    s_wifi_tcp_clients[i].reset();
                }
            }
        }
    }

    void poll_tcp_server()
    {
        // Strictly one transport at a time, never both - matches
        // s_eth_connected/s_wifi_connected themselves already being
        // mutually exclusive (see setup()'s own comment).
        if (s_eth_connected)
            poll_tcp_clients(s_tcp_server, s_tcp_clients, kMaxEthTcpClients, s_tcp_server_started, "Ethernet");
        else if (s_wifi_connected)
            poll_tcp_clients(s_wifi_tcp_server, s_wifi_tcp_clients, kMaxWifiTcpClients, s_wifi_tcp_server_started,
                              "WiFi");
    }

    // M5_Ethernet's socketSend() silently clamps any single write over
    // ~2KB (the W5500's per-socket TX buffer) while still reporting the
    // full size as sent - proven live in RTSNow-SPI-CCP's PoeStatusServer
    // while serving an embedded logo image over this same library. A
    // known_devices line can comfortably exceed that with enough peers, so
    // every write here goes out in small, explicitly-checked chunks rather
    // than trusting one write() call to either succeed completely or fail
    // loudly.
    constexpr size_t kTcpChunkSize = 512;

    template <typename ClientT>
    void write_line_to_tcp_client(ClientT &client, const String &line)
    {
        const uint8_t *data = reinterpret_cast<const uint8_t *>(line.c_str());
        size_t total = line.length();
        size_t sent = 0;
        while (sent < total)
        {
            size_t chunk = (total - sent) < kTcpChunkSize ? (total - sent) : kTcpChunkSize;
            size_t written = client.write(data + sent, chunk);
            if (written == 0)
                return;  // client gone - service_tcp_client's own connected() check cleans it up next poll
            sent += written;
        }
        client.write(reinterpret_cast<const uint8_t *>("\n"), 1);
    }

    void broadcast_line_to_tcp_clients(const String &line)
    {
        // Only one of these two loops ever actually finds a connected
        // client in practice (Ethernet and WiFi are never both up - see
        // poll_tcp_server()'s own comment) - written as two loops rather
        // than one dispatching on s_eth_connected/s_wifi_connected so a
        // client that's mid-disconnect during the transport decision's
        // own transition doesn't miss an event either way.
        for (uint8_t i = 0; i < kMaxEthTcpClients; i++)
            if (s_tcp_clients[i].client && s_tcp_clients[i].client.connected())
                write_line_to_tcp_client(s_tcp_clients[i].client, line);
        for (uint8_t i = 0; i < kMaxWifiTcpClients; i++)
            if (s_wifi_tcp_clients[i].client && s_wifi_tcp_clients[i].client.connected())
                write_line_to_tcp_client(s_wifi_tcp_clients[i].client, line);
    }
#endif

#if defined(BOARD_GATEWAY_ATOMS3_POE)
    // ---- HTML status page (port 80) - separate from the JSON protocol
    // server above (port 5055, for the desktop app); this one's for a
    // human with a browser. Structurally a port of RTSNow-SPI-CCP's own
    // PoeStatusServer.cpp (same board, same dual-transport situation,
    // proven design) - adapted here to show gateway state (known devices,
    // ESP-NOW channel) instead of dryer registers. No embedded logo image
    // (that'd mean duplicating rts_gif/rtslogo.h across repos for a
    // cosmetic-only page) - plain text/table content only.
    constexpr uint16_t kHttpPort = 80;
    constexpr uint32_t kHttpClientTimeoutMs = 3000;
    // A real browser opens several simultaneous connections for one page
    // load - the document itself, the logo image, and the page's own
    // script firing its first fetch('/api/status') immediately - not one
    // at a time. This used to be a single non-array slot per transport
    // (mirroring an early mistake already fixed once for the JSON
    // protocol server - see kMaxEthTcpClients/kMaxWifiTcpClients's own
    // comments), so only the FIRST of those concurrent requests ever got
    // served; the rest got an outright connection refusal, silently
    // swallowed by the page's own fetch().catch(){} - symptom looked
    // exactly like "the page only updates on the 5s timer, not
    // immediately on load", confirmed live via a concurrent-request test
    // (3 of 4 simultaneous requests refused outright with just 1 slot).
    constexpr uint8_t kMaxEthHttpClients = 3;
    constexpr uint8_t kMaxWifiHttpClients = 4;  // no hardware ceiling on WiFi - see kMaxWifiTcpClients's own comment

    template <typename ClientT>
    struct HttpListenerState
    {
        ClientT client;
        char requestLine[160] = {0};
        size_t requestLineLen = 0;
        bool requestLineDone = false;
        uint32_t clientStartMs = 0;
        void reset()
        {
            requestLineLen = 0;
            requestLineDone = false;
        }
    };

    EthernetServer s_http_eth_server(kHttpPort);
    WiFiServer s_http_wifi_server(kHttpPort);
    HttpListenerState<EthernetClient> s_http_eth_states[kMaxEthHttpClients];
    HttpListenerState<WiFiClient> s_http_wifi_states[kMaxWifiHttpClients];

    // M5_Ethernet's socketSend() silently clamps any single write over
    // ~2KB (the W5500's per-socket TX buffer) while still reporting the
    // full size as sent - see write_http_logo()'s own comment, which
    // already worked around this for the logo specifically. write_http_
    // style()/write_http_script() below are each one big println() call
    // built from many adjacent string literals - write_http_script() grew
    // past 2KB over the course of this session without this same
    // discipline applied, and got silently truncated on Ethernet as a
    // result (confirmed live: the page loaded, but every AJAX-updated
    // field stayed at its placeholder "-" because the truncated <script>
    // block failed to parse in the browser at all). Chunking both here,
    // not just whichever one happens to be over the line today.
    constexpr size_t kHttpWriteChunkSize = 512;

    template <typename ClientT>
    void write_http_chunked(ClientT &client, const char *data)
    {
        size_t total = strlen(data);
        size_t sent = 0;
        while (sent < total)
        {
            size_t chunk = (total - sent) < kHttpWriteChunkSize ? (total - sent) : kHttpWriteChunkSize;
            size_t written = client.write(reinterpret_cast<const uint8_t *>(data) + sent, chunk);
            if (written == 0)
                return;  // client gone
            sent += written;
        }
    }

    template <typename ClientT>
    void write_http_style(ClientT &client)
    {
        // Same chrome as RTSNow-SPI-CCP's PoeStatusServer.cpp (logo/title
        // header, left sidebar nav, section box styling) - this is the same
        // physical board family and the same "Digi gateway admin UI" look
        // that page deliberately borrowed, so this page matches it exactly
        // rather than introducing a second, different visual language.
        write_http_chunked(client, "<style>"
                       "body{font-family:-apple-system,Arial,sans-serif;margin:0;background:#fff;color:#222;}"
                       "#topbar{display:flex;align-items:center;gap:16px;padding:12px 20px;}"
                       "#topbar img{width:100px;height:100px;}"
                       "#topbar h1{font-size:1.4em;margin:0;color:#001a70;}"
                       "#layout{display:flex;}"
                       "#sidebar{width:170px;flex-shrink:0;padding:16px 12px;border-right:1px solid #ddd;}"
                       "#sidebar a{display:block;color:#0645ad;text-decoration:none;font-size:0.92em;margin-bottom:8px;}"
                       "#sidebar a:hover{text-decoration:underline;}"
                       "#sidebar .navhead{font-weight:700;color:#222;margin:14px 0 4px;font-size:0.9em;}"
                       "#main{flex:1;padding:16px 20px;min-width:0;}"
                       ".box{border:1px solid #b9c9e8;margin-bottom:14px;border-radius:2px;overflow:hidden;}"
                       ".box h2{background:#001a70;color:#fff;font-size:1em;margin:0;padding:8px 12px;}"
                       ".box .body{padding:10px 14px;}"
                       ".box .body p{margin:4px 0;font-size:0.92em;}"
                       "table{border-collapse:collapse;width:100%;font-size:0.9em;}"
                       "th{text-align:left;color:#001a70;border-bottom:2px solid #b9c9e8;padding:5px 8px;}"
                       "td{border-bottom:1px solid #eee;padding:5px 8px;}"
                       // Missing this is exactly why the mesh RSSI bars
                       // rendered garbled/overlapping - the ▂▄▆█▁ glyphs
                       // are block-drawing characters that only line up
                       // cleanly at a fixed advance width; a proportional
                       // body font renders them at inconsistent widths.
                       // Matches PoeStatusServer.cpp's own td.rssi rule.
                       "td.rssi{font-family:monospace;letter-spacing:1px;}"
                       "#sidebar a.active{font-weight:700;text-decoration:underline;}"
                       "button.reboot-btn{background:#001a70;color:#fff;border:none;border-radius:2px;"
                       "padding:4px 10px;font-size:0.85em;cursor:pointer;}"
                       "button.reboot-btn:disabled{background:#9aa5c4;cursor:default;}"
                       // A row being rebooted greys out until a fresh
                       // heartbeat proves the device is back - see
                       // rebootDevice()/refresh()'s own comment for the
                       // exact "back up" heuristic.
                       "tr.rebooting{opacity:0.45;}"
                       "</style>");
    }

    template <typename ClientT>
    void write_http_script(ClientT &client)
    {
        write_http_chunked(client, "<script>"
                       // Mirrors desktop-app/src/gateway/models.py's own
                       // _DEVICE_TYPE_LABELS exactly - deviceTypeName
                       // travels the wire as a terse, hyphen-free
                       // identifier (see RTSNOW_DeviceIdentity's char[16]),
                       // mapped to a friendlier label for display only in
                       // both places that render it. No shared source
                       // between this C++/JS page and that Python file -
                       // keep both in sync by hand if this list changes.
                       "var DEVICE_TYPE_LABELS={"
                       "'UsbGateway':'USB-Gateway',"
                       "'EthernetGateway':'Ethernet-Gateway',"
                       "'RTSNow-UNADYN':'SPI-CCP'"
                       "};"
                       "function deviceTypeLabel(t){return DEVICE_TYPE_LABELS[t]||t;}"
                       // Same 4-bar glyph + dBm thresholds as
                       // PoeStatusServer.cpp's own rssiBars() and the
                       // desktop app's rssi_bar_count()/format_rssi()
                       // (node_network_page.py) - one shared visual
                       // language for "what counts as a good link" across
                       // every RTS-NOW surface, not a fourth independently
                       // tuned scale.
                       "function rssiBars(rssi){"
                       "if(rssi===null||rssi===undefined)return '\xE2\x80\x93';"
                       "var t=[-50,-60,-70,-80],f=['\xE2\x96\x82','\xE2\x96\x84','\xE2\x96\x86','\xE2\x96\x88'],bars=0;"
                       "for(var i=0;i<t.length;i++)if(rssi>=t[i])bars++;"
                       "var g='';"
                       "for(var i=0;i<4;i++)g+=(i<bars?f[i]:'\xE2\x96\x81');"
                       "return g+' '+rssi+' dBm';"
                       "}"
                       "function formatUptime(s){"
                       "var d=Math.floor(s/86400);s%=86400;"
                       "var h=Math.floor(s/3600);s%=3600;"
                       "var m=Math.floor(s/60);s%=60;"
                       "var parts=[];"
                       "if(d>0)parts.push(d+'d');"
                       "if(d>0||h>0)parts.push(h+'h');"
                       "if(d>0||h>0||m>0)parts.push(m+'m');"
                       "parts.push(s+'s');"
                       "return parts.join(' ');"
                       "}"
                       "function el(id){return document.getElementById(id);}"
                       // mac -> Date.now() at the moment Reboot was clicked
                       // for it. A row stays greyed/disabled (see
                       // refresh()'s own use of this) until a poll reports
                       // an 'ago' smaller than how long it's been since the
                       // click - that only happens once the device has
                       // actually re-announced itself post-reboot, so it's
                       // a real "back up and communicating" signal, not
                       // just a fixed timeout guess.
                       "var reboots={};"
                       "function rebootDevice(mac,btn){"
                       "if(btn)btn.disabled=true;"
                       "reboots[mac]=Date.now();"
                       "fetch('/api/reboot?mac='+mac).catch(function(){});"
                       "refresh();"
                       "}"
                       // Rebooting the gateway itself, not some other known
                       // device (see /api/reboot_self's own comment - no
                       // mac involved, it always targets this gateway).
                       // The fetch itself will almost always error out or
                       // hang (the gateway drops off the network mid-
                       // response as it restarts) - that's expected, not a
                       // real failure, so both .then and .catch land on
                       // the same "rebooting" message rather than trying
                       // to distinguish a real error from the reboot
                       // itself severing the connection.
                       "function rebootSelf(btn){"
                       "if(!confirm('Reboot this gateway now?'))return;"
                       "btn.disabled=true;"
                       "el('rebootSelfStatus').textContent='Rebooting...';"
                       "fetch('/api/reboot_self').then(function(){},function(){});"
                       "}"
                       "function refresh(){"
                       "fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
                       "if(el('uptime'))el('uptime').textContent=formatUptime(d.uptime);"
                       "if(el('freeHeap'))el('freeHeap').textContent=d.freeHeap.toLocaleString()+' bytes';"
                       "if(el('connStatus'))el('connStatus').textContent=d.connStatus;"
                       "if(el('channel'))el('channel').textContent=d.channel;"
                       "if(el('netDetails')){"
                       "var net='';"
                       "if(d.ethIp)net+='<p>Ethernet IP: '+d.ethIp+'</p>';"
                       "if(d.wifiIp)net+='<p>WiFi IP: '+d.wifiIp+' (SSID: '+d.wifiSsid+')</p>';"
                       "el('netDetails').innerHTML=net;"
                       "}"
                       "if(el('devicesBody')){"
                       "var rows='';"
                       "if(d.devices.length===0)rows='<tr><td colspan=\"8\">No devices heard yet</td></tr>';"
                       "d.devices.forEach(function(p){"
                       "var rebooting=false;"
                       "if(reboots[p.mac]!==undefined){"
                       "var elapsed=(Date.now()-reboots[p.mac])/1000;"
                       "if(p.ago<elapsed)delete reboots[p.mac];"
                       "else rebooting=true;"
                       "}"
                       "rows+='<tr class=\"'+(rebooting?'rebooting':'')+'\"><td>'+p.name+'</td><td>'+p.project+'</td><td>'+"
                       "deviceTypeLabel(p.type)+'</td><td>'+p.mac+'</td><td>'+(p.ip||'-')+'</td><td class=\"rssi\">'+"
                       "rssiBars(p.meshRssi)+'</td><td>'+p.ago+'s ago</td><td><button class=\"reboot-btn\" '+"
                       "(rebooting?'disabled':'')+' onclick=\"rebootDevice(\\''+p.mac+'\\',this)\">Reboot</button></td></tr>';"
                       "});"
                       "el('devicesBody').innerHTML=rows;"
                       "}"
                       "}).catch(function(){});"
                       "}"
                       "refresh();"
                       "setInterval(refresh,5000);"
                       "</script>");
    }

    const char *reset_reason_str(esp_reset_reason_t reason)
    {
        switch (reason)
        {
        case ESP_RST_POWERON: return "Power-on";
        case ESP_RST_EXT: return "External pin";
        case ESP_RST_SW: return "Software reset (esp_restart)";
        case ESP_RST_PANIC: return "Panic/exception";
        case ESP_RST_INT_WDT: return "Interrupt watchdog";
        case ESP_RST_TASK_WDT: return "Task watchdog";
        case ESP_RST_WDT: return "Other watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT: return "Brownout";
        case ESP_RST_SDIO: return "SDIO";
        default: return "Unknown";
        }
    }

    template <typename ClientT>
    void write_http_nav_link(ClientT &client, const char *href, const char *label, const char *activePage)
    {
        if (strcmp(href, activePage) == 0)
            client.printf("<a href=\"%s\" class=\"active\">%s</a>", href, label);
        else
            client.printf("<a href=\"%s\">%s</a>", href, label);
    }

    // Shared chrome for every real page (Home/Network/Known Devices/System
    // Information all now have their own URL - replaces the old single-
    // page-with-#anchors design, whose sidebar just said "On this page").
    // activePage is one of the href values passed to write_http_nav_link
    // below, used to bold/underline whichever page is currently showing.
    template <typename ClientT>
    void write_http_page_head(ClientT &client, const char *title, const char *activePage)
    {
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/html");
        client.println("Connection: close");
        client.println();
        client.printf("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                      "<title>%s</title>",
                      title);
        write_http_style(client);
        client.println("</head><body>");
        client.println("<div id=\"topbar\"><img src=\"/logo.gif\" alt=\"RTS\">"
                       "<h1>RTS-NOW Gateway Status</h1></div>");
        client.println("<div id=\"layout\">");
        client.print("<div id=\"sidebar\">");
        write_http_nav_link(client, "/", "Home", activePage);
        write_http_nav_link(client, "/network", "Network", activePage);
        write_http_nav_link(client, "/devices", "Known Devices", activePage);
        write_http_nav_link(client, "/system", "System Information", activePage);
        client.println("</div>");
        client.println("<div id=\"main\">");
    }

    template <typename ClientT>
    void write_http_page_foot(ClientT &client)
    {
        client.println("</div></div>");  // close #main, #layout
        write_http_script(client);
        client.println("</body></html>");
    }

    template <typename ClientT>
    void write_http_home_page(ClientT &client)
    {
        write_http_page_head(client, "RTS-NOW Gateway - Home", "/");
        client.println("<div class=\"box\"><h2>Device</h2><div class=\"body\">");
        client.printf("<p><b>Gateway</b> (deviceID 0x%08X)</p>\n", local_device_id());
        client.printf("<p>MAC address: %s</p>\n", WiFi.macAddress().c_str());
        client.println("<p>Uptime: <span id=\"uptime\">-</span></p>");
        client.println("<p>Free heap: <span id=\"freeHeap\">-</span></p>");
        client.println("<p>Firmware: 1.0.0</p>");
        client.println("<p>ESP-NOW channel: <span id=\"channel\">-</span></p>");
        client.println("</div></div>");
        write_http_page_foot(client);
    }

    template <typename ClientT>
    void write_http_network_page(ClientT &client)
    {
        write_http_page_head(client, "RTS-NOW Gateway - Network", "/network");
        client.println("<div class=\"box\"><h2>Network</h2><div class=\"body\">");
        client.println("<p>Connection status: <b id=\"connStatus\">-</b></p>");
        client.println("<div id=\"netDetails\"></div>");
        client.println("</div></div>");
        write_http_page_foot(client);
    }

    template <typename ClientT>
    void write_http_devices_page(ClientT &client)
    {
        write_http_page_head(client, "RTS-NOW Gateway - Known Devices", "/devices");
        client.println("<div class=\"box\"><h2>Known Devices</h2><div class=\"body\">");
        client.println("<table><tr><th>Name</th><th>Project</th><th>Type</th><th>MAC</th><th>IP</th><th>Mesh</th>"
                       "<th>Last heard</th><th>Action</th></tr><tbody id=\"devicesBody\"></tbody></table>");
        client.println("</div></div>");
        write_http_page_foot(client);
    }

    template <typename ClientT>
    void write_http_system_page(ClientT &client)
    {
        write_http_page_head(client, "RTS-NOW Gateway - System Information", "/system");
        client.println("<div class=\"box\"><h2>Gateway</h2><div class=\"body\">");
        client.printf("<p><b>Gateway</b> (deviceID 0x%08X)</p>\n", local_device_id());
        client.printf("<p>Board: %s</p>\n", BOARD_NAME);
        client.printf("<p>MAC address: %s</p>\n", WiFi.macAddress().c_str());
        client.println("<p>Uptime: <span id=\"uptime\">-</span></p>");
        client.println("<p>Free heap: <span id=\"freeHeap\">-</span></p>");
        client.println("<p>Firmware: 1.0.0</p>");
        client.println("<p>ESP-NOW channel: <span id=\"channel\">-</span></p>");
        client.println("</div></div>");

        client.println("<div class=\"box\"><h2>Network</h2><div class=\"body\">");
        client.println("<p>Connection status: <b id=\"connStatus\">-</b></p>");
        client.println("<div id=\"netDetails\"></div>");
        client.println("</div></div>");

        client.println("<div class=\"box\"><h2>Hardware / Firmware</h2><div class=\"body\">");
        client.printf("<p>Chip: %s rev %d, %d core(s)</p>\n", ESP.getChipModel(), ESP.getChipRevision(),
                      ESP.getChipCores());
        client.printf("<p>CPU frequency: %u MHz</p>\n", ESP.getCpuFreqMHz());
        client.printf("<p>Flash size: %u bytes</p>\n", ESP.getFlashChipSize());
        client.printf("<p>SDK version: %s</p>\n", ESP.getSdkVersion());
        client.printf("<p>Last reset reason: %s</p>\n", reset_reason_str(esp_reset_reason()));
        client.println("</div></div>");

        client.println("<div class=\"box\"><h2>Actions</h2><div class=\"body\">");
        client.println("<button class=\"reboot-btn\" onclick=\"rebootSelf(this)\">Reboot Gateway</button>"
                       "<span id=\"rebootSelfStatus\" style=\"margin-left:10px;\"></span>");
        client.println("</div></div>");
        write_http_page_foot(client);
    }

    // Chunk size well under the W5500's per-socket TX buffer - see
    // PoeStatusServer.cpp's own kLogoChunkSize comment for the exact bug
    // (M5_Ethernet's socketSend() silently clamping a single large write)
    // this sidesteps; WiFiClient has no equivalent limit but chunking it
    // the same way regardless costs nothing and keeps one code path.
    constexpr size_t kLogoChunkSize = 512;

    template <typename ClientT>
    void write_http_logo(ClientT &client)
    {
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: image/gif");
        client.printf("Content-Length: %u\n", rts_gif_len);
        client.println("Connection: close");
        client.println();
        size_t sent = 0;
        while (sent < rts_gif_len)
        {
            size_t remaining = rts_gif_len - sent;
            size_t chunk = remaining < kLogoChunkSize ? remaining : kLogoChunkSize;
            size_t written = client.write(rts_gif + sent, chunk);
            if (written == 0)
                break;  // client disconnected mid-transfer
            sent += written;
        }
    }

    template <typename ClientT>
    void write_http_status_json(ClientT &client)
    {
        uint32_t now = millis();
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: application/json");
        client.println("Connection: close");
        client.println();
        client.printf("{\"uptime\":%lu,\"freeHeap\":%u,\"channel\":%u,", static_cast<unsigned long>(now / 1000),
                      ESP.getFreeHeap(), s_channel);

        // Mutually exclusive by construction (see setup()'s own comment) -
        // never both here either.
        if (s_eth_connected)
        {
            client.print("\"connStatus\":\"Ethernet\",");
            client.printf("\"ethIp\":\"%s\",", Ethernet.localIP().toString().c_str());
        }
        else if (s_wifi_connected)
        {
            client.print("\"connStatus\":\"WiFi (fallback)\",");
            client.printf("\"wifiIp\":\"%s\",\"wifiSsid\":\"%s\",", WiFi.localIP().toString().c_str(),
                          WiFi.SSID().c_str());
        }
        else
        {
            client.print("\"connStatus\":\"Disconnected\",");
        }

        client.print("\"devices\":[");
        for (int i = 0; i < s_known_device_count; i++)
        {
            const KnownDevice &d = s_known_devices[i];
            char macStr[18];
            mac_to_str(d.mac, macStr, sizeof(macStr));
            if (i > 0)
                client.print(",");
            client.print("{\"name\":\"");
            client.print(d.friendlyName);
            client.print("\",\"project\":\"");
            client.print(d.projectName);
            client.print("\",\"type\":\"");
            client.print(d.deviceTypeName);
            client.print("\",\"mac\":\"");
            client.print(macStr);
            client.print("\",\"ip\":");
            if (d.ipv4Address != 0)
            {
                client.print("\"");
                client.print(IPAddress(d.ipv4Address).toString());
                client.print("\"");
            }
            else
            {
                client.print("null");
            }
            client.print(",\"meshRssi\":");
            // espNowRssi: this gateway's own measured RSSI on this
            // device's most recent ESP-NOW frame (see KnownDevice's own
            // comment) - the same "MESH" signal the desktop app's Known
            // Devices table shows, not wifiRssi (that device's own link
            // to its WiFi router, a different physical link entirely).
            if (d.espNowRssi == RTSNOW_RSSI_UNKNOWN)
                client.print("null");
            else
                client.print(d.espNowRssi);
            client.print(",\"ago\":");
            client.print(static_cast<unsigned long>((now - d.lastSeenMs) / 1000));
            client.print("}");
        }
        client.println("]}");
    }

    // Splits "GET /api/reboot?mac=AA:BB:CC:DD:EE:FF HTTP/1.1" into route
    // ("/api/reboot") and query ("mac=AA:BB:CC:DD:EE:FF") separately - the
    // route alone used to be extracted (as the whole path+query string,
    // never actually containing a '?' before there was any endpoint that
    // took query parameters at all).
    void extract_http_request(const char *requestLine, char *outPath, size_t outPathSize, char *outQuery,
                              size_t outQuerySize)
    {
        outPath[0] = '\0';
        outQuery[0] = '\0';
        const char *pathStart = strchr(requestLine, ' ');
        if (pathStart == nullptr)
            return;
        pathStart++;
        const char *pathEnd = strchr(pathStart, ' ');
        if (pathEnd == nullptr)
            return;
        const char *queryStart = static_cast<const char *>(memchr(pathStart, '?', pathEnd - pathStart));
        const char *routeEnd = queryStart != nullptr ? queryStart : pathEnd;
        size_t routeLen = static_cast<size_t>(routeEnd - pathStart);
        if (routeLen >= outPathSize)
            routeLen = outPathSize - 1;
        memcpy(outPath, pathStart, routeLen);
        outPath[routeLen] = '\0';
        if (queryStart != nullptr)
        {
            queryStart++;  // skip '?'
            size_t queryLen = static_cast<size_t>(pathEnd - queryStart);
            if (queryLen >= outQuerySize)
                queryLen = outQuerySize - 1;
            memcpy(outQuery, queryStart, queryLen);
            outQuery[queryLen] = '\0';
        }
    }

    // MAC addresses only ever contain hex digits and ':' - both URL-safe
    // unencoded in a query component (see RFC 3986), so the JS side never
    // percent-encodes it and this never needs to percent-decode either.
    bool extract_query_param(const char *query, const char *key, char *outValue, size_t outSize)
    {
        size_t keyLen = strlen(key);
        const char *p = query;
        while (p != nullptr && *p != '\0')
        {
            if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=')
            {
                const char *valueStart = p + keyLen + 1;
                const char *valueEnd = strchr(valueStart, '&');
                size_t valueLen = valueEnd != nullptr ? static_cast<size_t>(valueEnd - valueStart)
                                                      : strlen(valueStart);
                if (valueLen >= outSize)
                    valueLen = outSize - 1;
                memcpy(outValue, valueStart, valueLen);
                outValue[valueLen] = '\0';
                return true;
            }
            p = strchr(p, '&');
            if (p != nullptr)
                p++;
        }
        return false;
    }

    // GET (not POST - this whole HTTP server only ever parses a request
    // line, no body/headers - see poll_http_listener's own comment) -
    // reboots exactly the one node named over ESP-NOW (do_reboot_send).
    // Rebooting THIS gateway itself is a separate endpoint, see
    // write_http_reboot_self_response below.
    template <typename ClientT>
    void write_http_reboot_response(ClientT &client, const char *query)
    {
        char macStr[24] = "";
        uint8_t mac[6];
        bool ok = false;
        const char *error = "missing or invalid mac parameter";
        if (extract_query_param(query, "mac", macStr, sizeof(macStr)) && parse_mac(macStr, mac))
        {
            error = do_reboot_send(mac);
            ok = error == nullptr;
        }
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: application/json");
        client.println("Connection: close");
        client.println();
        if (ok)
            client.println("{\"ok\":true}");
        else
            client.printf("{\"ok\":false,\"error\":\"%s\"}\n", error);
    }

    // No mac/query involved at all - always targets this gateway itself,
    // a purely local action (no ESP-NOW send). Writes the response, gives
    // it a moment to actually reach the client (matches self_ota_end()'s
    // own delay(100)-before-restart pattern elsewhere in this file), then
    // restarts inline - same as every other reboot path in this codebase,
    // not deferred through a pending-flag/loop() dance.
    template <typename ClientT>
    void write_http_reboot_self_response(ClientT &client)
    {
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: application/json");
        client.println("Connection: close");
        client.println();
        client.println("{\"ok\":true}");
        delay(150);
        ESP.restart();
    }

    // One slot's worth of request handling - accepting into a free/
    // evicted slot and iterating every connected slot both happen in
    // poll_http_listener() below, mirroring poll_tcp_clients()'s own
    // multi-slot pattern (same reasoning: a single-client design silently
    // refuses every additional simultaneous connection instead of queuing
    // it, and a real browser opens several at once per page load - see
    // kMaxEthHttpClients's own comment for how this was actually found).
    template <typename ClientT>
    void service_http_client(HttpListenerState<ClientT> &st)
    {
        while (st.client.available())
        {
            char c = static_cast<char>(st.client.read());
            if (c == '\n')
            {
                st.requestLineDone = true;
                break;
            }
            if (c != '\r' && st.requestLineLen < sizeof(st.requestLine) - 1)
                st.requestLine[st.requestLineLen++] = c;
        }

        if (st.requestLineDone)
        {
            st.requestLine[st.requestLineLen] = '\0';
            char path[32];
            char query[64];
            extract_http_request(st.requestLine, path, sizeof(path), query, sizeof(query));
            if (strcmp(path, "/api/status") == 0)
                write_http_status_json(st.client);
            else if (strcmp(path, "/api/reboot") == 0)
                write_http_reboot_response(st.client, query);
            else if (strcmp(path, "/api/reboot_self") == 0)
                write_http_reboot_self_response(st.client);
            else if (strcmp(path, "/logo.gif") == 0)
                write_http_logo(st.client);
            else if (strcmp(path, "/network") == 0)
                write_http_network_page(st.client);
            else if (strcmp(path, "/devices") == 0)
                write_http_devices_page(st.client);
            else if (strcmp(path, "/system") == 0)
                write_http_system_page(st.client);
            else
                write_http_home_page(st.client);
            st.client.stop();
            st.reset();
            return;
        }

        if (millis() - st.clientStartMs > kHttpClientTimeoutMs)
        {
            st.client.stop();
            st.reset();
        }
    }

    template <typename ServerT, typename ClientT>
    void poll_http_listener(ServerT &server, HttpListenerState<ClientT> *states, uint8_t clientCount)
    {
        // Keeps accepting into every free slot, not just the first one -
        // a single accept() per call meant several truly-simultaneous
        // incoming connections (confirmed live: a browser's document +
        // logo + immediate AJAX fetch, all landing in the same instant)
        // only ever got ONE of them serviced per tick even with multiple
        // slots configured, since the loop used to stop right after the
        // first free slot regardless of whether more were both free and
        // pending. Only stops early once accept() itself has nothing left
        // to give - trying further free slots after that wouldn't help.
        bool anyFree = false;
        for (uint8_t i = 0; i < clientCount; i++)
        {
            if (!states[i].client || !states[i].client.connected())
            {
                anyFree = true;
                ClientT incoming = server.accept();
                if (!incoming)
                    break;
                states[i].client = incoming;
                states[i].reset();
                states[i].clientStartMs = millis();
            }
        }
        if (!anyFree)
        {
            // Every slot full - evict the oldest (slot 0, same reasoning
            // as poll_tcp_clients()'s own comment: a small, short-lived
            // array isn't worth real LRU bookkeeping) rather than refuse
            // the new connection outright - the exact behavior that
            // caused this whole class of bug in the first place.
            ClientT incoming = server.accept();
            if (incoming)
            {
                states[0].client.stop();
                states[0].client = incoming;
                states[0].reset();
                states[0].clientStartMs = millis();
            }
        }

        for (uint8_t i = 0; i < clientCount; i++)
            if (states[i].client && states[i].client.connected())
                service_http_client(states[i]);
    }

    bool s_http_eth_started = false;
    bool s_http_wifi_started = false;

    // One-shot, boot-time decision, matching s_eth_connected/s_wifi_connected
    // themselves (see setup()'s own comment) - not re-checked after boot.
    void begin_http_status_server()
    {
        if (s_eth_connected)
        {
            s_http_eth_server.begin();
            s_http_eth_started = true;
            Serial.printf("HTTP status page (Ethernet) listening on http://%s/\n",
                          Ethernet.localIP().toString().c_str());
        }
        else if (s_wifi_connected)
        {
            s_http_wifi_server.begin();
            s_http_wifi_started = true;
            Serial.printf("HTTP status page (WiFi) listening on http://%s/\n", WiFi.localIP().toString().c_str());
        }
    }

    void poll_http_status_server()
    {
        if (s_http_eth_started)
            poll_http_listener(s_http_eth_server, s_http_eth_states, kMaxEthHttpClients);
        else if (s_http_wifi_started)
            poll_http_listener(s_http_wifi_server, s_http_wifi_states, kMaxWifiHttpClients);
    }
#endif

    // ---- Serial command handling: human-typeable text interface ----

    void cmd_provision(const char *macStr, const char *ssid, const char *password)
    {
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            Serial.println("provision: invalid MAC address");
            return;
        }
        const char *error = do_provision(mac, ssid, password);
        if (error != nullptr)
        {
            Serial.printf("provision: %s\n", error);
            return;
        }
        Serial.printf("Sent Wi-Fi credentials (SSID \"%s\") to %s - waiting for it to join and announce...\n", ssid, macStr);
    }

    void cmd_reject(const char *macStr)
    {
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            Serial.println("reject: invalid MAC address");
            return;
        }
        const char *error = do_reject(mac);
        if (error != nullptr)
        {
            Serial.printf("reject: %s\n", error);
            return;
        }
        Serial.printf("Rejected %s - it should resume sweeping\n", macStr);
    }

    void cmd_forget(const char *macStr)
    {
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            Serial.println("forget: invalid MAC address");
            return;
        }
        const char *error = do_forget(mac);
        if (error != nullptr)
        {
            Serial.printf("forget: %s\n", error);
            return;
        }
        Serial.printf("Forgot %s - it'll reappear if it announces/heartbeats again\n", macStr);
    }

    // Auto-detects the setting's value type from its typed-in text form:
    // "true"/"false" -> bool, a clean integer -> int32, a clean float ->
    // float32, anything else -> string. A human-typed convenience, not
    // part of the wire protocol itself (RTSNOW_SettingPayload just carries
    // whatever type is decided here). The JSON interface skips this
    // guesswork entirely - see handle_setting_command's explicit `valueType`.
    void cmd_setting(const char *macStr, const char *key, const char *value)
    {
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            Serial.println("setting: invalid MAC address");
            return;
        }
        if (strlen(key) >= sizeof(RTSNOW_SettingPayload::key))
        {
            Serial.println("setting: key too long");
            return;
        }

        RTSNOW_SettingPayload payload{};
        strncpy(payload.key, key, sizeof(payload.key) - 1);

        char *end = nullptr;
        long asInt = strtol(value, &end, 10);
        bool isInt = (end != value && *end == '\0');
        float asFloat = strtof(value, &end);
        bool isFloat = (end != value && *end == '\0');

        if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0)
        {
            payload.valueType = RTSNOW_SETTING_BOOL;
            payload.boolValue = (strcmp(value, "true") == 0);
        }
        else if (isInt)
        {
            payload.valueType = RTSNOW_SETTING_INT32;
            payload.intValue = static_cast<int32_t>(asInt);
        }
        else if (isFloat)
        {
            payload.valueType = RTSNOW_SETTING_FLOAT32;
            payload.floatValue = asFloat;
        }
        else
        {
            if (strlen(value) >= sizeof(payload.stringValue))
            {
                Serial.println("setting: string value too long");
                return;
            }
            payload.valueType = RTSNOW_SETTING_STRING;
            strncpy(payload.stringValue, value, sizeof(payload.stringValue) - 1);
        }

        const char *error = do_setting_send(mac, payload);
        if (error != nullptr)
        {
            Serial.printf("setting: %s\n", error);
            return;
        }
        Serial.printf("Sent setting \"%s\" = \"%s\" to %s\n", key, value, macStr);
    }

    void cmd_reboot(const char *macStr)
    {
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            Serial.println("reboot: invalid MAC address");
            return;
        }
        const char *error = do_reboot_send(mac);
        if (error != nullptr)
        {
            Serial.printf("reboot: %s\n", error);
            return;
        }
        Serial.printf("Sent reboot to %s\n", macStr);
    }

    void cmd_poll_registers(const char *macStr)
    {
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            Serial.println("poll_registers: invalid MAC address");
            return;
        }
        const char *error = do_request_registers_send(mac);
        if (error != nullptr)
        {
            Serial.printf("poll_registers: %s\n", error);
            return;
        }
        Serial.printf("Requested registers from %s\n", macStr);
    }

    void cmd_channel(const char *arg)
    {
        char *end = nullptr;
        long n = strtol(arg, &end, 10);
        if (end == arg || *end != '\0' || n < RTSNOW_PROVISIONING_CHANNEL_MIN || n > RTSNOW_PROVISIONING_CHANNEL_MAX)
        {
            Serial.printf("channel: expected a number %u-%u\n", RTSNOW_PROVISIONING_CHANNEL_MIN, RTSNOW_PROVISIONING_CHANNEL_MAX);
            return;
        }
        do_channel(static_cast<uint8_t>(n));
        Serial.printf("Gateway now on channel %u (saved)\n", s_channel);
    }

    void print_help()
    {
        Serial.println();
        Serial.println("Commands:");
        Serial.println("  list                                 - show known devices (from RTSNOW_ANNOUNCE/HEARTBEAT)");
        Serial.println("  pending                              - show pending Wi-Fi provisioning requests");
        Serial.println("  provision <mac> <ssid> <password>    - accept a pending request, send Wi-Fi credentials");
        Serial.println("  reject <mac>                         - decline a pending request");
        Serial.println("  setting <mac> <key> <value>          - push a generic setting to a known device");
        Serial.println("  reboot <mac>                         - ask a known device to restart");
        Serial.println("  poll_registers <mac>                 - ask a known device to report its register table");
        Serial.println("  channel <1-11>                       - change/persist this gateway's own ESP-NOW channel");
        Serial.println("  discover                             - broadcast RTSNOW_DISCOVER now");
        Serial.println("  help                                 - show this list");
        Serial.println("Note: SSID/password/key/value must not contain spaces in this v1 command interface.");
        Serial.println("A parallel JSON-lines interface is also available for a desktop app - see ../../SERIAL_PROTOCOL.md.");
    }

    void handle_command(char *line)
    {
        char *cmd = strtok(line, " ");
        if (cmd == nullptr)
            return;

        if (strcmp(cmd, "list") == 0)
        {
            print_known_devices();
        }
        else if (strcmp(cmd, "pending") == 0)
        {
            print_pending();
        }
        else if (strcmp(cmd, "provision") == 0)
        {
            char *mac = strtok(nullptr, " ");
            char *ssid = strtok(nullptr, " ");
            char *password = strtok(nullptr, " ");
            if (!mac || !ssid || !password)
                Serial.println("usage: provision <mac> <ssid> <password>");
            else
                cmd_provision(mac, ssid, password);
        }
        else if (strcmp(cmd, "reject") == 0)
        {
            char *mac = strtok(nullptr, " ");
            if (!mac)
                Serial.println("usage: reject <mac>");
            else
                cmd_reject(mac);
        }
        else if (strcmp(cmd, "forget") == 0)
        {
            char *mac = strtok(nullptr, " ");
            if (!mac)
                Serial.println("usage: forget <mac>");
            else
                cmd_forget(mac);
        }
        else if (strcmp(cmd, "setting") == 0)
        {
            char *mac = strtok(nullptr, " ");
            char *key = strtok(nullptr, " ");
            char *value = strtok(nullptr, " ");
            if (!mac || !key || !value)
                Serial.println("usage: setting <mac> <key> <value>");
            else
                cmd_setting(mac, key, value);
        }
        else if (strcmp(cmd, "reboot") == 0)
        {
            char *mac = strtok(nullptr, " ");
            if (!mac)
                Serial.println("usage: reboot <mac>");
            else
                cmd_reboot(mac);
        }
        else if (strcmp(cmd, "poll_registers") == 0)
        {
            char *mac = strtok(nullptr, " ");
            if (!mac)
                Serial.println("usage: poll_registers <mac>");
            else
                cmd_poll_registers(mac);
        }
        else if (strcmp(cmd, "channel") == 0)
        {
            char *n = strtok(nullptr, " ");
            if (!n)
                Serial.println("usage: channel <1-11>");
            else
                cmd_channel(n);
        }
        else if (strcmp(cmd, "discover") == 0)
        {
            send_broadcast(RTSNOW_DISCOVER, nullptr, 0);
            Serial.println("-> RTSNOW_DISCOVER");
        }
        else if (strcmp(cmd, "help") == 0)
        {
            print_help();
        }
        else
        {
            Serial.printf("Unknown command \"%s\" - try `help`\n", cmd);
        }
    }

    // ---- Serial command handling: JSON-lines interface (see the file's top comment) ----

    void handle_provision_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        const char *ssid = doc["ssid"] | "";
        const char *password = doc["password"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("provision", &doc, false, "invalid MAC address");
            return;
        }
        const char *error = do_provision(mac, ssid, password);
        send_ack("provision", &doc, error == nullptr, error);
    }

    void handle_reject_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("reject", &doc, false, "invalid MAC address");
            return;
        }
        const char *error = do_reject(mac);
        send_ack("reject", &doc, error == nullptr, error);
    }

    void handle_forget_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("forget", &doc, false, "invalid MAC address");
            return;
        }
        const char *error = do_forget(mac);
        send_ack("forget", &doc, error == nullptr, error);
    }

    // Unlike the text interface's cmd_setting, the value's type comes from
    // an explicit `valueType` field ("string"/"int"/"float"/"bool") rather
    // than being guessed - the client always knows what it means to send.
    void handle_setting_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        const char *key = doc["key"] | "";
        const char *valueType = doc["valueType"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("setting", &doc, false, "invalid MAC address");
            return;
        }
        if (strlen(key) >= sizeof(RTSNOW_SettingPayload::key))
        {
            send_ack("setting", &doc, false, "key too long");
            return;
        }

        RTSNOW_SettingPayload payload{};
        strncpy(payload.key, key, sizeof(payload.key) - 1);

        if (strcmp(valueType, "string") == 0)
        {
            const char *value = doc["value"] | "";
            if (strlen(value) >= sizeof(payload.stringValue))
            {
                send_ack("setting", &doc, false, "string value too long");
                return;
            }
            payload.valueType = RTSNOW_SETTING_STRING;
            strncpy(payload.stringValue, value, sizeof(payload.stringValue) - 1);
        }
        else if (strcmp(valueType, "int") == 0)
        {
            payload.valueType = RTSNOW_SETTING_INT32;
            payload.intValue = doc["value"] | 0;
        }
        else if (strcmp(valueType, "float") == 0)
        {
            payload.valueType = RTSNOW_SETTING_FLOAT32;
            payload.floatValue = doc["value"] | 0.0f;
        }
        else if (strcmp(valueType, "bool") == 0)
        {
            payload.valueType = RTSNOW_SETTING_BOOL;
            payload.boolValue = doc["value"] | false;
        }
        else
        {
            send_ack("setting", &doc, false, "valueType must be one of: string, int, float, bool");
            return;
        }

        const char *error = do_setting_send(mac, payload);
        send_ack("setting", &doc, error == nullptr, error);
    }

    void handle_reboot_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("reboot", &doc, false, "invalid MAC address");
            return;
        }
        const char *error = do_reboot_send(mac);
        send_ack("reboot", &doc, error == nullptr, error);
    }

    void handle_poll_registers_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("poll_registers", &doc, false, "invalid MAC address");
            return;
        }
        const char *error = do_request_registers_send(mac);
        send_ack("poll_registers", &doc, error == nullptr, error);
    }

    // ---- Firmware update over ESP-NOW: JSON command handlers ----
    // The desktop app drives the whole transfer, one of these per wire
    // message - see do_ota_*_send's own comment for why there's no
    // resend loop at this layer.

    void handle_espnow_ota_start_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("espnow_ota_start", &doc, false, "invalid MAC address");
            return;
        }
        const char *md5 = doc["md5"] | "";
        if (strlen(md5) != 32)
        {
            send_ack("espnow_ota_start", &doc, false, "md5 must be exactly 32 hex chars");
            return;
        }
        RTSNOW_OtaStart payload{};
        payload.totalSize = doc["size"] | 0;
        payload.totalChunks = doc["totalChunks"] | 0;
        strncpy(payload.md5, md5, sizeof(payload.md5) - 1);
        const char *error = do_ota_start_send(mac, payload);
        send_ack("espnow_ota_start", &doc, error == nullptr, error);
    }

    void handle_espnow_ota_chunk_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("espnow_ota_chunk", &doc, false, "invalid MAC address");
            return;
        }
        const char *hex = doc["dataHex"] | "";
        RTSNOW_OtaChunk payload{};
        payload.index = doc["index"] | 0;
        size_t decodedLen = 0;
        if (!hex_decode(hex, payload.data, sizeof(payload.data), decodedLen))
        {
            send_ack("espnow_ota_chunk", &doc, false, "invalid or oversized dataHex");
            return;
        }
        payload.length = static_cast<uint16_t>(decodedLen);
        const char *error = do_ota_chunk_send(mac, payload);
        send_ack("espnow_ota_chunk", &doc, error == nullptr, error);
    }

    void handle_espnow_ota_end_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("espnow_ota_end", &doc, false, "invalid MAC address");
            return;
        }
        const char *error = do_ota_end_send(mac);
        send_ack("espnow_ota_end", &doc, error == nullptr, error);
    }

    void handle_espnow_ota_abort_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("espnow_ota_abort", &doc, false, "invalid MAC address");
            return;
        }
        const char *error = do_ota_abort_send(mac);
        send_ack("espnow_ota_abort", &doc, error == nullptr, error);
    }

    // ---- Firmware update over TCP - this gateway being flashed directly
    // over the same JSON-lines connection a desktop app already has open
    // (see self_ota_begin()'s own comment for how this shares state with
    // the ESP-NOW receiving path above) ----

    void handle_ota_start_command(const JsonDocument &doc)
    {
        uint32_t size = doc["size"] | 0;
        uint32_t totalChunks = doc["totalChunks"] | 0;
        const char *md5 = doc["md5"] | "";
        if (strlen(md5) != 32)
        {
            send_ack("ota_start", &doc, false, "md5 must be exactly 32 hex chars");
            return;
        }
        char error[32] = "";
        bool began = self_ota_begin(size, totalChunks, md5, error, sizeof(error));
        send_ack("ota_start", &doc, began, began ? nullptr : error);
    }

    void handle_ota_chunk_command(const JsonDocument &doc)
    {
        uint32_t index = doc["index"] | 0;
        const char *hex = doc["dataHex"] | "";
        // Independent of RTSNOW_OtaChunk.data[220] (the ESP-NOW path's own
        // buffer, capped by ESP-NOW's ~250-byte payload limit) - this is a
        // separate command family carried over a real TCP socket, with no
        // radio-payload constraint at all. Matches kTcpChunkSize in
        // gateway_tcp_ota_cli.py - keep both in sync if this changes
        // (also bump TcpClientState::lineBuf's size to fit the resulting
        // hex-encoded line).
        uint8_t buf[2048];
        size_t len = 0;
        if (!hex_decode(hex, buf, sizeof(buf), len))
        {
            send_ack("ota_chunk", &doc, false, "invalid or oversized dataHex");
            return;
        }
        // Own event type (not send_ack's plain ok/error shape) so the
        // client can correlate the ack to a specific chunk index, exactly
        // like espnow_ota_chunk_ack does for the mesh path.
        JsonDocument out;
        out["event"] = "ota_chunk_ack";
        out["index"] = index;
        out["ok"] = self_ota_write_chunk(index, buf, static_cast<uint16_t>(len));
        echo_id(out, doc);
        emit_json(out);
    }

    void handle_ota_end_command(const JsonDocument &doc)
    {
        char error[32] = "";
        bool success = self_ota_end(error, sizeof(error));
        // self_ota_end() already restarted the chip on success - this ack
        // only actually reaches the client on the failure path.
        send_ack("ota_end", &doc, success, success ? nullptr : error);
    }

    void handle_ota_abort_command(const JsonDocument &doc)
    {
        self_ota_abort();
        send_ack("ota_abort", &doc, true);
    }

    void handle_channel_command(const JsonDocument &doc)
    {
        int value = doc["value"] | -1;
        if (value < RTSNOW_PROVISIONING_CHANNEL_MIN || value > RTSNOW_PROVISIONING_CHANNEL_MAX)
        {
            char error[48];
            snprintf(error, sizeof(error), "channel must be %u-%u", RTSNOW_PROVISIONING_CHANNEL_MIN, RTSNOW_PROVISIONING_CHANNEL_MAX);
            send_ack("channel", &doc, false, error);
            return;
        }
        do_channel(static_cast<uint8_t>(value));
        send_ack("channel", &doc, true);
    }

    // Debug/test-only - see s_routingOverridden's own comment. viaMac ""
    // clears the override for this target and resumes normal auto-routing;
    // any other value pins routeViaDeviceId to that relay and freezes
    // recompute_routing_if_due() globally until cleared.
    void handle_force_route_command(const JsonDocument &doc)
    {
        const char *macStr = doc["mac"] | "";
        const char *viaMacStr = doc["viaMac"] | "";
        uint8_t mac[6];
        if (!parse_mac(macStr, mac))
        {
            send_ack("force_route", &doc, false, "invalid MAC address");
            return;
        }
        int targetIdx = find_known_device_by_mac(mac);
        if (targetIdx < 0)
        {
            send_ack("force_route", &doc, false, "unknown target device");
            return;
        }
        if (strlen(viaMacStr) == 0)
        {
            s_known_devices[targetIdx].routeViaDeviceId = 0;
            s_routingOverridden = false;
            send_ack("force_route", &doc, true, "cleared - normal auto-routing resumed");
            return;
        }
        uint8_t viaMac[6];
        if (!parse_mac(viaMacStr, viaMac))
        {
            send_ack("force_route", &doc, false, "invalid viaMac address");
            return;
        }
        int relayIdx = find_known_device_by_mac(viaMac);
        if (relayIdx < 0)
        {
            send_ack("force_route", &doc, false, "unknown relay device");
            return;
        }
        s_known_devices[targetIdx].routeViaDeviceId = s_known_devices[relayIdx].deviceID;
        s_routingOverridden = true;
        send_ack("force_route", &doc, true, "forced - auto-routing frozen until cleared");
    }

    void handle_json_line(char *line)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, line);
        if (err)
        {
            send_ack("", nullptr, false, "invalid JSON");
            return;
        }

        const char *cmd = doc["cmd"] | "";
        if (strcmp(cmd, "hello") == 0)
            send_hello_event(&doc);
        else if (strcmp(cmd, "list") == 0)
            send_known_devices_event(&doc);
        else if (strcmp(cmd, "pending") == 0)
            send_pending_requests_event(&doc);
        else if (strcmp(cmd, "provision") == 0)
            handle_provision_command(doc);
        else if (strcmp(cmd, "reject") == 0)
            handle_reject_command(doc);
        else if (strcmp(cmd, "forget") == 0)
            handle_forget_command(doc);
        else if (strcmp(cmd, "setting") == 0)
            handle_setting_command(doc);
        else if (strcmp(cmd, "reboot") == 0)
            handle_reboot_command(doc);
        else if (strcmp(cmd, "poll_registers") == 0)
            handle_poll_registers_command(doc);
        else if (strcmp(cmd, "espnow_ota_start") == 0)
            handle_espnow_ota_start_command(doc);
        else if (strcmp(cmd, "espnow_ota_chunk") == 0)
            handle_espnow_ota_chunk_command(doc);
        else if (strcmp(cmd, "espnow_ota_end") == 0)
            handle_espnow_ota_end_command(doc);
        else if (strcmp(cmd, "espnow_ota_abort") == 0)
            handle_espnow_ota_abort_command(doc);
        else if (strcmp(cmd, "ota_start") == 0)
            handle_ota_start_command(doc);
        else if (strcmp(cmd, "ota_chunk") == 0)
            handle_ota_chunk_command(doc);
        else if (strcmp(cmd, "ota_end") == 0)
            handle_ota_end_command(doc);
        else if (strcmp(cmd, "ota_abort") == 0)
            handle_ota_abort_command(doc);
        else if (strcmp(cmd, "channel") == 0)
            handle_channel_command(doc);
        else if (strcmp(cmd, "force_route") == 0)
            handle_force_route_command(doc);
        else if (strcmp(cmd, "discover") == 0)
        {
            send_broadcast(RTSNOW_DISCOVER, nullptr, 0);
            send_ack("discover", &doc, true);
        }
        else
            send_ack(cmd, &doc, false, "unknown command");
    }

    void service_serial()
    {
        // Must comfortably fit an espnow_ota_chunk command line: ~80 bytes of
        // JSON overhead plus CHUNK_SIZE*2 hex chars for the data field
        // (220*2 = 440) - 192 was fine for every other command but silently
        // truncated (and lost the framing "\n" of) OTA chunk lines.
        static char lineBuf[640];
        static size_t lineLen = 0;

        while (Serial.available())
        {
            char c = static_cast<char>(Serial.read());
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                lineBuf[lineLen] = '\0';
                if (lineLen > 0)
                {
                    // A line is JSON if its first non-whitespace character
                    // is '{' - see the file's top comment.
                    char *trimmed = lineBuf;
                    while (*trimmed == ' ' || *trimmed == '\t')
                        trimmed++;
                    if (*trimmed == '{')
                        handle_json_line(trimmed);
                    else
                        handle_command(lineBuf);
                }
                lineLen = 0;
                continue;
            }
            if (lineLen + 1 < sizeof(lineBuf))
                lineBuf[lineLen++] = c;
        }
    }

    // ---- ESP-NOW receive ----

    void on_espnow_recv(const uint8_t *mac, const uint8_t *data, int len)
    {
        if (len < static_cast<int>(sizeof(RTSNOW_Header)))
            return;

        RTSNOW_Header header;
        memcpy(&header, data, sizeof(header));
        if (header.version != 1)
            return;
        if (header.sourceID == local_device_id())
            return;

        const uint8_t *payload = data + sizeof(header);
        int payloadLen = len - static_cast<int>(sizeof(header));

        if (header.messageType != RTSNOW_HEARTBEAT)
            s_rx_flash_until_ms = millis() + kTrafficFlashMs;

        switch (header.messageType)
        {
        case RTSNOW_ANNOUNCE:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_DeviceIdentity)))
                return;
            RTSNOW_DeviceIdentity identity;
            memcpy(&identity, payload, sizeof(identity));
            identity.projectName[sizeof(identity.projectName) - 1] = '\0';
            identity.deviceTypeName[sizeof(identity.deviceTypeName) - 1] = '\0';
            identity.friendlyName[sizeof(identity.friendlyName) - 1] = '\0';
            note_known_device(mac, identity);
            check_awaiting_confirm(identity.deviceID);
            break;
        }
        case RTSNOW_HEARTBEAT:
        {
            // Accepts a shorter, pre-wifiRssi/boardName/neighbors
            // RTSNOW_Heartbeat (identity + uptimeSeconds only) from
            // firmware that hasn't been rebuilt against the current
            // rtsnow_protocol.h yet - see wifiRssi's own comment for why
            // this must stay tolerant rather than rejecting the whole
            // heartbeat outright. Every trailing field is pre-set to its
            // "unknown"/empty/not-provided default below, BEFORE the
            // zero-initialized hb{} gets memcpy'd over - only actually
            // overwritten if the received payload was long enough to
            // include real bytes for it. neighborCount specifically must
            // NOT default to 0 the way hb{} alone would leave it: 0 is
            // also a real, valid "no neighbors" reading from an up-to-date
            // sender, so leaving it ambiguous with "field wasn't even
            // present" would make an older firmware's short heartbeat
            // wrongly clear out an already-known-good neighbor list (see
            // kNeighborCountNotProvided).
            constexpr int kBaseHeartbeatSize = offsetof(RTSNOW_Heartbeat, wifiRssi);
            if (payloadLen < kBaseHeartbeatSize)
                return;
            RTSNOW_Heartbeat hb{};
            hb.wifiRssi = RTSNOW_RSSI_UNKNOWN;
            hb.neighborCount = kNeighborCountNotProvided;
            size_t copyLen = static_cast<size_t>(payloadLen) < sizeof(hb) ? static_cast<size_t>(payloadLen) : sizeof(hb);
            memcpy(&hb, payload, copyLen);
            hb.identity.projectName[sizeof(hb.identity.projectName) - 1] = '\0';
            hb.identity.deviceTypeName[sizeof(hb.identity.deviceTypeName) - 1] = '\0';
            hb.identity.friendlyName[sizeof(hb.identity.friendlyName) - 1] = '\0';
            hb.boardName[sizeof(hb.boardName) - 1] = '\0';
            note_known_device(mac, hb.identity, hb.wifiRssi, hb.boardName, hb.neighborCount, hb.neighbors);
            break;
        }
        case RTSNOW_PROVISION_REQUEST:
            on_provision_request(mac, payload, payloadLen);
            break;
        case RTSNOW_SETTING_ACK:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_SettingAck)))
                return;
            RTSNOW_SettingAck ack;
            memcpy(&ack, payload, sizeof(ack));
            ack.key[sizeof(ack.key) - 1] = '\0';
            char macStr[18];
            mac_to_str(mac, macStr, sizeof(macStr));
            char logLine[96];
            snprintf(logLine, sizeof(logLine), "<- RTSNOW_SETTING_ACK \"%s\" %s from %s", ack.key, ack.accepted ? "accepted" : "rejected", macStr);
            queue_output(logLine); // called from the ESP-NOW callback - see "Deferred Serial output"

            JsonDocument doc;
            doc["event"] = "setting_ack";
            doc["mac"] = macStr;
            doc["key"] = ack.key;
            doc["accepted"] = static_cast<bool>(ack.accepted);
            emit_json(doc);
            break;
        }
        case RTSNOW_REGISTER_VALUES:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_RegisterBlock)))
                return;
            RTSNOW_RegisterBlock block;
            memcpy(&block, payload, sizeof(block));
            if (block.registerCount > 64)
                block.registerCount = 64; // defensive - a well-behaved node never sends more than this
            char macStr[18];
            mac_to_str(mac, macStr, sizeof(macStr));

            JsonDocument doc;
            doc["event"] = "register_values";
            doc["mac"] = macStr;
            doc["startRegister"] = block.startRegister;
            JsonArray values = doc["values"].to<JsonArray>();
            for (uint16_t i = 0; i < block.registerCount; i++)
                values.add(block.values[i]);
            emit_json(doc); // called from the ESP-NOW callback - see "Deferred Serial output"
            break;
        }
        case RTSNOW_OTA_START_ACK:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_OtaAck)))
                return;
            RTSNOW_OtaAck ack;
            memcpy(&ack, payload, sizeof(ack));
            ack.message[sizeof(ack.message) - 1] = '\0';
            char macStr[18];
            resolve_true_mac_str(header.sourceID, mac, macStr, sizeof(macStr));

            JsonDocument doc;
            doc["event"] = "espnow_ota_start_ack";
            doc["mac"] = macStr;
            doc["ok"] = static_cast<bool>(ack.ok);
            if (!ack.ok)
                doc["message"] = ack.message;
            emit_json(doc);
            break;
        }
        case RTSNOW_OTA_CHUNK_ACK:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_OtaChunkAck)))
                return;
            RTSNOW_OtaChunkAck ack;
            memcpy(&ack, payload, sizeof(ack));
            char macStr[18];
            resolve_true_mac_str(header.sourceID, mac, macStr, sizeof(macStr));

            JsonDocument doc;
            doc["event"] = "espnow_ota_chunk_ack";
            doc["mac"] = macStr;
            doc["index"] = ack.index;
            doc["ok"] = static_cast<bool>(ack.ok);
            emit_json(doc);
            break;
        }
        case RTSNOW_OTA_END_ACK:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_OtaAck)))
                return;
            RTSNOW_OtaAck ack;
            memcpy(&ack, payload, sizeof(ack));
            ack.message[sizeof(ack.message) - 1] = '\0';
            char macStr[18];
            resolve_true_mac_str(header.sourceID, mac, macStr, sizeof(macStr));

            JsonDocument doc;
            doc["event"] = "espnow_ota_end_ack";
            doc["mac"] = macStr;
            doc["ok"] = static_cast<bool>(ack.ok);
            if (!ack.ok)
                doc["message"] = ack.message;
            emit_json(doc);
            break;
        }
        // Firmware update over ESP-NOW, RECEIVING side - this gateway
        // itself being flashed (see service_self_ota()'s own comment).
        // Every message here just flags + copies the small fixed-size
        // payload; service_self_ota() (called from loop()) does the
        // actual Update.begin()/write()/end() calls, keeping flash access
        // out of this callback's task context - same deferred pattern
        // rtsnow_node.cpp uses for a node's own receiving side.
        case RTSNOW_OTA_START:
            if (payloadLen >= static_cast<int>(sizeof(RTSNOW_OtaStart)))
            {
                memcpy(&s_pendingSelfOtaStart, payload, sizeof(s_pendingSelfOtaStart));
                memcpy(s_pendingSelfOtaStartMac, mac, 6);
                s_pendingSelfOtaStartRequesterId = header.sourceID;
                s_selfOtaStartPending = true;
            }
            break;
        case RTSNOW_OTA_CHUNK:
            if (payloadLen >= static_cast<int>(sizeof(RTSNOW_OtaChunk)))
            {
                // Drop (don't buffer) if a previous chunk hasn't been
                // processed yet - the sender's own strict stop-and-wait
                // means this should never happen; dropping is correct for
                // a stray duplicate/retransmit that arrives after this
                // gateway already moved on.
                if (!s_selfOtaChunkPending)
                {
                    memcpy(&s_pendingSelfOtaChunk, payload, sizeof(s_pendingSelfOtaChunk));
                    memcpy(s_pendingSelfOtaChunkMac, mac, 6);
                    s_pendingSelfOtaChunkRequesterId = header.sourceID;
                    s_selfOtaChunkPending = true;
                }
            }
            break;
        case RTSNOW_OTA_END:
            memcpy(s_pendingSelfOtaEndMac, mac, 6);
            s_pendingSelfOtaEndRequesterId = header.sourceID;
            s_selfOtaEndPending = true;
            break;
        case RTSNOW_OTA_ABORT:
            s_selfOtaAbortPending = true;
            break;
        default:
            break; // not yet handled by this gateway
        }
    }

    // ---- ESP-NOW RSSI, via WiFi promiscuous sniffing ----
    //
    // esp_now_register_recv_cb's own callback (on_espnow_recv above) never
    // gets an RSSI reading - this SDK's esp_now_recv_cb_t signature is
    // still the plain (mac, data, len) one with no radio metadata
    // attached. The radio metadata (wifi_pkt_rx_ctrl_t, which does carry
    // rssi) is only available by separately registering a WiFi
    // promiscuous-mode callback, which sees every raw 802.11 frame on the
    // current channel - filtered here to WIFI_PKT_MGMT only (both by the
    // driver's own filter, set in setup(), and rechecked here) since
    // ESP-NOW rides on 802.11 vendor-specific Action frames, a management
    // subtype - and further filtered to just the ones carrying the
    // Espressif OUI (18:FE:34) in that Action frame's vendor header, so
    // this doesn't misattribute an ordinary probe/auth/assoc frame from a
    // known node's own Wi-Fi STA traffic as if it were an ESP-NOW RSSI
    // reading.
    //
    // Runs in the WiFi driver's own task context, same as
    // on_espnow_recv - see "Deferred Serial output" above for why nothing
    // here may block, allocate, or touch Serial: this is only a MAC
    // comparison against the existing known-device table plus one int8_t
    // write, deliberately kept that cheap.
    void on_wifi_promiscuous_rx(void *buf, wifi_promiscuous_pkt_type_t type)
    {
        if (type != WIFI_PKT_MGMT)
            return;

        const wifi_promiscuous_pkt_t *pkt = reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
        const uint8_t *frame = pkt->payload;
        int frameLen = pkt->rx_ctrl.sig_len;

        // 802.11 MAC header: frame control(2) + duration(2) + addr1(6) +
        // addr2(6) + addr3(6) + seq control(2) = 24 bytes, then the Action
        // frame body starts with category(1) + OUI(3).
        constexpr int kMacHeaderLen = 24;
        if (frameLen < kMacHeaderLen + 4)
            return;

        const uint8_t *body = frame + kMacHeaderLen;
        constexpr uint8_t kVendorSpecificCategory = 0x7F;
        constexpr uint8_t kEspressifOui[3] = {0x18, 0xFE, 0x34};
        if (body[0] != kVendorSpecificCategory || memcmp(body + 1, kEspressifOui, 3) != 0)
            return;

        const uint8_t *sourceMac = frame + 10; // addr2 = transmitter
        int idx = find_known_device_by_mac(sourceMac);
        if (idx >= 0)
            s_known_devices[idx].espNowRssi = pkt->rx_ctrl.rssi;
    }

    // ---- LED status ----
    // No touchscreen on this hardware (unlike the CYD) - the LED plus
    // Serial is the only status feedback available.

    void set_led(uint32_t color)
    {
        s_led.setPixelColor(0, color);
        s_led.show();
    }

    bool any_pending()
    {
        for (int i = 0; i < kMaxPendingRequests; i++)
            if (s_pending[i].inUse)
                return true;
        return false;
    }

    void update_led()
    {
        // Takes priority over everything else below - a self-OTA transfer
        // in progress (this gateway itself being flashed, whether the
        // request arrived over TCP or ESP-NOW - both funnel through the
        // same s_selfOtaActive/self_ota_begin(), see that function's own
        // comment) is the one state worth a human physically walking up
        // to check on, so it should be visually unmistakable regardless
        // of whatever else this gateway happens to be doing at that
        // moment (mesh traffic, a pending provisioning request, etc.).
        if (s_selfOtaActive)
        {
            bool blinkOn = (millis() / 150) % 2 == 0;
            set_led(blinkOn ? s_led.Color(255, 255, 255) : 0);
            return;
        }
        if (millis() < s_rx_flash_until_ms)
        {
            set_led(s_led.Color(0, 200, 0));
            return;
        }
        if (millis() < s_tx_flash_until_ms)
        {
            set_led(s_led.Color(200, 0, 0));
            return;
        }

        if (any_pending())
        {
            // Cyan blink: one or more provisioning requests awaiting an
            // operator decision - see `pending`.
            bool blinkOn = (millis() / 300) % 2 == 0;
            set_led(blinkOn ? s_led.Color(0, 150, 150) : 0);
        }
        else
        {
            set_led(s_led.Color(0, 40, 120)); // blue: idle, ready
        }
    }
}

void setup()
{
    // Must be called before begin() - default (256B) is smaller than a
    // single espnow_ota_chunk command line (~520B), risking silent RX
    // overflow if loop() is busy (e.g. mid ESP-NOW relay) when a chunk
    // line arrives.
    Serial.setRxBufferSize(1024);
    Serial.begin(115200);

    s_led.begin();
    s_led.setBrightness(255);

    Serial.println("LED self-test: white flash");
    set_led(s_led.Color(200, 200, 200));
    delay(400);
    set_led(0);
    delay(200);

    s_channel = load_channel();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    apply_channel(s_channel);

#if defined(BOARD_GATEWAY_ATOMS3_POE)
    // Ethernet-priority, WiFi fallback - never both at once. Ethernet
    // tried first since it's this board's normal, wired, PoE-powered
    // state; WiFi only attempted if that fails, so USB-free network
    // reachability survives an unplugged/dead Ethernet cable instead of
    // this unit going completely silent apart from USB serial.
    //
    // The channel-lock concern this section used to (Ethernet-only, no
    // fallback at all) avoid entirely is real, not cosmetic: the whole
    // mesh depends on every node finding this gateway on one known ESP-NOW
    // channel. A real WiFi.begin(ssid, pass) association silently re-locks
    // the radio's channel to whatever the AP uses, regardless of what
    // apply_channel(s_channel) set moments ago - fighting that (forcing
    // s_channel back) would just break the WiFi association itself (the
    // AP won't hear a STA that's been yanked off its channel). So instead
    // of fighting it, this embraces it: on a successful WiFi fallback,
    // s_channel is updated (not persisted - Ethernet may well be back next
    // boot, and the manually configured default channel should still
    // apply then) to match wherever WiFi actually landed, exactly like an
    // ordinary WiFi-connected node already does (see EspNowLink.cpp).
    s_eth_connected = gateway_ethernet_begin();
    if (!s_eth_connected)
    {
        Serial.println("Ethernet down - trying WiFi fallback...");
        WiFi.begin(GATEWAY_WIFI_SSID, GATEWAY_WIFI_PASSWORD);
        uint32_t wifiFallbackStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wifiFallbackStart < 15000)
            delay(100);
        s_wifi_connected = WiFi.status() == WL_CONNECTED;
        if (s_wifi_connected)
        {
            apply_channel(WiFi.channel());
            Serial.printf("WiFi fallback OK: SSID=%s IP=%s channel=%u\n", WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str(), s_channel);
        }
        else
        {
            Serial.println("WiFi fallback FAILED - neither transport is up; staying on the configured "
                            "channel, USB serial still works regardless.");
        }
    }
    else
    {
        Serial.println("Ethernet OK - skipping WiFi fallback (never both at once, see this section's own comment).");
    }
    begin_http_status_server();
    if (s_eth_connected)
        begin_mdns_ethernet();
    else if (s_wifi_connected)
    {
        if (MDNS.begin(kMdnsHostname))
            Serial.printf("mDNS (WiFi): responding to %s.local queries\n", kMdnsHostname);
        else
            Serial.println("mDNS (WiFi): MDNS.begin() failed");
    }
#endif

    if (esp_now_init() != ESP_OK)
    {
        Serial.println("esp_now_init() failed - halting.");
        while (true)
        {
            set_led(s_led.Color(150, 0, 0));
            delay(300);
            set_led(0);
            delay(300);
        }
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcastMac, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    esp_now_register_recv_cb(on_espnow_recv);

    // ESP-NOW RSSI capture (see on_wifi_promiscuous_rx's own comment for
    // why this needs a separate WiFi promiscuous callback rather than
    // just reading it off the normal ESP-NOW receive callback). The
    // driver-level filter here does the heavy lifting - only
    // WIFI_PKT_MGMT frames ever reach the callback at all, so ordinary
    // data traffic on this channel is never even handed to this gateway's
    // code. Coexists with normal WiFi STA state/ESP-NOW (this gateway
    // isn't itself joined to an AP - see apply_channel() above), a
    // well-established pattern for exactly this RSSI-sniffing purpose.
    wifi_promiscuous_filter_t promiscuousFilter = {};
    promiscuousFilter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&promiscuousFilter);
    esp_wifi_set_promiscuous_rx_cb(on_wifi_promiscuous_rx);
    esp_wifi_set_promiscuous(true);

    Serial.println();
    Serial.println("=== RTS ESP-NOW Gateway ===");
    Serial.printf("Local gateway deviceID: 0x%08X\n", local_device_id());
    Serial.printf("Operating on ESP-NOW channel %u (change with `channel <n>`)\n", s_channel);
    print_help();

    send_hello_event(nullptr);

    update_led();
}

void loop()
{
    service_serial();
#if defined(BOARD_GATEWAY_ATOMS3_POE)
    if (s_eth_connected)
        Ethernet.maintain();  // renews the DHCP lease as needed; no-op otherwise
    poll_tcp_server();
    poll_http_status_server();
    if (s_eth_connected)
        poll_mdns_ethernet();
    // WiFi's own mDNS (ESPmDNS/esp-idf mdns component) runs its own
    // background task once MDNS.begin() succeeds - no poll needed here.
#endif
    expire_pending();
    expire_awaiting_confirm();
    recompute_routing_if_due();
    send_gateway_announce_if_due();
    service_self_ota();
    drain_output_queue();
    update_led();
    delay(20);
}
