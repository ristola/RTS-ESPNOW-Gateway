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
#include <Update.h>
#include <WiFi.h>
#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>

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

    // ---- Firmware update over ESP-NOW (see RTSNOW_OTA_START's own
    // comment in rtsnow_protocol.h) ----
    //
    // One transfer at a time, matching there being exactly one OTA flash
    // partition to write - no need for anything more elaborate. Same
    // "flag in the callback, act in loop()" discipline as everything else
    // in this file: Update.write()/.begin()/.end() all touch flash, which
    // stays out of the ESP-NOW receive callback's own task context on
    // principle, same as every other real work in this file.
    bool s_otaActive = false;         // true between an accepted OTA_START and OTA_END (success or failure)
    uint32_t s_otaTotalChunks = 0;
    uint32_t s_otaNextExpectedIndex = 0;
    uint8_t s_otaPeerMac[6];          // physical mac to send OTA_START_ACK to - set from s_pendingOtaStartMac; chunk/end acks use their own freshly-captured mac instead (see below)

    // Every ack needs BOTH of these, not just the physical mac: the mac
    // is who to physically unicast to (may be a relay, not the true
    // originating gateway), while requesterId is the true originator's
    // own deviceID, set as the ack's destinationID (see sendTo's own
    // comment) so a relay's existing generic destinationID-based
    // forwarding routes the reply all the way back to the gateway even
    // though it was physically sent to the relay. Captured fresh from
    // each received message's own header.sourceID/physical mac, not
    // assumed constant across a transfer, even though in practice a
    // single transfer's requester/relay choice doesn't change mid-flight.
    bool s_otaStartPending = false;
    RTSNOW_OtaStart s_pendingOtaStart;
    uint8_t s_pendingOtaStartMac[6];
    uint32_t s_pendingOtaStartRequesterId = 0xFFFFFFFF;

    // At most one chunk buffered at a time - the sender's own strict
    // stop-and-wait (see RTSNOW_OtaChunk's comment) means a second chunk
    // is never sent before the first is acked, so there's nothing to gain
    // from a deeper queue here.
    bool s_otaChunkPending = false;
    RTSNOW_OtaChunk s_pendingOtaChunk;
    uint8_t s_pendingOtaChunkMac[6];
    uint32_t s_pendingOtaChunkRequesterId = 0xFFFFFFFF;

    bool s_otaEndPending = false;
    uint8_t s_pendingOtaEndMac[6];
    uint32_t s_pendingOtaEndRequesterId = 0xFFFFFFFF;

    bool s_otaAbortPending = false;

    uint8_t s_selfMac[6]; // cached once in rtsnowNodeBegin() - see onWifiPromiscuousRx's own-transmission guard

    // Single-hop relay forwarding (see onEspNowRecv's destinationID check
    // and gateway-firmware's recompute_routing() for how a node ends up
    // being asked to do this at all). Queued here and actually resent
    // from rtsnowNodeLoop(), NOT sent directly from onEspNowRecv - same
    // "flag in the callback, act in loop()" discipline as
    // s_pendingReboot/s_pendingSettingAck above, and for an added reason
    // specific to relaying: a single-shot forward was confirmed on real
    // hardware to have a real chance of silently failing, precisely
    // because a node only ever gets CHOSEN as a relay for a target whose
    // link is marginal even to it (that's the entire premise - a relay's
    // own link to the final target isn't necessarily strong just because
    // its link to the gateway is). Resending a few times, a beat apart,
    // is the exact same reliability fix gateway-firmware's own
    // kRemoteControlResendCount already applies to its direct sends.
    struct PendingRelay
    {
        bool inUse = false;
        uint8_t targetMac[6];
        uint8_t attemptsRemaining = 0;
        uint32_t nextAttemptMs = 0;
        uint8_t buf[250];
        int len = 0;
    };
    constexpr int kMaxPendingRelays = 2; // relaying is the exception, not the common case - this node is usually either the gateway's direct target or not involved at all
    PendingRelay s_pendingRelays[kMaxPendingRelays];
    constexpr uint8_t kRelayResendCount = 3;
    constexpr uint32_t kRelayResendGapMs = 40;

    // ---- Neighbor discovery (also feeds gateway-firmware's own
    // recompute_routing(), see RTSNOW_Heartbeat's own neighbors/
    // neighborCount comment) ----
    //
    // deviceID<->MAC comes from other nodes' own RTSNOW_ANNOUNCE/
    // RTSNOW_HEARTBEAT broadcasts (already reaching every device in
    // range, same as they already reach the gateway - ESP-NOW broadcasts
    // aren't gateway-exclusive), learned passively in onEspNowRecv below.
    // rssi/lastSeenMs come from a completely separate source,
    // onWifiPromiscuousRx: the normal esp_now_register_recv_cb callback
    // (used for onEspNowRecv itself) never gets radio metadata on this
    // SDK - only a WiFi promiscuous-mode tap does (identical reasoning,
    // and identical technique, to gateway-firmware's own
    // on_wifi_promiscuous_rx). A peer only ever gets reported as a
    // neighbor (see buildNeighborList) once both halves are known: its
    // identity (from an announce/heartbeat) AND a recent sniffed RSSI
    // (from the promiscuous tap) - one alone isn't enough to say "I can
    // hear this device."
    struct PeerInfo
    {
        uint32_t deviceID = 0;
        uint8_t mac[6] = {0};
        int8_t rssi = RTSNOW_RSSI_UNKNOWN;
        uint32_t lastSeenMs = 0;
        bool inUse = false;
    };
    // Generous vs. RTSNOW_MAX_NEIGHBORS (the wire limit on how many get
    // reported per heartbeat) - this table can track more peers than fit
    // in one heartbeat; buildNeighborList only reports the freshest ones
    // if it's ever actually full enough for that distinction to matter.
    constexpr uint8_t kMaxPeers = 16;
    PeerInfo s_peers[kMaxPeers];

    // A peer's sniffed RSSI is only trusted for this long before
    // buildNeighborList stops reporting it at all - a few heartbeat
    // intervals' worth, so a neighbor that's gone truly quiet drops off
    // the list instead of showing a frozen, possibly long-stale reading
    // forever.
    constexpr uint32_t kPeerStaleMs = 60000;

    // Learns/refreshes a peer's deviceID<->MAC mapping - called from
    // onEspNowRecv for every RTSNOW_ANNOUNCE/RTSNOW_HEARTBEAT this node
    // overhears from another node. Deliberately does NOT touch
    // rssi/lastSeenMs - those only ever come from onWifiPromiscuousRx,
    // never from this (RSSI-blind) callback.
    void notePeer(uint32_t deviceID, const uint8_t mac[6])
    {
        for (int i = 0; i < kMaxPeers; i++)
        {
            if (s_peers[i].inUse && s_peers[i].deviceID == deviceID)
            {
                memcpy(s_peers[i].mac, mac, 6);
                return;
            }
        }
        for (int i = 0; i < kMaxPeers; i++)
        {
            if (!s_peers[i].inUse)
            {
                s_peers[i] = PeerInfo{};
                s_peers[i].inUse = true;
                s_peers[i].deviceID = deviceID;
                memcpy(s_peers[i].mac, mac, 6);
                return;
            }
        }
        // Table genuinely full - silently drop. kMaxPeers is generous for
        // any realistic deployment of this project; losing the ability to
        // track one more peer's neighbor-diagnostic data isn't worth the
        // complexity of eviction logic for a case this unlikely.
    }

    // See this whole section's own comment above for why this needs a
    // separate WiFi promiscuous callback rather than getting RSSI off the
    // normal ESP-NOW receive callback. Runs in the WiFi driver's own task
    // context, same discipline as onEspNowRecv - a MAC comparison against
    // the peer table plus one int8_t/uint32_t write, deliberately kept
    // that cheap.
    void onWifiPromiscuousRx(void *buf, wifi_promiscuous_pkt_type_t type)
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
        if (memcmp(sourceMac, s_selfMac, 6) == 0)
            return; // our own transmission - shouldn't normally loop back, cheap to guard anyway

        for (int i = 0; i < kMaxPeers; i++)
        {
            if (s_peers[i].inUse && memcmp(s_peers[i].mac, sourceMac, 6) == 0)
            {
                s_peers[i].rssi = pkt->rx_ctrl.rssi;
                s_peers[i].lastSeenMs = millis();
                return;
            }
        }
        // Unknown MAC - either the gateway itself (which never sends its
        // own announce/heartbeat, so it's never in this table - correct,
        // this is node<->node discovery, not node<->gateway, which
        // wifiRssi/the gateway's own espNowRssi capture already cover)
        // or just a peer whose announce/heartbeat hasn't arrived yet.
        // Nothing to attribute this reading to until one does.
    }

    // Fills outNeighbors (capacity maxOut) with this node's current view
    // of nearby peers - see this section's own top comment for what
    // "current" requires (both a known identity AND a recent sniffed
    // RSSI). Called fresh from rtsnowNodeLoop() every heartbeat, not
    // cached, so a neighbor that drops off (or newly appears) is
    // reflected within one heartbeat interval.
    uint8_t buildNeighborList(RTSNOW_NeighborInfo *outNeighbors, uint8_t maxOut)
    {
        uint8_t count = 0;
        uint32_t now = millis();
        for (int i = 0; i < kMaxPeers && count < maxOut; i++)
        {
            if (!s_peers[i].inUse)
                continue;
            if (s_peers[i].rssi == RTSNOW_RSSI_UNKNOWN)
                continue;
            if (now - s_peers[i].lastSeenMs > kPeerStaleMs)
                continue;
            outNeighbors[count].deviceID = s_peers[i].deviceID;
            outNeighbors[count].rssi = s_peers[i].rssi;
            count++;
        }
        return count;
    }

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

    // destinationID defaults to broadcast (0xFFFFFFFF), correct for every
    // pre-existing call site: the ESP-NOW hardware only ever delivers a
    // unicast frame to its actual physical destination regardless of this
    // logical field, so it only needs to be set explicitly when a reply
    // might have to travel back through a relay - see onEspNowRecv's OTA
    // ack sends, which pass the original request's own header.sourceID
    // back here so a relay's existing generic destinationID-based
    // forwarding (see that function's own comment) can route the reply
    // to the true originating gateway, not just whoever physically
    // delivered the request to this node.
    bool sendTo(const uint8_t mac[6], RTSNOW_MessageType type, const void *payload, uint16_t payloadLen,
                uint32_t destinationID = 0xFFFFFFFF)
    {
        uint8_t buf[250];
        if (sizeof(RTSNOW_Header) + payloadLen > sizeof(buf))
            return false;

        RTSNOW_Header header;
        header.version = 1;
        header.messageType = type;
        header.sourceID = localDeviceId();
        header.destinationID = destinationID;
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

        // Single-hop relay (see rtsnow_protocol.h's destinationID comment
        // and gateway-firmware's recompute_routing()): the ONLY way this
        // node ever sees a frame addressed to someone else at all is that
        // the gateway explicitly unicast it to THIS node's own MAC,
        // having decided this node has a better path to the real target
        // than the gateway does directly - the ESP-NOW hardware itself
        // never delivers a unicast frame to any device except its actual
        // physical destination, so there's no risk of every node trying
        // to relay every frame it wasn't literally addressed to receive
        // in the first place. RTSNOW_ANNOUNCE/RTSNOW_HEARTBEAT broadcasts
        // (destinationID always 0xFFFFFFFF) are never affected by this -
        // this block only ever fires for a frame explicitly addressed
        // (by deviceID) to some OTHER specific device.
        //
        // Raw re-transmit: header and payload bytes both go out completely
        // unchanged, just to a different physical next-hop MAC - so
        // header.sourceID (who this is really from) and header.destinationID
        // (who it's really for) both stay intact for the real endpoints to
        // make sense of, and this node needs no awareness of message types
        // it doesn't otherwise handle. Exactly one hop: the real target's
        // own destinationID check below matches its own deviceID, so it
        // processes the frame normally rather than relaying it any
        // further - there is no path by which this can cascade or loop.
        //
        // Queued into s_pendingRelays rather than sent here directly -
        // see that struct's own comment for why (resend-for-reliability
        // needs delay()-style spacing, which does not belong inside this
        // callback's task context).
        if (header.destinationID != 0xFFFFFFFF && header.destinationID != localDeviceId())
        {
            if (static_cast<size_t>(len) <= sizeof(PendingRelay::buf))
            {
                for (int i = 0; i < kMaxPeers; i++)
                {
                    if (s_peers[i].inUse && s_peers[i].deviceID == header.destinationID)
                    {
                        int slot = -1;
                        for (int r = 0; r < kMaxPendingRelays; r++)
                        {
                            if (!s_pendingRelays[r].inUse)
                            {
                                slot = r;
                                break;
                            }
                        }
                        if (slot < 0)
                            slot = 0; // table full (relaying two things at once is already an edge case) - overwrite the oldest rather than drop this one silently
                        memcpy(s_pendingRelays[slot].targetMac, s_peers[i].mac, 6);
                        memcpy(s_pendingRelays[slot].buf, data, len);
                        s_pendingRelays[slot].len = len;
                        s_pendingRelays[slot].attemptsRemaining = kRelayResendCount;
                        s_pendingRelays[slot].nextAttemptMs = millis(); // first attempt fires on the very next rtsnowNodeLoop() call
                        s_pendingRelays[slot].inUse = true;
                        break;
                    }
                }
            }
            return; // not this node's to process, whether relayed or dropped (no known route)
        }

        const uint8_t *payload = data + sizeof(header);
        int payloadLen = len - static_cast<int>(sizeof(header));

        // Peer discovery (see this file's own top-of-section comment) -
        // purely passive, learned from broadcasts every node already
        // sends for unrelated reasons. Doesn't return or otherwise change
        // any of the existing handling below.
        if ((header.messageType == RTSNOW_ANNOUNCE || header.messageType == RTSNOW_HEARTBEAT) &&
            payloadLen >= static_cast<int>(sizeof(RTSNOW_DeviceIdentity)))
        {
            // Only the leading identity is needed here, not the rest of a
            // heartbeat - safe to read regardless of whether the sender's
            // own RTSNOW_Heartbeat is a different (older/newer) size than
            // this build's, since RTSNOW_DeviceIdentity's own layout never
            // changes size (see rtsnow_protocol.h).
            RTSNOW_DeviceIdentity identity;
            memcpy(&identity, payload, sizeof(identity));
            notePeer(identity.deviceID, mac);
        }

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

        // Firmware update over ESP-NOW (see RTSNOW_OTA_START's own
        // comment) - every message here just flags + copies the small
        // fixed-size payload; rtsnowNodeLoop() does the actual
        // Update.begin()/write()/end() calls, keeping flash access out of
        // this callback's task context.
        if (header.messageType == RTSNOW_OTA_START && payloadLen >= static_cast<int>(sizeof(RTSNOW_OtaStart)))
        {
            memcpy(&s_pendingOtaStart, payload, sizeof(s_pendingOtaStart));
            memcpy(s_pendingOtaStartMac, mac, 6);
            s_pendingOtaStartRequesterId = header.sourceID;
            s_otaStartPending = true;
            return;
        }

        if (header.messageType == RTSNOW_OTA_CHUNK && payloadLen >= static_cast<int>(sizeof(RTSNOW_OtaChunk)))
        {
            // Drop (don't buffer/queue) if a previous chunk hasn't been
            // processed yet - the sender's own strict stop-and-wait means
            // this should never actually happen, and dropping is exactly
            // what should happen for a stray duplicate/retransmit that
            // arrives after this node already moved on.
            if (!s_otaChunkPending)
            {
                memcpy(&s_pendingOtaChunk, payload, sizeof(s_pendingOtaChunk));
                memcpy(s_pendingOtaChunkMac, mac, 6);
                s_pendingOtaChunkRequesterId = header.sourceID;
                s_otaChunkPending = true;
            }
            return;
        }

        if (header.messageType == RTSNOW_OTA_END)
        {
            memcpy(s_pendingOtaEndMac, mac, 6);
            s_pendingOtaEndRequesterId = header.sourceID;
            s_otaEndPending = true;
            return;
        }

        if (header.messageType == RTSNOW_OTA_ABORT)
        {
            s_otaAbortPending = true;
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
    WiFi.macAddress(s_selfMac);
    esp_now_init(); // WiFi.mode(WIFI_STA) must already have happened by this point
    esp_now_register_recv_cb(onEspNowRecv);

    // Neighbor discovery's RSSI half (see onWifiPromiscuousRx's own
    // comment for why this needs a separate WiFi promiscuous tap rather
    // than reading RSSI off the normal ESP-NOW receive callback above).
    // The driver-level filter does the heavy lifting - only WIFI_PKT_MGMT
    // frames ever reach the callback at all. Coexists with this node's
    // own WiFi STA connection to its real AP (unlike gateway-firmware's
    // node, which stays deliberately unassociated) - promiscuous mode is
    // a passive additional tap, not exclusive with normal station
    // operation, a supported combination on this chip.
    wifi_promiscuous_filter_t promiscuousFilter = {};
    promiscuousFilter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&promiscuousFilter);
    esp_wifi_set_promiscuous_rx_cb(onWifiPromiscuousRx);
    esp_wifi_set_promiscuous(true);

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
        hb.wifiRssi = (WiFi.status() == WL_CONNECTED) ? static_cast<int8_t>(WiFi.RSSI()) : RTSNOW_RSSI_UNKNOWN;
        strncpy(hb.boardName, s_config.boardName, sizeof(hb.boardName) - 1);
        hb.neighborCount = buildNeighborList(hb.neighbors, RTSNOW_MAX_NEIGHBORS);
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

    // Relay resends (see PendingRelay's own comment) - each due attempt
    // fires straight from esp_now_send with no delay() here; the
    // kRelayResendGapMs spacing comes for free from this only being
    // checked once per rtsnowNodeLoop() pass rather than in a tight loop.
    for (int i = 0; i < kMaxPendingRelays; i++)
    {
        if (!s_pendingRelays[i].inUse || now < s_pendingRelays[i].nextAttemptMs)
            continue;
        ensurePeer(s_pendingRelays[i].targetMac);
        esp_now_send(s_pendingRelays[i].targetMac, s_pendingRelays[i].buf, s_pendingRelays[i].len);
        s_pendingRelays[i].attemptsRemaining--;
        if (s_pendingRelays[i].attemptsRemaining == 0)
            s_pendingRelays[i].inUse = false;
        else
            s_pendingRelays[i].nextAttemptMs = now + kRelayResendGapMs;
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

    // Firmware update over ESP-NOW (see RTSNOW_OTA_START's own comment).
    // Serviced before the plain reboot below since OTA_END's own success
    // path also restarts - if both happened to be pending in the same
    // tick, finishing (and rebooting into) an in-progress firmware write
    // takes priority over an unrelated reboot request.
    if (s_otaStartPending)
    {
        s_otaStartPending = false;
        memcpy(s_otaPeerMac, s_pendingOtaStartMac, 6);
        if (s_otaActive)
            Update.abort(); // a fresh START while one was already active - trust the new one, not stale state from whatever the old one was
        bool began = Update.begin(s_pendingOtaStart.totalSize);
        if (began)
        {
            char md5[33];
            memcpy(md5, s_pendingOtaStart.md5, sizeof(md5));
            md5[32] = '\0';
            if (strlen(md5) == 32) // only trust it if it actually looks like a real MD5 hex string
                Update.setMD5(md5);
            s_otaActive = true;
            s_otaTotalChunks = s_pendingOtaStart.totalChunks;
            s_otaNextExpectedIndex = 0;
        }
        RTSNOW_OtaAck ack{};
        ack.ok = began ? 1 : 0;
        if (!began)
            strncpy(ack.message, "Update.begin failed", sizeof(ack.message) - 1);
        sendTo(s_otaPeerMac, RTSNOW_OTA_START_ACK, &ack, sizeof(ack), s_pendingOtaStartRequesterId);
        Serial.printf("RTS-NOW: OTA-over-ESP-NOW start %s (size=%u, %u chunks)\n", began ? "accepted" : "REJECTED",
                      s_pendingOtaStart.totalSize, s_pendingOtaStart.totalChunks);
    }

    if (s_otaChunkPending)
    {
        s_otaChunkPending = false;
        uint32_t idx = s_pendingOtaChunk.index;
        bool ok = false;
        if (s_otaActive && idx == s_otaNextExpectedIndex)
        {
            size_t written = Update.write(s_pendingOtaChunk.data, s_pendingOtaChunk.length);
            ok = (written == s_pendingOtaChunk.length);
            if (ok)
                s_otaNextExpectedIndex++;
        }
        // else: no transfer in progress, or this index doesn't match what
        // comes next (a duplicate retransmit of an already-applied chunk,
        // or a genuinely out-of-order arrival) - ok stays false either
        // way, which tells the sender to retry rather than silently
        // desyncing this node's view of the transfer from the sender's.
        RTSNOW_OtaChunkAck ack{};
        ack.index = idx;
        ack.ok = ok ? 1 : 0;
        sendTo(s_pendingOtaChunkMac, RTSNOW_OTA_CHUNK_ACK, &ack, sizeof(ack), s_pendingOtaChunkRequesterId);
    }

    if (s_otaEndPending)
    {
        s_otaEndPending = false;
        RTSNOW_OtaAck ack{};
        if (s_otaActive && s_otaNextExpectedIndex == s_otaTotalChunks)
        {
            bool success = Update.end(true);
            ack.ok = success ? 1 : 0;
            if (!success)
                snprintf(ack.message, sizeof(ack.message), "Update.end failed: %s", Update.errorString());
        }
        else
        {
            ack.ok = 0;
            strncpy(ack.message, "incomplete transfer", sizeof(ack.message) - 1);
        }
        sendTo(s_pendingOtaEndMac, RTSNOW_OTA_END_ACK, &ack, sizeof(ack), s_pendingOtaEndRequesterId);
        if (ack.ok)
        {
            Serial.println("RTS-NOW: OTA-over-ESP-NOW complete - restarting");
            delay(100); // let the ack and this line actually get out before the restart
            if (s_config.onBeforeReboot != nullptr)
                s_config.onBeforeReboot();
            ESP.restart();
            // ESP.restart() does not return - nothing below this point in
            // this block ever runs on the success path.
        }
        else
        {
            Serial.printf("RTS-NOW: OTA-over-ESP-NOW failed: %s\n", ack.message);
            Update.abort();
            s_otaActive = false;
        }
    }

    if (s_otaAbortPending)
    {
        s_otaAbortPending = false;
        if (s_otaActive)
        {
            Update.abort();
            s_otaActive = false;
            Serial.println("RTS-NOW: OTA-over-ESP-NOW aborted");
        }
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
