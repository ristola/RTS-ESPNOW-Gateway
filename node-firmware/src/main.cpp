// RTS ESP-NOW Gateway - minimal reference/test node firmware.
//
// Exists to test ../gateway-firmware/'s provisioning handshake and
// settings push end-to-end against real second-device hardware (see
// ../PROTOCOL.md's "Status" section and ../TASKS.md) - NOT a real
// project's production node. A real project (DryerHealth's dryer nodes,
// a future CYD-4.3-FN-Tester FN pod speaking RTSNOW_, etc.) would start
// from this shape but replace the identity fields and add its own real
// sensor/actuator logic.
//
// Two node modes, chosen per build via kNeedsWifi below - "when there's no
// Wi-Fi network available, we depend on the RTS-NOW network" (an explicit
// requirement: some deployments have no Wi-Fi to hand out at all, so a
// node must still be able to power up, announce itself, and be recognized
// by the gateway over ESP-NOW alone):
//
// - kNeedsWifi = true - the original flow. State machine: SWEEPING (no
//   Wi-Fi credentials yet, broadcasting RTSNOW_PROVISION_REQUEST while
//   cycling channels 1-11) -> HELD (a gateway replied
//   RTSNOW_PROVISION_HOLD, so stop sweeping and wait) -> JOINING (received
//   RTSNOW_PROVISION_CREDENTIALS, attempting WiFi.begin) -> JOINED
//   (connected; broadcasts RTSNOW_ANNOUNCE once, then periodic
//   RTSNOW_HEARTBEAT). Wi-Fi credentials persist in NVS across reboots.
// - kNeedsWifi = false - mesh-only pairing. No Wi-Fi is ever attempted:
//   the node broadcasts RTSNOW_ANNOUNCE (ipv4Address=0) immediately at
//   boot and enters PAIRED, then behaves exactly like JOINED for
//   everything else (periodic RTSNOW_HEARTBEAT, accepts
//   RTSNOW_SET_SETTING). No new wire message for this - the gateway
//   already auto-adds any ANNOUNCE/HEARTBEAT sender to its known-devices
//   table with zero operator approval (see PROTOCOL.md's device identity
//   section), so "pairing" here is just choosing to send that broadcast
//   right away instead of only after a Wi-Fi join.
//
// Either way: use the `forget` serial command, or hold the physical
// button (GPIO41) for 5 seconds, to erase any stored Wi-Fi credentials
// and reboot - the fastest way to test re-pairing/re-provisioning from a
// clean state without reflashing.
//
// OTA (kNeedsWifi=true only - a mesh-only/Paired node has no network to
// receive an update over): standard ArduinoOTA over Wi-Fi, once JOINED.
// This board's default partition table (m5stack-atoms3's default.csv)
// already has otadata/app0/app1 - no partition changes needed. OTA over
// ESP-NOW itself (for mesh-only nodes with no Wi-Fi) was discussed and
// deliberately deferred - out of scope for this pass.

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>

#include "rtsnow_protocol.h"

namespace
{
    // The one thing that distinguishes this test node's two build modes -
    // see the file's top comment. Flip and reflash to test the other path.
    constexpr bool kNeedsWifi = false;

    constexpr uint8_t kRgbLedPin = 35;
    constexpr uint8_t kButtonPin = 41; // onboard button - held 5s = factory reset

    constexpr const char *kPrefsNamespace = "rtsnow_node";
    constexpr const char *kSsidKey = "ssid";
    constexpr const char *kPasswordKey = "password";
    constexpr const char *kFriendlyNameKey = "friendlyName";

    constexpr uint32_t kDwellMs = 400;           // time spent on each channel while sweeping
    constexpr uint32_t kHoldTimeoutMs = 15000;   // give up waiting on a hold and resume sweeping
    constexpr uint32_t kJoinTimeoutMs = 15000;   // give up on WiFi.begin() and resume sweeping
    constexpr uint32_t kHeartbeatIntervalMs = 10000;
    constexpr uint32_t kFactoryResetHoldMs = 5000;

    const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    constexpr uint16_t kLedCount = 1;
    Adafruit_NeoPixel s_led(kLedCount, kRgbLedPin, NEO_GRB + NEO_KHZ800);

