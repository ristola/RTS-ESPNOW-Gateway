// See rtsnow_node.h for the usage contract and rationale.
//
// Important operational caveat, inherited from every project that uses
// this helper: ESP-NOW rides whatever Wi-Fi channel this device's STA is
// currently on. If that's chosen by something outside our control (a
// project's own router, WiFiManager, etc.), RTS-ESPNOW-Gateway must be
// manually set (its `channel` command/desktop-app control) to that same
// channel, or it will never see this device's broadcasts. Mesh-only
// nodes with no Wi-Fi at all instead settle on whatever channel
// esp_now_init() leaves WiFi.mode(WIFI_STA) on (channel 1 unless changed).

#include "rtsnow_node.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>
#include <esp_now.h>

#include "rtsnow_protocol.h"

namespace
{
    constexpr const char *kPrefsNamespace = "rtsnow_node";
    constexpr const char *kFriendlyNameKey = "friendlyName";
    const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    RTSNowNodeConfig s_config;
    uint16_t s_nextSequence = 0;
    uint32_t s_lastHeartbeatMs = 0;
    bool s_began = false; // guards rtsnowNodeLoop() before rtsnowNodeBegin() has run

    char s_friendlyName[24] = "";

    // See gateway-firmware's own main.cpp "Deferred Serial output" comment
    // for why: esp_now_register_recv_cb's callback runs in the Wi-Fi
    // driver's own task, not loop()'s - acting on (or even just logging)
    // a received setting directly from inside it risks the same
    // corruption/unreliable-delivery issues documented there. Only
    // loop() ever acts on a received setting.
    bool s_pendingSettingAck = false;
    uint8_t s_pendingSettingAckMac[6];
    RTSNOW_SettingPayload s_pendingSetting;

    bool s_hasPendingRename = false;
    char s_pendingRename[24];

    // Same "only loop() acts, the receive callback only flags" discipline
    // as the setting-ack/rename handling above.
    bool s_pendingReboot = false;

    bool s_pendingRegisterRequest = false;
    uint8_t s_pendingRegisterRequestMac[6];

    uint32_t localDeviceId()
    {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        return (static_cast<uint32_t>(mac[2]) << 24) | (static_cast<uint32_t>(mac[3]) << 16) |
               (static_cast<uint32_t>(mac[4]) << 8) | static_cast<uint32_t>(mac[5]);
    }

