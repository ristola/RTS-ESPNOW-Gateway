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
#include <esp_now.h>
#include <esp_wifi.h>

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

    void send_gateway_announce_if_due()
    {
        uint32_t now = millis();
        if (now - s_last_gateway_announce_ms < kGatewayAnnounceIntervalMs)
            return;
        s_last_gateway_announce_ms = now;

        RTSNOW_DeviceIdentity id{};
        id.deviceID = local_device_id();
        strncpy(id.projectName, "RTSNow", sizeof(id.projectName) - 1);
        strncpy(id.deviceTypeName, "Gateway", sizeof(id.deviceTypeName) - 1);
        strncpy(id.friendlyName, "Gateway", sizeof(id.friendlyName) - 1);
        id.firmwareVersionMajor = 1;
        id.ipv4Address = 0; // this gateway has no IP of its own in the RTS-NOW sense - it's the ESP-NOW side, not a node with its own Modbus/HTTP surface
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
    expire_pending();
    expire_awaiting_confirm();
    recompute_routing_if_due();
    send_gateway_announce_if_due();
    drain_output_queue();
    update_led();
    delay(20);
}