    enum class State
    {
        Sweeping,
        Held,
        Joining,
        Joined,
        Paired, // mesh-only equivalent of Joined - see kNeedsWifi
    };

    State s_state = State::Sweeping;
    uint8_t s_channel = RTSNOW_PROVISIONING_CHANNEL_MIN;
    uint32_t s_nonce = 0;
    uint16_t s_next_sequence = 0;

    uint32_t s_channel_dwell_until_ms = 0;
    uint32_t s_hold_until_ms = 0;
    uint32_t s_join_timeout_ms = 0;
    uint32_t s_last_heartbeat_ms = 0;
    uint32_t s_last_join_status_log_ms = 0;

    uint32_t s_button_press_started_ms = 0;
    bool s_button_was_pressed = false;

    bool s_ota_begun = false;      // ArduinoOTA.begin() called once per Wi-Fi join
    bool s_ota_in_progress = false;

    bool s_pending_setting_ack = false;
    uint8_t s_pending_setting_ack_mac[6];
    char s_pending_setting_ack_key[24];

    // "friendlyName" is a reserved setting key (see PROTOCOL.md's "Generic
    // settings" section) this firmware specifically recognizes: renames
    // the device and persists it in NVS, surviving reboots. Deferred to
    // loop() rather than applied directly in on_espnow_recv, same
    // reasoning as s_pending_setting_ack.
    char s_friendly_name[24] = "";
    bool s_has_pending_rename = false;
    char s_pending_rename[24];

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
        header.destinationID = 0xFFFFFFFF;
        header.sequence = s_next_sequence++;
        header.payloadLength = payloadLen;

        memcpy(buf, &header, sizeof(header));
        if (payloadLen > 0)
            memcpy(buf + sizeof(header), payload, payloadLen);