    void ensurePeer(const uint8_t mac[6])
    {
        if (esp_now_is_peer_exist(mac))
            return;
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = 0; // 0 = use the current channel, whatever this device's Wi-Fi STA is on
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    bool sendTo(const uint8_t mac[6], RTSNOW_MessageType type, const void *payload, uint16_t payloadLen)
    {
        uint8_t buf[250];
        if (sizeof(RTSNOW_Header) + payloadLen > sizeof(buf))
            return false;

        RTSNOW_Header header;
        header.version = 1;
        header.messageType = type;
        header.sourceID = localDeviceId();
        header.destinationID = 0xFFFFFFFF;
        header.sequence = s_nextSequence++;
        header.payloadLength = payloadLen;

        memcpy(buf, &header, sizeof(header));
        if (payloadLen > 0)
            memcpy(buf + sizeof(header), payload, payloadLen);

        ensurePeer(mac);
        return esp_now_send(mac, buf, sizeof(header) + payloadLen) == ESP_OK;
    }

    bool sendBroadcast(RTSNOW_MessageType type, const void *payload, uint16_t payloadLen)
    {
        return sendTo(kBroadcastMac, type, payload, payloadLen);
    }

    void saveFriendlyName(const char *name)
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/false);
        prefs.putString(kFriendlyNameKey, name);
        prefs.end();
    }

    // Loads a previously-saved custom name, or falls back to the config's
    // defaultFriendlyName if none was ever set - called once from
    // rtsnowNodeBegin().
    void loadFriendlyName()
    {
        Preferences prefs;
        prefs.begin(kPrefsNamespace, /*readOnly=*/true);
        String stored = prefs.getString(kFriendlyNameKey, "");
        prefs.end();

        const char *fallback = stored.length() > 0 ? stored.c_str() : s_config.defaultFriendlyName;
        strncpy(s_friendlyName, fallback, sizeof(s_friendlyName) - 1);
        s_friendlyName[sizeof(s_friendlyName) - 1] = '\0';
    }

    RTSNOW_DeviceIdentity makeIdentity()
    {
        RTSNOW_DeviceIdentity id{};
        id.deviceID = localDeviceId();
        strncpy(id.projectName, s_config.projectName, sizeof(id.projectName) - 1);
        strncpy(id.deviceTypeName, s_config.deviceTypeName, sizeof(id.deviceTypeName) - 1);
        strncpy(id.friendlyName, s_friendlyName, sizeof(id.friendlyName) - 1);
        id.firmwareVersionMajor = s_config.firmwareVersionMajor;
        id.firmwareVersionMinor = s_config.firmwareVersionMinor;
        id.firmwareVersionPatch = s_config.firmwareVersionPatch;
        id.ipv4Address = s_config.ipv4AddressProvider != nullptr
                             ? s_config.ipv4AddressProvider()
                             : ((WiFi.status() == WL_CONNECTED) ? static_cast<uint32_t>(WiFi.localIP()) : 0);
        return id;
    }

    void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len)
    {
        if (len < static_cast<int>(sizeof(RTSNOW_Header)))
            return;
        RTSNOW_Header header;
        memcpy(&header, data, sizeof(header));
        if (header.version != 1 || header.sourceID == localDeviceId())
            return;

        const uint8_t *payload = data + sizeof(header);
        int payloadLen = len - static_cast<int>(sizeof(header));

        if (header.messageType == RTSNOW_REBOOT)
        {
            s_pendingReboot = true;
            return;
        }

        if (header.messageType == RTSNOW_REQUEST_REGISTERS)
        {
            memcpy(s_pendingRegisterRequestMac, mac, 6);
            s_pendingRegisterRequest = true;
            return;
        }

        if (header.messageType != RTSNOW_SET_SETTING)
            return;
        if (payloadLen < static_cast<int>(sizeof(RTSNOW_SettingPayload)))
            return;

        RTSNOW_SettingPayload setting;
        memcpy(&setting, payload, sizeof(setting));
        setting.key[sizeof(setting.key) - 1] = '\0';

        // "friendlyName" is the one reserved setting key this library
        // always applies itself (see RTS-ESPNOW-Gateway's PROTOCOL.md).
        // Anything else is handed to the host project's own
        // onGenericSetting, if it set one - see rtsnow_node.h's comment.
        memcpy(s_pendingSettingAckMac, mac, 6);
        s_pendingSetting = setting;
        s_pendingSettingAck = true;

        if (setting.valueType == RTSNOW_SETTING_STRING && strcmp(setting.key, "friendlyName") == 0)
        {
            setting.stringValue[sizeof(setting.stringValue) - 1] = '\0';
            strncpy(s_pendingRename, setting.stringValue, sizeof(s_pendingRename) - 1);
            s_pendingRename[sizeof(s_pendingRename) - 1] = '\0';
            s_hasPendingRename = true;
        }
    }
}

void rtsnowNodeBegin(const RTSNowNodeConfig &config)
{
    s_config = config;
    if (s_config.heartbeatIntervalMs == 0)
        s_config.heartbeatIntervalMs = 10000;

    loadFriendlyName();
    esp_now_init(); // WiFi.mode(WIFI_STA) must already have happened by this point
    esp_now_register_recv_cb(onEspNowRecv);

    RTSNOW_DeviceIdentity id = makeIdentity();
    sendBroadcast(RTSNOW_ANNOUNCE, &id, sizeof(id));
    delay(50);
    sendBroadcast(RTSNOW_ANNOUNCE, &id, sizeof(id)); // sent twice - cheap insurance, matches gateway-firmware's own node

    s_lastHeartbeatMs = millis();
    s_began = true;
    Serial.printf("RTS-NOW: announced (deviceID=0x%08X) on Wi-Fi channel %u - gateway must be set to this same channel to see it\n",
                  localDeviceId(), WiFi.channel());
}

