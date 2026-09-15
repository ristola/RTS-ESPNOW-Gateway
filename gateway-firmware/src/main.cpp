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

    bool send_to(const uint8_t mac[6], RTSNOW_MessageType type, const void *payload, uint16_t payloadLen)
    {
        uint8_t buf[250];
        if (sizeof(RTSNOW_Header) + payloadLen > sizeof(buf))
            return false;

        RTSNOW_Header header;
        header.version = 1;
        header.messageType = type;
        header.sourceID = local_device_id();
        header.destinationID = 0xFFFFFFFF; // recipient identity isn't tracked by MAC-only messages
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

    void note_known_device(const uint8_t mac[6], const RTSNOW_DeviceIdentity &identity)
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
        emit_json(doc);
    }

    int find_known_device_by_mac(const uint8_t mac[6])
    {
        for (int i = 0; i < s_known_device_count; i++)
            if (memcmp(s_known_devices[i].mac, mac, 6) == 0)
                return i;
        return -1;
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

    // Sends an already-built setting payload (the text and JSON interfaces
    // each build one their own way - see cmd_setting and handle_setting_command).
    const char *do_setting_send(const uint8_t mac[6], const RTSNOW_SettingPayload &payload)
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        ensure_peer(mac);
        send_to(mac, RTSNOW_SET_SETTING, &payload, sizeof(payload));
        return nullptr;
    }

    // Both reboot and register-request are fire-and-forget from the
    // gateway's point of view, same caveat as do_setting_send above: a
    // successful send here only means the ESP-NOW packet went out, not that
    // the node received it. See PROTOCOL.md's "Remote control" section for
    // how each side's real confirmation actually arrives (RTSNOW_ANNOUNCE
    // for reboot, RTSNOW_REGISTER_VALUES for a register request).
    //
    // Sent kRemoteControlResendCount times rather than once - real-hardware
    // testing during this feature's development hit a meaningfully high
    // single-shot loss rate on this link (a single reboot or register
    // request going completely unanswered was common, not rare), and
    // there's no ack/retry anywhere in this protocol layer to fall back on.
    // A few resends a beat apart is the same "cheap insurance" tradeoff
    // rtsnow_node.cpp's own initial RTSNOW_ANNOUNCE already makes (sent
    // twice on boot) - harmless if the node is duplicate-tolerant, which
    // both RTSNOW_REBOOT (a single boolean flag) and
    // RTSNOW_REQUEST_REGISTERS (each resend just gets its own independent
    // reply) are.
    constexpr uint8_t kRemoteControlResendCount = 3;
    constexpr uint32_t kRemoteControlResendGapMs = 40;

    const char *do_reboot_send(const uint8_t mac[6])
    {
        int idx = find_known_device_by_mac(mac);
        if (idx < 0)
            return "unknown MAC - it must have announced/heartbeated at least once (see `list`)";
        ensure_peer(mac);
        for (uint8_t i = 0; i < kRemoteControlResendCount; i++)
        {
            send_to(mac, RTSNOW_REBOOT, nullptr, 0);
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
        ensure_peer(mac);
        for (uint8_t i = 0; i < kRemoteControlResendCount; i++)
        {
            send_to(mac, RTSNOW_REQUEST_REGISTERS, nullptr, 0);
            if (i + 1 < kRemoteControlResendCount)
                delay(kRemoteControlResendGapMs);
        }
        return nullptr;
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
        else if (strcmp(cmd, "setting") == 0)
            handle_setting_command(doc);
        else if (strcmp(cmd, "reboot") == 0)
            handle_reboot_command(doc);
        else if (strcmp(cmd, "poll_registers") == 0)
            handle_poll_registers_command(doc);
        else if (strcmp(cmd, "channel") == 0)
            handle_channel_command(doc);
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
        static char lineBuf[192];
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
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_Heartbeat)))
                return;
            RTSNOW_Heartbeat hb;
            memcpy(&hb, payload, sizeof(hb));
            hb.identity.projectName[sizeof(hb.identity.projectName) - 1] = '\0';
            hb.identity.deviceTypeName[sizeof(hb.identity.deviceTypeName) - 1] = '\0';
            hb.identity.friendlyName[sizeof(hb.identity.friendlyName) - 1] = '\0';
            note_known_device(mac, hb.identity);
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
        default:
            break; // not yet handled by this gateway
        }
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
    drain_output_queue();
    update_led();
    delay(20);
}