        ensure_peer(mac);
        return esp_now_send(mac, buf, sizeof(header) + payloadLen) == ESP_OK;
    }

    bool send_broadcast(RTSNOW_MessageType type, const void *payload, uint16_t payloadLen)
    {
        return send_to(kBroadcastMac, type, payload, payloadLen);
    }

    RTSNOW_DeviceIdentity make_identity()
    {
        RTSNOW_DeviceIdentity id{};
        id.deviceID = local_device_id();
        // Not "GatewayTest" (the original name here) - that read as if
        // these nodes were themselves gateways in the desktop app's
        // device table, which they aren't; only the AtomS3U is the
        // gateway.
        strncpy(id.projectName, "RTSNOW-Test", sizeof(id.projectName) - 1);
        strncpy(id.deviceTypeName, "TestNode", sizeof(id.deviceTypeName) - 1);
        strncpy(id.friendlyName, s_friendly_name, sizeof(id.friendlyName) - 1);
        id.firmwareVersionMajor = 0;
        id.firmwareVersionMinor = 1;
        id.firmwareVersionPatch = 0;
        id.ipv4Address = (WiFi.status() == WL_CONNECTED) ? static_cast<uint32_t>(WiFi.localIP()) : 0;
        return id;
    }

    void apply_channel(uint8_t channel)
    {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }

    void save_credentials(const char *ssid, const char *password)
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/false);
        prefs.putString(kSsidKey, ssid);
        prefs.putString(kPasswordKey, password);
        prefs.end();
    }

    bool load_credentials(char *ssidOut, size_t ssidSize, char *passwordOut, size_t passwordSize)
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/true);
        String ssid = prefs.getString(kSsidKey, "");
        String password = prefs.getString(kPasswordKey, "");
        prefs.end();
        if (ssid.length() == 0)
            return false;
        strncpy(ssidOut, ssid.c_str(), ssidSize - 1);
        ssidOut[ssidSize - 1] = '\0';
        strncpy(passwordOut, password.c_str(), passwordSize - 1);
        passwordOut[passwordSize - 1] = '\0';
        return true;
    }

    void forget_credentials()
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/false);
        prefs.clear(); // also clears any custom friendlyName - a factory reset reverts the name too
        prefs.end();
    }

    void save_friendly_name(const char *name)
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/false);
        prefs.putString(kFriendlyNameKey, name);
        prefs.end();
    }

    // Loads a previously-saved custom name, or derives the original
    // default ("TestNode-XXXX", low 16 bits of deviceID) if none was ever
    // set - called once at boot.
    void load_friendly_name()
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/true);
        String stored = prefs.getString(kFriendlyNameKey, "");
        prefs.end();

        if (stored.length() > 0)
        {
            strncpy(s_friendly_name, stored.c_str(), sizeof(s_friendly_name) - 1);
            s_friendly_name[sizeof(s_friendly_name) - 1] = '\0';
        }
        else
        {
            snprintf(s_friendly_name, sizeof(s_friendly_name), "TestNode-%04X", static_cast<unsigned>(local_device_id() & 0xFFFF));
        }
    }

    void send_provision_request()
    {
        RTSNOW_ProvisionRequest req{};
        req.identity = make_identity();
        req.nonce = s_nonce;
        send_broadcast(RTSNOW_PROVISION_REQUEST, &req, sizeof(req));
        Serial.printf("Sweeping: channel %u - broadcast RTSNOW_PROVISION_REQUEST (nonce=0x%08X)\n", s_channel, s_nonce);
    }

    void resume_sweeping()
    {
        s_state = State::Sweeping;
        s_nonce = esp_random();
        s_channel = RTSNOW_PROVISIONING_CHANNEL_MIN;
        apply_channel(s_channel);
        send_provision_request();
        s_channel_dwell_until_ms = millis() + kDwellMs;
    }

    void start_joining(const char *ssid, const char *password)
    {
        Serial.printf("Received credentials for SSID \"%s\" - joining...\n", ssid);
        save_credentials(ssid, password);
        s_state = State::Joining;
        WiFi.begin(ssid, password);
        s_join_timeout_ms = millis() + kJoinTimeoutMs;
    }

    // Standard Wi-Fi OTA (ArduinoOTA) - only reachable once JOINED, so
    // only relevant for kNeedsWifi=true nodes. Called once per join;
    // s_ota_begun guards against re-registering callbacks/re-binding the
    // OTA port if the node reconnects after a brief Wi-Fi drop.
    void begin_ota()
    {
        if (s_ota_begun)
            return;
        s_ota_begun = true;

        char hostname[32];
        snprintf(hostname, sizeof(hostname), "rtsnow-node-%08x", local_device_id());
        ArduinoOTA.setHostname(hostname);

        ArduinoOTA.onStart([]()
                            {
            s_ota_in_progress = true;
            Serial.println("OTA: starting"); });
        ArduinoOTA.onEnd([]()
                          {
            s_ota_in_progress = false;
            Serial.println("OTA: complete - rebooting"); });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                               { Serial.printf("OTA: %u%%\n", (progress * 100) / total); });
        ArduinoOTA.onError([](ota_error_t error)
                            {
            s_ota_in_progress = false;
            Serial.printf("OTA: error (code %u) - resuming normal operation\n", error); });

        ArduinoOTA.begin();
        Serial.printf("OTA ready - hostname \"%s\"\n", hostname);
    }

    void on_join_success()
    {
        s_state = State::Joined;
        Serial.printf("Joined Wi-Fi - IP=%s channel=%u\n", WiFi.localIP().toString().c_str(), WiFi.channel());
        begin_ota();

        RTSNOW_DeviceIdentity id = make_identity();
        send_broadcast(RTSNOW_ANNOUNCE, &id, sizeof(id));
        delay(50);
        send_broadcast(RTSNOW_ANNOUNCE, &id, sizeof(id)); // sent twice - broadcast is reliable but cheap insurance during a channel transition

        s_last_heartbeat_ms = millis();
    }

    // kNeedsWifi=false's entire "provisioning" - no Wi-Fi, no gateway
    // round-trip needed. RTSNOW_ANNOUNCE (ipv4Address=0) is enough: the
    // gateway already auto-adds any announcing device to its known-devices
    // table with no operator approval - see the file's top comment.
    void enter_paired_mesh_only()
    {
        s_state = State::Paired;
        Serial.println("Mesh-only pairing: broadcasting RTSNOW_ANNOUNCE (no Wi-Fi)");

        RTSNOW_DeviceIdentity id = make_identity();
        send_broadcast(RTSNOW_ANNOUNCE, &id, sizeof(id));
        delay(50);
        send_broadcast(RTSNOW_ANNOUNCE, &id, sizeof(id));

        s_last_heartbeat_ms = millis();
    }

    void send_heartbeat()
    {
        RTSNOW_Heartbeat hb{};
        hb.identity = make_identity();
        hb.uptimeSeconds = millis() / 1000;
        send_broadcast(RTSNOW_HEARTBEAT, &hb, sizeof(hb));
    }

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

        switch (header.messageType)
        {
        case RTSNOW_PROVISION_HOLD:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_ProvisionNonce)) || s_state != State::Sweeping)
                return;
            RTSNOW_ProvisionNonce hold;
            memcpy(&hold, payload, sizeof(hold));
            if (hold.nonce != s_nonce)
                return;
            s_state = State::Held;
            s_hold_until_ms = millis() + kHoldTimeoutMs;
            Serial.printf("Held on channel %u - awaiting credentials or rejection\n", s_channel);
            break;
        }
        case RTSNOW_PROVISION_CREDENTIALS:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_WifiCredentials)))
                return;
            RTSNOW_WifiCredentials creds;
            memcpy(&creds, payload, sizeof(creds));
            if (creds.nonce != s_nonce)
                return;
            creds.ssid[sizeof(creds.ssid) - 1] = '\0';
            creds.password[sizeof(creds.password) - 1] = '\0';
            start_joining(creds.ssid, creds.password);
            break;
        }
        case RTSNOW_PROVISION_REJECTED:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_ProvisionNonce)))
                return;
            RTSNOW_ProvisionNonce rej;
            memcpy(&rej, payload, sizeof(rej));
            if (rej.nonce != s_nonce)
                return;
            Serial.println("Rejected by gateway - resuming sweep");
            resume_sweeping();
            break;
        }
        case RTSNOW_SET_SETTING:
        {
            if (payloadLen < static_cast<int>(sizeof(RTSNOW_SettingPayload)))
                return;
            RTSNOW_SettingPayload setting;
            memcpy(&setting, payload, sizeof(setting));
            setting.key[sizeof(setting.key) - 1] = '\0';
            char macStr[18];
            mac_to_str(mac, macStr, sizeof(macStr));
            switch (setting.valueType)
            {
            case RTSNOW_SETTING_STRING:
                setting.stringValue[sizeof(setting.stringValue) - 1] = '\0';
                Serial.printf("Setting from %s: \"%s\" = \"%s\" (string)\n", macStr, setting.key, setting.stringValue);
                break;
            case RTSNOW_SETTING_INT32:
                Serial.printf("Setting from %s: \"%s\" = %d (int)\n", macStr, setting.key, setting.intValue);
                break;
            case RTSNOW_SETTING_FLOAT32:
                Serial.printf("Setting from %s: \"%s\" = %f (float)\n", macStr, setting.key, setting.floatValue);
                break;
            case RTSNOW_SETTING_BOOL:
                Serial.printf("Setting from %s: \"%s\" = %s (bool)\n", macStr, setting.key, setting.boolValue ? "true" : "false");
                break;
            default:
                Serial.printf("Setting from %s: \"%s\" (unknown valueType %u)\n", macStr, setting.key, setting.valueType);
                break;
            }

            // Deferred to loop() rather than sent here - see PROTOCOL.md's
            // "Why broadcast, not ACK": a unicast reply sent synchronously
            // from inside an ESP-NOW receive callback was the exact
            // pattern found unreliable on real ESP32-S3 hardware.
            memcpy(s_pending_setting_ack_mac, mac, 6);
            strncpy(s_pending_setting_ack_key, setting.key, sizeof(s_pending_setting_ack_key) - 1);
            s_pending_setting_ack_key[sizeof(s_pending_setting_ack_key) - 1] = '\0';
            s_pending_setting_ack = true;

            // "friendlyName" is the one reserved setting key this
            // firmware actually applies (see PROTOCOL.md) - queued the
            // same way, applied (and saved to NVS) from loop().
            if (setting.valueType == RTSNOW_SETTING_STRING && strcmp(setting.key, "friendlyName") == 0)
            {
                strncpy(s_pending_rename, setting.stringValue, sizeof(s_pending_rename) - 1);
                s_pending_rename[sizeof(s_pending_rename) - 1] = '\0';
                s_has_pending_rename = true;
            }
            break;
        }
        default:
            break;
        }
    }

    void update_led()
    {
        // OTA takes priority over everything else, including a button
        // hold - a fast white blink, distinct from every other state color
        // this firmware uses, so it's unmistakable that a flash is in
        // progress (don't power-cycle, don't hold the button, etc.).
        if (s_ota_in_progress)
        {
            bool blinkOn = (millis() / 100) % 2 == 0;
            s_led.setPixelColor(0, blinkOn ? s_led.Color(255, 255, 255) : 0);
            s_led.show();
            return;
        }

        // Button-hold feedback takes priority over the normal state color -
        // solid yellow as soon as a press starts, so there's no ambiguity
        // about whether the button registered.
        if (s_button_was_pressed && s_button_press_started_ms != 0)
        {
            s_led.setPixelColor(0, s_led.Color(200, 160, 0));
            s_led.show();
            return;
        }

        uint32_t color;
        switch (s_state)
        {
        case State::Sweeping:
        {
            bool blinkOn = (millis() / 300) % 2 == 0;
            color = blinkOn ? s_led.Color(0, 0, 120) : 0;
            break;
        }
        case State::Held:
            color = s_led.Color(0, 150, 150);
            break;
        case State::Joining:
            color = s_led.Color(150, 130, 0);
            break;
        case State::Joined:
            color = s_led.Color(0, 150, 0);
            break;
        case State::Paired:
            color = s_led.Color(0, 0, 200); // paired over the mesh (no Wi-Fi) - solid blue, distinct from Sweeping's dim blinking blue
            break;
        default:
            color = 0;
            break;
        }
        s_led.setPixelColor(0, color);
        s_led.show();
    }

    void print_status()
    {
        const char *stateName = "?";
        switch (s_state)
        {
        case State::Sweeping: stateName = "SWEEPING"; break;
        case State::Held: stateName = "HELD"; break;
        case State::Joining: stateName = "JOINING"; break;
        case State::Joined: stateName = "JOINED"; break;
        case State::Paired: stateName = "PAIRED (mesh-only)"; break;
        }
        Serial.printf("state=%s channel=%u nonce=0x%08X deviceID=0x%08X\n", stateName, s_channel, s_nonce, local_device_id());
        if (s_state == State::Joined)
            Serial.printf("  Wi-Fi: SSID=\"%s\" IP=%s channel=%u\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.channel());
    }

    // Onboard button (GPIO41) held 5s = factory reset: erase any stored
    // Wi-Fi credentials and reboot. Works in either build mode - for
    // kNeedsWifi=false there's nothing stored to erase, but the reboot
    // itself re-triggers mesh-only pairing, which is exactly what testing
    // re-pairing needs.
    void check_factory_reset_button()
    {
        if (s_ota_in_progress)
            return; // don't let a stray press interrupt/reboot mid-flash

        bool pressed = (digitalRead(kButtonPin) == LOW);

        if (pressed && !s_button_was_pressed)
        {
            s_button_press_started_ms = millis();
        }
        else if (!pressed && s_button_was_pressed)
        {
            s_button_press_started_ms = 0; // released early - cancel
        }
        s_button_was_pressed = pressed;

        if (pressed && s_button_press_started_ms != 0 && millis() - s_button_press_started_ms >= kFactoryResetHoldMs)
        {
            Serial.println("Button held 5s - factory reset, rebooting...");
            for (int i = 0; i < 3; i++)
            {
                s_led.setPixelColor(0, s_led.Color(255, 255, 255));
                s_led.show();
                delay(100);
                s_led.setPixelColor(0, 0);
                s_led.show();
                delay(100);
            }
            forget_credentials();
            delay(200);
            ESP.restart();
        }
    }

    void handle_command(char *line)
    {
        char *cmd = strtok(line, " ");
        if (cmd == nullptr)
            return;

        if (strcmp(cmd, "status") == 0)
        {
            print_status();
        }
        else if (strcmp(cmd, "forget") == 0)
        {
            forget_credentials();
            Serial.println("Credentials erased - rebooting...");
            delay(200);
            ESP.restart();
        }
        else if (strcmp(cmd, "help") == 0)
        {
            Serial.println("Commands: status | forget (erase Wi-Fi creds + reboot) | help");
        }
        else
        {
            Serial.printf("Unknown command \"%s\" - try `help`\n", cmd);
        }
    }

    void service_serial()
    {
        static char lineBuf[128];
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
                    handle_command(lineBuf);
                lineLen = 0;
                continue;
            }
            if (lineLen + 1 < sizeof(lineBuf))
                lineBuf[lineLen++] = c;
        }
    }
}