void rtsnowNodeLoop()
{
    if (!s_began)
        return;

    uint32_t now = millis();
    if (now - s_lastHeartbeatMs >= s_config.heartbeatIntervalMs)
    {
        RTSNOW_Heartbeat hb{};
        hb.identity = makeIdentity();
        hb.uptimeSeconds = now / 1000;
        sendBroadcast(RTSNOW_HEARTBEAT, &hb, sizeof(hb));

        // Broadcast alongside the heartbeat (not just in reply to an
        // explicit RTSNOW_REQUEST_REGISTERS) so the gateway/desktop app
        // picks up a settings change made through some *other* interface
        // entirely - e.g. SPI-IM's own web dashboard - within one
        // heartbeat interval, without needing the app to already know to
        // ask again. Same registerBlockProvider used for the on-demand
        // path below; still silently skipped if a host project left it
        // null.
        if (s_config.registerBlockProvider != nullptr)
        {
            RTSNOW_RegisterBlock block{};
            s_config.registerBlockProvider(block);
            sendBroadcast(RTSNOW_REGISTER_VALUES, &block, sizeof(block));
        }

        s_lastHeartbeatMs = now;
    }

    if (s_pendingSettingAck)
    {
        bool isFriendlyName = strcmp(s_pendingSetting.key, "friendlyName") == 0;
        bool accepted = isFriendlyName || s_config.onGenericSetting == nullptr
                             ? true
                             : s_config.onGenericSetting(s_pendingSetting);

        RTSNOW_SettingAck ack{};
        strncpy(ack.key, s_pendingSetting.key, sizeof(ack.key) - 1);
        ack.accepted = accepted ? 1 : 0;
        sendTo(s_pendingSettingAckMac, RTSNOW_SETTING_ACK, &ack, sizeof(ack));
        Serial.printf("RTS-NOW: setting \"%s\" %s\n", s_pendingSetting.key, accepted ? "applied" : "rejected");
        s_pendingSettingAck = false;
    }

    if (s_hasPendingRename)
    {
        strncpy(s_friendlyName, s_pendingRename, sizeof(s_friendlyName) - 1);
        s_friendlyName[sizeof(s_friendlyName) - 1] = '\0';
        saveFriendlyName(s_friendlyName);
        Serial.printf("RTS-NOW: renamed to \"%s\" (saved)\n", s_friendlyName);
        s_hasPendingRename = false;
    }

    // Serviced before a pending reboot below, so a request that arrived
    // just before a reboot command still gets answered.
    if (s_pendingRegisterRequest)
    {
        s_pendingRegisterRequest = false;
        if (s_config.registerBlockProvider != nullptr)
        {
            RTSNOW_RegisterBlock block{};
            s_config.registerBlockProvider(block);
            sendTo(s_pendingRegisterRequestMac, RTSNOW_REGISTER_VALUES, &block, sizeof(block));
            Serial.printf("RTS-NOW: sent %u register(s) starting at %u\n", block.registerCount, block.startRegister);
        }
        // else: this node has no register table to report - silently not
        // answered, same as an RTSNOW_SET_SETTING key this node doesn't
        // recognize.
    }

    if (s_pendingReboot)
    {
        Serial.println("RTS-NOW: reboot requested - restarting now");
        delay(50); // let the Serial.println above actually get out over USB CDC before the restart
        if (s_config.onBeforeReboot != nullptr)
            s_config.onBeforeReboot();
        ESP.restart();
    }
}

const char *rtsnowNodeFriendlyName()
{
    return s_friendlyName;
}