void setup()
{
    Serial.begin(115200);
    s_led.begin();
    s_led.setBrightness(255);
    pinMode(kButtonPin, INPUT_PULLUP);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK)
    {
        Serial.println("esp_now_init() failed - halting.");
        while (true)
        {
            s_led.setPixelColor(0, s_led.Color(150, 0, 0));
            s_led.show();
            delay(300);
            s_led.setPixelColor(0, 0);
            s_led.show();
            delay(300);
        }
    }
    esp_now_register_recv_cb(on_espnow_recv);
    load_friendly_name();

    Serial.println();
    Serial.println("=== RTS ESP-NOW Test Node ===");
    Serial.printf("Local deviceID: 0x%08X\n", local_device_id());
    Serial.printf("Friendly name: %s\n", s_friendly_name);
    Serial.printf("Mode: %s\n", kNeedsWifi ? "Wi-Fi (needs provisioning)" : "mesh-only (no Wi-Fi)");
    Serial.println("Commands: status | forget | help");
    Serial.println("Hold the button 5s for a factory reset (erase creds + reboot).");

    if (!kNeedsWifi)
    {
        enter_paired_mesh_only();
        return;
    }

    char ssid[32];
    char password[64];
    if (load_credentials(ssid, sizeof(ssid), password, sizeof(password)))
    {
        Serial.printf("Stored credentials found (SSID \"%s\") - joining directly...\n", ssid);
        s_state = State::Joining;
        WiFi.begin(ssid, password);
        s_join_timeout_ms = millis() + kJoinTimeoutMs;
    }
    else
    {
        resume_sweeping();
    }
}

void loop()
{
    service_serial();
    uint32_t now = millis();

    switch (s_state)
    {
    case State::Sweeping:
        if (now >= s_channel_dwell_until_ms)
        {
            s_channel = (s_channel % RTSNOW_PROVISIONING_CHANNEL_MAX) + 1;
            apply_channel(s_channel);
            send_provision_request();
            s_channel_dwell_until_ms = now + kDwellMs;
        }
        break;
    case State::Held:
        if (now >= s_hold_until_ms)
        {
            Serial.println("Hold timed out - resuming sweep");
            resume_sweeping();
        }
        break;
    case State::Joining:
        if (WiFi.status() == WL_CONNECTED)
        {
            on_join_success();
        }
        else if (now >= s_join_timeout_ms)
        {
            Serial.printf("Join attempt failed/timed out (WiFi.status()=%d) - resuming sweep\n", WiFi.status());
            resume_sweeping();
        }
        else if (now - s_last_join_status_log_ms >= 1000)
        {
            Serial.printf("  ...joining, WiFi.status()=%d\n", WiFi.status());
            s_last_join_status_log_ms = now;
        }
        break;
    case State::Joined:
        ArduinoOTA.handle();
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("Lost Wi-Fi connection - resuming sweep");
            resume_sweeping();
        }
        else if (now - s_last_heartbeat_ms >= kHeartbeatIntervalMs)
        {
            send_heartbeat();
            s_last_heartbeat_ms = now;
        }
        break;
    case State::Paired:
        if (now - s_last_heartbeat_ms >= kHeartbeatIntervalMs)
        {
            send_heartbeat();
            s_last_heartbeat_ms = now;
        }
        break;
    }

    if (s_pending_setting_ack)
    {
        RTSNOW_SettingAck ack{};
        strncpy(ack.key, s_pending_setting_ack_key, sizeof(ack.key) - 1);
        ack.accepted = 1;
        send_to(s_pending_setting_ack_mac, RTSNOW_SETTING_ACK, &ack, sizeof(ack));
        s_pending_setting_ack = false;
    }

    if (s_has_pending_rename)
    {
        strncpy(s_friendly_name, s_pending_rename, sizeof(s_friendly_name) - 1);
        s_friendly_name[sizeof(s_friendly_name) - 1] = '\0';
        save_friendly_name(s_friendly_name);
        Serial.printf("Renamed to \"%s\" (saved)\n", s_friendly_name);
        s_has_pending_rename = false;
    }

    check_factory_reset_button();
    update_led();
    delay(20);
}
