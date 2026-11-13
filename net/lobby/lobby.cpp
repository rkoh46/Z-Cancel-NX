// SPDX-License-Identifier: GPL-3.0-or-later
/* The lobby protocol: what RMG-K's LobbyClient (Source/RMG/UserInterface/Dialog/Lobby/
 * LobbyClient.cpp) and the protocol half of RollbackLobbyDialog.cpp do, without Qt. Line numbers
 * in the comments are RMG-K 0.9.16 (2f5454a). Not implemented, because the Switch build does not
 * offer them yet: spectating and live replay (BROADCAST_*, SPECTATE_*), late join (MATCH_ADMIT_*,
 * so HELLO says lateJoin:false), quick match, moderation commands, the legacy UDP anchor. Their
 * server messages are ignored, as RMG-K ignores unknown types (LobbyClient.cpp:958). */
#include "znx_lobby.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ice_mesh.h"
#include "json.h"
#include "net_io.h"
#include "ws.h"

using namespace znxlobby;

namespace
{
constexpr uint64_t kHeartbeatMs = 15000;              // LobbyClient.cpp:65
constexpr uint64_t kPingTickMs = 3000;                // RollbackLobbyDialog.cpp:749
constexpr int kProbeAttempts = 4;                     // LobbyClient.cpp:148
constexpr uint64_t kProbeRetryMs = 300;               // LobbyClient.cpp:149
constexpr int kMaxIceAutoRestarts = 19;               // LobbyClient.cpp:163
constexpr uint64_t kMatchIceWaitMs = 15000;           // RollbackLobbyDialog.cpp:5776
constexpr uint64_t kPrematchTimeoutMs = 10000;        // LobbyClient.cpp:2575
constexpr uint64_t kPrematchResendMs = 100;           // LobbyClient.cpp:2625
constexpr int kPrematchAckRepeats = 8;                // LobbyClient.cpp:2735
constexpr int kDefaultPrediction = 7;                 // RollbackLobbyDialog.cpp:125

constexpr uint8_t kPingRequest = 1, kPingReply = 2, kPingReport = 3;   // LobbyClient.cpp:155-159
constexpr char kAnchorMagic[4] = {'R', 'M', 'G', 'K'};
constexpr uint8_t kOpPrematchManifest = 0x06, kOpPrematchAck = 0x07, kOpPrematchFragment = 0x08;
constexpr size_t kFragmentPayload = 1000;             // LobbyClient.cpp:446

int autoDelayForPing(int ping)   // RollbackLobbyDialog.cpp:113-121
{
    if (ping < 0) return 2;
    if (ping <= 50) return 1;
    if (ping <= 100) return 2;
    if (ping <= 150) return 3;
    if (ping <= 220) return 4;
    return 5;
}

uint64_t fnv1a64(const std::string& data)   // LobbyClient.cpp:247-256
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

void putLe(std::string& s, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

uint64_t getLe(const char* p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v |= uint64_t(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}

void putBe64(std::string& s, uint64_t v)
{
    for (int i = 7; i >= 0; --i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

uint64_t getBe64(const char* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    return v;
}

// RMG-K's manifest with no cheats and no save section: "RMGKPMAN", version 1, sections = cheats,
// then the cheat list "RMGKCHT1" with a count of 0 (LobbyClient.cpp:334-367, Cheats.cpp:1174-1205).
std::string emptyManifest()
{
    std::string cheats("RMGKCHT1", 8);
    putLe(cheats, 0, 4);
    std::string m("RMGKPMAN", 8);
    putLe(m, 1, 4);
    putLe(m, 1, 4);
    putLe(m, cheats.size(), 4);
    m += cheats;
    return m;
}

// The structure applyPrematchManifest checks before handing the parts to the core
// (LobbyClient.cpp:369-416).
bool manifestWellFormed(const std::string& m)
{
    if (m.size() < 20 || std::memcmp(m.data(), "RMGKPMAN", 8) != 0)
        return false;
    const uint32_t version = uint32_t(getLe(m.data() + 8, 4));
    const uint32_t mask = uint32_t(getLe(m.data() + 12, 4));
    if (version != 1 || (mask & ~3u) != 0)
        return false;
    size_t off = 16;
    const uint32_t cheatLen = uint32_t(getLe(m.data() + off, 4));
    off += 4;
    if (off + cheatLen > m.size() || cheatLen < 12 || std::memcmp(m.data() + off, "RMGKCHT1", 8) != 0)
        return false;
    off += cheatLen;
    if (mask & 2u)
    {
        if (off + 4 > m.size())
            return false;
        const uint32_t saveLen = uint32_t(getLe(m.data() + off, 4));
        off += 4;
        if (off + saveLen > m.size())
            return false;
        off += saveLen;
    }
    return off == m.size();
}

void copyStr(char* dst, size_t cap, const std::string& s)
{
    if (cap == 0)
        return;
    const size_t n = std::min(cap - 1, s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

const char* iceStateName(IceState s)
{
    switch (s)
    {
    case IceState::Disconnected: return "disconnected";
    case IceState::Gathering: return "gathering";
    case IceState::Connecting: return "connecting";
    case IceState::Connected: return "connected";
    case IceState::Completed: return "completed";
    case IceState::Failed: return "failed";
    }
    return "unknown";
}
} // namespace

struct znx_lobby
{
    enum class Conn { Disconnected, Connecting, Authenticating, Connected, Failed };
    enum class MatchPhase { None, WaitIce, Prematch, Ready, Running };

    struct Probe
    {
        uint64_t target = 0;
        uint64_t sendMs = 0;
        uint64_t attemptSendMs = 0;
        uint64_t nextAttemptMs = 0;
        int attempt = 1;
    };
    struct MatchPeer
    {
        uint64_t userId = 0;
        std::string username;
        int slot = 0;
        bool iceSupported = false;
    };

    // configuration
    std::string stunHostOverride;
    bool stunOverridden = false;
    uint16_t stunPortOverride = 19302;
    bool useServerTurn = true;
    std::string bindAddress, localIp, connection;
    int tzOffset = 0;
    bool anonymous = false;
    bool saveDataCapable = false;
    void (*logFn)(void*, const char*) = nullptr;
    void* logUser = nullptr;

    // connection
    WsClient ws;
    Conn conn = Conn::Disconnected;
    std::string username, romMd5, clientVersion;
    uint64_t selfId = 0;
    std::string observedIp, region;
    uint64_t nextHeartbeat = 0;
    std::deque<znx_lobby_event> events;

    // lobby
    std::map<uint64_t, znx_lobby_user_info> users;
    std::vector<znx_lobby_room_info> rooms;

    // room
    uint64_t roomId = 0;          // the room the dialog shows (ROOM_CREATED / ROOM_JOIN_OK)
    Json roomState;
    bool roomSeen = false;
    int localDelay = 2;
    bool delayAuto = true;
    bool delayPublished = false;
    int publishedDelay = -1;
    std::string prematchManifest = emptyManifest();

    // ICE (LobbyClient.hpp:530-546)
    IceMesh mesh;
    uint64_t iceRoomId = 0;
    uint64_t iceHostId = 0;
    std::set<uint64_t> icePeers;
    std::map<uint64_t, int> lastIceState;
    std::map<uint64_t, uint32_t> iceGeneration;
    std::map<uint64_t, int> iceRestarts;
    bool matchTransportActive = false;
    std::set<uint64_t> iceOutsideMatch;
    bool deferredMeshReset = false, deferredTransportReset = false;
    std::map<uint64_t, std::map<uint64_t, int>> roomPeerPings;

    // pings
    std::map<uint64_t, Probe> probes;   // by nonce
    std::map<uint64_t, int> measuredPing;
    uint64_t nextPingTick = 0;

    // datagrams taken from the mesh but not yet handed to znx_lobby_receive (any thread)
    std::mutex rxMutex;
    std::map<int, std::deque<IcePacket>> rxPending;

    // match
    MatchPhase phase = MatchPhase::None;
    uint64_t matchId = 0;
    std::vector<MatchPeer> matchPeers;
    int matchSeats = 0;
    uint64_t phaseDeadline = 0;
    uint64_t matchHostId = 0;
    int matchLocalSlot = 0;
    int matchDelay = 2;
    int matchPrediction = kDefaultPrediction;
    int matchPacing = 1;
    std::string matchManifest;
    uint64_t matchManifestHash = 0;
    std::vector<std::string> outbound;      // host: manifest packet(s)
    std::set<uint64_t> pendingAcks;
    uint64_t nextPrematchSend = 0;
    uint64_t fragHash = 0;
    int fragCount = 0;
    uint32_t fragTotal = 0;
    std::vector<std::string> fragParts;

    // ---------------------------------------------------------------------------------------
    void log(const char* fmt, ...)
    {
        if (!logFn)
            return;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        logFn(logUser, buf);
    }

    znx_lobby_event& push(znx_lobby_event_type type)
    {
        znx_lobby_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = type;
        events.push_back(ev);
        return events.back();
    }

    void pushText(znx_lobby_event_type type, const std::string& text)
    {
        copyStr(push(type).text, sizeof(znx_lobby_event::text), text);
    }

    bool sendEnvelope(const char* type, const Json& data = Json(), const char* id = nullptr)
    {
        if (!ws.isOpen())
            return false;
        Json env = Json::object();
        env.set("type", Json::string(type));
        if (id)
            env.set("id", Json::string(id));
        if (data.isObject() && data.size() > 0)
            env.set("data", data);
        return ws.sendText(env.dump());
    }

    // ---- connection ----------------------------------------------------------------------
    int connect(const char* url, const char* user, const char* md5, const char* version)
    {
        if (matchTransportActive)
        {
            // HELLO_OK reconfigures ICE; a running match owns the mesh (LobbyClient.cpp:583-591).
            log("lobby: reconnect deferred while the match owns the ICE transport");
            return -1;
        }
        if (conn != Conn::Disconnected && conn != Conn::Failed)
            return -1;
        username = user ? user : "";
        romMd5 = md5 ? md5 : "";
        clientVersion = version && *version ? version : ZNX_LOBBY_CLIENT_VERSION;
        selfId = 0;
        users.clear();
        rooms.clear();
        clearRoom();
        std::string error;
        if (!ws.open(url ? url : ZNX_LOBBY_URL_RELEASE, error))
        {
            conn = Conn::Failed;
            pushText(ZNX_LOBBY_EV_DISCONNECTED, error);
            return -1;
        }
        conn = Conn::Connecting;
        log("lobby: connecting to %s", url ? url : ZNX_LOBBY_URL_RELEASE);
        return 0;
    }

    void disconnect()
    {
        resetIceTransport();
        if (ws.state() != WsClient::State::Idle && ws.state() != WsClient::State::Closed)
            ws.close();
        conn = Conn::Disconnected;
    }

    void onOpened()
    {
        conn = Conn::Authenticating;
        log("lobby: WebSocket connected, authenticating");
        Json d = Json::object();
        d.set("username", Json::string(username));
        d.set("clientVersion", Json::string(clientVersion));
        if (!connection.empty())
            d.set("connection", Json::string(connection));
        if (anonymous)
            d.set("anonymous", Json::boolean(true));
        if (tzOffset != 0)
            d.set("tzOffset", Json::integer(tzOffset));
        Json roms = Json::array();
        if (!romMd5.empty())
            roms.push(Json::string(romMd5));
        d.set("romHashes", roms);
        d.set("iceSupported", Json::boolean(true));
        d.set("lateJoin", Json::boolean(false));   // no MATCH_ADMIT hand-off here
        d.set("saveData", Json::boolean(saveDataCapable));
        if (!localIp.empty())
            d.set("localIp", Json::string(localIp));
        sendEnvelope("HELLO", d, "hello-1");
    }

    void onClosed(const std::string& reason)
    {
        log("lobby: connection closed: %s", reason.c_str());
        resetIceTransport();
        measuredPing.clear();
        probes.clear();
        const bool wasConnected = conn != Conn::Disconnected;
        if (conn != Conn::Failed)
            conn = Conn::Disconnected;
        if (roomId != 0)
            clearRoom();
        if (wasConnected)
            pushText(ZNX_LOBBY_EV_DISCONNECTED, reason);
    }

    void clearRoom()
    {
        roomId = 0;
        roomState = Json();
        roomSeen = false;
        delayPublished = false;
        publishedDelay = -1;
    }

    // ---- envelopes ------------------------------------------------------------------------
    void handle(const std::string& text)
    {
        Json env;
        std::string err;
        if (!Json::parse(text, env, &err) || !env.isObject())
        {
            log("lobby: bad JSON (%s)", err.c_str());
            return;
        }
        const std::string type = env["type"].str();
        const Json& data = env["data"];
        if (type == "HELLO_OK") helloOk(data);
        else if (type == "HELLO_FAIL") helloFail(data);
        else if (type == "HEARTBEAT_ACK") {}
        else if (type == "PRESENCE_FULL") presence(data, true);
        else if (type == "PRESENCE_DELTA") presence(data, false);
        else if (type == "ROOM_LIST") roomList(data);
        else if (type == "ROOM_CREATED") roomCreated(data);
        else if (type == "ROOM_CREATE_FAIL") pushText(ZNX_LOBBY_EV_ROOM_CREATE_FAILED, data["reason"].str());
        else if (type == "ROOM_STATE") roomStateMsg(data, false);
        else if (type == "ROOM_JOIN_OK") roomStateMsg(data, true);
        else if (type == "ROOM_JOIN_FAIL") pushText(ZNX_LOBBY_EV_ROOM_JOIN_FAILED, data["reason"].str());
        else if (type == "ROOM_LEFT") roomLeft(data);
        else if (type == "CHAT_MSG") chat(data);
        else if (type == "CHAT_HISTORY_REPLY")
        {
            for (const Json& m : data["messages"].elements())
                chat(m);
        }
        else if (type == "ICE_SIGNAL") iceSignal(data);
        else if (type == "MATCH_BEGIN") matchBegin(data);
        else if (type == "MATCH_PEER_LEFT")
        {
            auto& ev = push(ZNX_LOBBY_EV_MATCH_PEER_LEFT);
            ev.match_id = data["matchId"].toU64();
            ev.user_id = data["userId"].toU64();
            ev.value = data["slot"].toInt(0);
            copyStr(ev.text, sizeof(ev.text), data["reason"].str());
        }
        else if (type == "MOD_NOTICE")
        {
            auto& ev = push(ZNX_LOBBY_EV_NOTICE);
            copyStr(ev.name, sizeof(ev.name), data["severity"].str());
            copyStr(ev.text, sizeof(ev.text), data["text"].str());
        }
        else
            log("lobby: ignored message %s", type.c_str());
    }

    void helloOk(const Json& d)
    {
        selfId = d["userId"].toU64();
        observedIp = d["observedIp"].str();
        region = d["region"].str();

        // STUN/TURN as LobbyClient.cpp:969-991, unless the caller overrode them.
        IceConfig ic;
        ic.stunHost = "stun.l.google.com";
        ic.stunPort = 19302;
        const Json& ice = d["ice"];
        if (!ice["stun"]["host"].str().empty())
            ic.stunHost = ice["stun"]["host"].str();
        const int sp = ice["stun"]["port"].toInt();
        if (sp > 0 && sp <= 65535)
            ic.stunPort = static_cast<uint16_t>(sp);
        if (stunOverridden)
        {
            ic.stunHost = stunHostOverride;
            ic.stunPort = stunPortOverride;
        }
        if (useServerTurn)
        {
            for (const Json& t : ice["turn"].elements())
            {
                IceTurnServer s;
                s.host = t["host"].str();
                const int port = t["port"].toInt(3478);
                if (port < 1 || port > 65535 || s.host.empty())
                    continue;
                s.port = static_cast<uint16_t>(port);
                s.username = t["username"].str();
                s.password = t["password"].str();
                ic.turn.push_back(s);
            }
        }
        ic.bindAddress = bindAddress;
        if (!mesh.configure(selfId, ic))
        {
            conn = Conn::Failed;
            pushText(ZNX_LOBBY_EV_DISCONNECTED, "Failed to initialize ICE transport.");
            ws.close();
            return;
        }
        log("lobby: authenticated as user %llu (observed %s, region %s), STUN %s, %zu TURN",
            (unsigned long long)selfId, observedIp.c_str(), region.c_str(),
            ic.stunHost.empty() ? "off" : (ic.stunHost + ":" + std::to_string(ic.stunPort)).c_str(),
            ic.turn.size());
        conn = Conn::Connected;
        nextHeartbeat = now_ms() + kHeartbeatMs;
        push(ZNX_LOBBY_EV_CONNECTED).user_id = selfId;
    }

    void helloFail(const Json& d)
    {
        const std::string reason = d["reason"].str();
        const std::string message = d["message"].str();
        conn = Conn::Failed;
        pushText(ZNX_LOBBY_EV_HELLO_FAILED, message.empty() ? reason : message);
        ws.close();
    }

    static znx_lobby_user_info parseUser(const Json& o)   // LobbyClient.cpp:3608-3625
    {
        znx_lobby_user_info u;
        std::memset(&u, 0, sizeof(u));
        u.id = o["id"].toU64();
        copyStr(u.username, sizeof(u.username), o["username"].str());
        copyStr(u.state, sizeof(u.state), o["state"].str());
        copyStr(u.region, sizeof(u.region), o["region"].str());
        copyStr(u.country, sizeof(u.country), o["country"].str());
        copyStr(u.client_version, sizeof(u.client_version), o["clientVersion"].str());
        copyStr(u.connection, sizeof(u.connection), o["connection"].str());
        u.room_id = o["currentRoomId"].toU64();
        copyStr(u.room_name, sizeof(u.room_name), o["currentRoomName"].str());
        return u;
    }

    void presence(const Json& d, bool full)
    {
        if (full)
        {
            users.clear();
            for (const Json& u : d["users"].elements())
            {
                auto info = parseUser(u);
                users[info.id] = info;
            }
        }
        else
        {
            for (const char* key : {"added", "updated"})
                for (const Json& u : d[key].elements())
                {
                    auto info = parseUser(u);
                    users[info.id] = info;
                }
            for (const Json& id : d["removed"].elements())
                users.erase(id.toU64());
        }
        push(ZNX_LOBBY_EV_USERS);
    }

    void roomList(const Json& d)   // LobbyClient.cpp:1090-1118
    {
        rooms.clear();
        for (const Json& o : d["rooms"].elements())
        {
            znx_lobby_room_info r;
            std::memset(&r, 0, sizeof(r));
            r.id = o["id"].toU64();
            copyStr(r.name, sizeof(r.name), o["name"].str());
            r.host_id = o["hostId"].toU64();
            copyStr(r.host_name, sizeof(r.host_name), o["hostName"].str());
            copyStr(r.rom_name, sizeof(r.rom_name), o["rom"]["name"].str());
            copyStr(r.rom_md5, sizeof(r.rom_md5), o["rom"]["md5"].str());
            r.players = o["players"].toInt();
            r.max_players = o["maxPlayers"].toInt();
            copyStr(r.state, sizeof(r.state), o["state"].str());
            r.has_password = o["hasPassword"].toBool();
            r.late_join = o["lateJoin"].toBool();
            rooms.push_back(r);
        }
        push(ZNX_LOBBY_EV_ROOM_LIST);
    }

    void roomCreated(const Json& d)
    {
        const uint64_t id = d["roomId"].toU64();
        enterRoom(id);
        push(ZNX_LOBBY_EV_ROOM_CREATED).room_id = id;
    }

    void enterRoom(uint64_t id)
    {
        clearRoom();
        roomId = id;
        localDelay = delayAuto ? autoDelayForPing(-1) : localDelay;
    }

    void roomStateMsg(const Json& d, bool joinOk)
    {
        reconcileIcePeers(d);
        const uint64_t id = d["id"].toU64();
        if (joinOk)
        {
            enterRoom(id);
            push(ZNX_LOBBY_EV_ROOM_JOINED).room_id = id;
        }
        if (id == 0 || id != roomId)
            return;
        roomState = d;
        roomSeen = true;
        if (roomState["state"].str() == "waiting")
            applyLocalDelay(false);
        push(ZNX_LOBBY_EV_ROOM_STATE).room_id = id;
    }

    void roomLeft(const Json& d)
    {
        resetIceMesh();
        clearRoom();
        pushText(ZNX_LOBBY_EV_ROOM_LEFT, d["reason"].str());
    }

    void chat(const Json& d)   // LobbyClient.cpp:1635-1645
    {
        auto& ev = push(ZNX_LOBBY_EV_CHAT);
        ev.user_id = d["fromUserId"].toU64();
        copyStr(ev.channel, sizeof(ev.channel), d["channel"].str());
        copyStr(ev.name, sizeof(ev.name), d["fromUsername"].str());
        copyStr(ev.text, sizeof(ev.text), d["message"].str());
    }

    // ---- delay ---------------------------------------------------------------------------
    int worstLocalPeerPing() const   // RollbackLobbyDialog.cpp:4873-4887
    {
        int worst = -1;
        for (const Json& p : roomState["players"].elements())
        {
            const uint64_t uid = p["userId"].toU64();
            if (uid == 0 || uid == selfId)
                continue;
            auto it = measuredPing.find(uid);
            if (it != measuredPing.end())
                worst = std::max(worst, it->second);
        }
        return worst;
    }

    void applyLocalDelay(bool force)   // RollbackLobbyDialog.cpp:4896-4945 (waiting room only)
    {
        if (roomId == 0 || roomState["state"].str() != "waiting")
            return;
        const int delay = delayAuto ? autoDelayForPing(worstLocalPeerPing()) : localDelay;
        localDelay = delay;
        if (!force && delayPublished && delay == publishedDelay)
            return;
        Json d = Json::object();
        d.set("delay", Json::integer(delay));
        d.set("delayAuto", Json::boolean(delayAuto));
        if (sendEnvelope("ROOM_UPDATE_DELAY", d))
        {
            delayPublished = true;
            publishedDelay = delay;
        }
    }

    // ---- ICE mesh (LobbyClient.cpp:1154-1633) --------------------------------------------
    void reconcileIcePeers(const Json& rs)
    {
        const uint64_t id = rs["id"].toU64();
        if (matchTransportActive)
        {
            if (id != iceRoomId)
                return;
            iceOutsideMatch.clear();
            std::set<uint64_t> seated;
            for (const Json& p : rs["players"].elements())
            {
                const uint64_t uid = p["userId"].toU64();
                if (uid == 0 || uid == selfId)
                    continue;
                seated.insert(uid);
                if (!p["inMatch"].toBool(true))
                    iceOutsideMatch.insert(uid);
                if (p["iceSupported"].toBool())
                    ensureIcePeer(uid);
            }
            std::vector<uint64_t> gone;
            for (uint64_t uid : icePeers)
                if (!seated.count(uid))
                    gone.push_back(uid);
            for (uint64_t uid : gone)
                removeIcePeer(uid);
            return;
        }
        if (id == 0)
            return;
        if (iceRoomId != 0 && iceRoomId != id)
            resetIceMesh();
        iceRoomId = id;
        const uint64_t host = rs["hostId"].toU64();
        if (iceHostId != 0 && iceHostId != host)
            roomPeerPings.clear();
        iceHostId = host;

        std::set<uint64_t> desired;
        for (const Json& p : rs["players"].elements())
        {
            const uint64_t uid = p["userId"].toU64();
            if (uid == 0 || uid == selfId)
                continue;
            if (!p["iceSupported"].toBool())
            {
                log("lobby: peer %llu does not support ICE", (unsigned long long)uid);
                continue;
            }
            desired.insert(uid);
            if (!icePeers.count(uid))
            {
                iceGeneration.emplace(uid, 0);
                iceRestarts.emplace(uid, 0);
                if (mesh.addPeer(uid))
                {
                    icePeers.insert(uid);
                    peerIceEvent(uid, 0);
                    log("lobby: ICE agent for peer %llu (room %llu)", (unsigned long long)uid,
                        (unsigned long long)id);
                }
                else
                    log("lobby: could not create an ICE agent for peer %llu", (unsigned long long)uid);
            }
        }
        std::vector<uint64_t> stale;
        for (uint64_t uid : icePeers)
            if (!desired.count(uid))
                stale.push_back(uid);
        for (uint64_t uid : stale)
            removeIcePeer(uid);
    }

    void ensureIcePeer(uint64_t uid)
    {
        if (uid == 0 || uid == selfId || icePeers.count(uid) || iceRoomId == 0)
            return;
        iceGeneration.emplace(uid, 0);
        iceRestarts.emplace(uid, 0);
        if (mesh.addPeer(uid))
        {
            icePeers.insert(uid);
            peerIceEvent(uid, 0);
        }
    }

    void peerIceEvent(uint64_t uid, int value)
    {
        auto& ev = push(ZNX_LOBBY_EV_PEER_ICE);
        ev.user_id = uid;
        ev.value = value;
        ev.value2 = iceRestarts[uid] + 1;
    }

    void removeIcePeer(uint64_t uid)
    {
        mesh.removePeer(uid);
        icePeers.erase(uid);
        lastIceState.erase(uid);
        iceGeneration.erase(uid);
        iceRestarts.erase(uid);
        roomPeerPings.erase(uid);
        for (auto& kv : roomPeerPings)
            kv.second.erase(uid);
    }

    bool iceAgentInUseByMatch(uint64_t uid) const
    {
        return matchTransportActive && !iceOutsideMatch.count(uid);
    }

    void resetIceMesh()
    {
        if (matchTransportActive)
        {
            deferredMeshReset = true;
            return;
        }
        for (uint64_t uid : icePeers)
            mesh.removePeer(uid);
        icePeers.clear();
        lastIceState.clear();
        iceGeneration.clear();
        iceRestarts.clear();
        iceRoomId = 0;
        iceHostId = 0;
        roomPeerPings.clear();
        probes.clear();
    }

    void resetIceTransport()
    {
        if (matchTransportActive)
        {
            deferredTransportReset = true;
            return;
        }
        resetIceMesh();
        mesh.reset();
        deferredMeshReset = deferredTransportReset = false;
    }

    void setMatchTransportActive(bool active)
    {
        if (matchTransportActive == active)
            return;
        matchTransportActive = active;
        iceOutsideMatch.clear();
        if (active)
            return;
        if (deferredTransportReset)
            resetIceTransport();
        else if (deferredMeshReset)
        {
            deferredMeshReset = false;
            resetIceMesh();
        }
    }

    void iceSignal(const Json& d)   // LobbyClient.cpp:1323-1413
    {
        const uint64_t rid = d["roomId"].toU64();
        const uint64_t from = d["fromUserId"].toU64();
        const std::string kind = d["kind"].str();
        const uint32_t generation = static_cast<uint32_t>(d["generation"].toInt());
        if (rid == 0 || rid != iceRoomId || from == 0 || from == selfId)
            return;
        const uint32_t current = iceGeneration.count(from) ? iceGeneration[from] : 0;
        if (generation < current)
            return;
        if (iceAgentInUseByMatch(from) && (kind == "restart" || generation > current))
            return;
        if (kind == "restart")
        {
            if (generation == 0 || generation == current)
                return;
            if (!icePeers.count(from))
            {
                mesh.removePeer(from);
                iceGeneration[from] = generation;
                iceRestarts[from] = iceRestarts[from] + 1;
                return;
            }
            restartIcePeer(from, generation, false);
            return;
        }
        if (generation > current)
        {
            if (icePeers.count(from))
                restartIcePeer(from, generation, false);
            else
            {
                mesh.removePeer(from);
                iceGeneration[from] = generation;
            }
        }
        mesh.handleRemoteSignal(from, kind, d["value"].str());
    }

    bool restartIcePeer(uint64_t uid, uint32_t generation, bool notifyPeer)
    {
        if (iceAgentInUseByMatch(uid) || iceRoomId == 0 || !icePeers.count(uid) || generation == 0 ||
            generation <= iceGeneration[uid] || iceRestarts[uid] >= kMaxIceAutoRestarts)
            return false;
        if (notifyPeer)
        {
            Json d = Json::object();
            d.set("targetUserId", Json::uinteger(uid));
            d.set("roomId", Json::uinteger(iceRoomId));
            d.set("kind", Json::string("restart"));
            d.set("generation", Json::integer(generation));
            sendEnvelope("ICE_SIGNAL", d);
        }
        iceGeneration[uid] = generation;
        iceRestarts[uid] = iceRestarts[uid] + 1;
        lastIceState.erase(uid);
        peerIceEvent(uid, 0);
        for (auto it = probes.begin(); it != probes.end();)
            it = it->second.target == uid ? probes.erase(it) : std::next(it);
        mesh.removePeer(uid);
        const bool added = mesh.addPeer(uid);
        log("lobby: ICE restart for peer %llu, generation %u: %s", (unsigned long long)uid, generation,
            added ? "ok" : "failed");
        if (!added)
            peerIceEvent(uid, -1);
        return added;
    }

    void sendIcePing(uint64_t uid, uint64_t nonce)
    {
        std::string p(1, char(kPingRequest));
        putBe64(p, nonce);
        mesh.send(uid, uint8_t(IceChannel::Ping), p.data(), p.size());
    }

    void sendRoomPingReport(uint64_t target, int rtt)   // LobbyClient.cpp:1475-1495
    {
        if (iceHostId == 0 || iceHostId == selfId || target == 0 || target == iceHostId ||
            selfId >= target || !icePeers.count(target))
            return;
        std::string p(1, char(kPingReport));
        putBe64(p, target);
        const int v = std::max(0, std::min(65535, rtt));
        p.push_back(char((v >> 8) & 0xff));
        p.push_back(char(v & 0xff));
        mesh.send(iceHostId, uint8_t(IceChannel::Ping), p.data(), p.size());
    }

    void requestPingProbe(uint64_t target)   // LobbyClient.cpp:3463-3488
    {
        if (target == 0 || target == selfId || !mesh.peerConnected(target))
            return;
        for (const auto& kv : probes)
            if (kv.second.target == target)
                return;
        uint64_t nonce = random_u64();
        while (nonce == 0 || probes.count(nonce))
            nonce = random_u64();
        const uint64_t now = now_ms();
        Probe p;
        p.target = target;
        p.sendMs = p.attemptSendMs = now;
        p.nextAttemptMs = now + kProbeRetryMs;
        probes[nonce] = p;
        sendIcePing(target, nonce);
    }

    void flushIce()   // LobbyClient.cpp:1497-1633
    {
        if (ws.isOpen() && iceRoomId != 0)
        {
            for (IceSignal& s : mesh.takeLocalSignals())
            {
                Json d = Json::object();
                d.set("targetUserId", Json::uinteger(s.peerUserId));
                d.set("roomId", Json::uinteger(iceRoomId));
                d.set("kind", Json::string(s.kind));
                d.set("generation", Json::integer(iceGeneration.count(s.peerUserId) ? iceGeneration[s.peerUserId] : 0));
                if (!s.value.empty())
                    d.set("value", Json::string(s.value));
                sendEnvelope("ICE_SIGNAL", d);
            }
        }

        for (IcePacket& pk : mesh.takePackets(uint8_t(IceChannel::Ping)))
        {
            if (pk.data.size() == 11 && uint8_t(pk.data[0]) == kPingReport)
            {
                const uint64_t target = getBe64(pk.data.data() + 1);
                const int rtt = (uint8_t(pk.data[9]) << 8) | uint8_t(pk.data[10]);
                if (selfId == iceHostId && icePeers.count(pk.peerUserId) && icePeers.count(target) &&
                    pk.peerUserId < target)
                    roomPeerPings[pk.peerUserId][target] = rtt;
                continue;
            }
            if (pk.data.size() != 9)
                continue;
            const uint64_t nonce = getBe64(pk.data.data() + 1);
            if (uint8_t(pk.data[0]) == kPingRequest)
            {
                std::vector<char> reply = pk.data;
                reply[0] = char(kPingReply);
                mesh.send(pk.peerUserId, uint8_t(IceChannel::Ping), reply.data(), reply.size());
                continue;
            }
            if (uint8_t(pk.data[0]) != kPingReply)
                continue;
            auto it = probes.find(nonce);
            if (it == probes.end() || it->second.target != pk.peerUserId)
                continue;
            const int rtt = pk.roundTripUs
                ? int(std::min<uint64_t>((pk.roundTripUs + 500) / 1000, 65535))
                : int(std::min<uint64_t>(now_ms() - it->second.attemptSendMs, 65535));
            probes.erase(it);
            measuredPing[pk.peerUserId] = rtt;
            sendRoomPingReport(pk.peerUserId, rtt);
            auto& ev = push(ZNX_LOBBY_EV_PEER_PING);
            ev.user_id = pk.peerUserId;
            ev.value = rtt;
            applyLocalDelay(false);
        }

        std::vector<uint64_t> toRestart;
        for (uint64_t uid : std::vector<uint64_t>(icePeers.begin(), icePeers.end()))
        {
            const IceState st = mesh.peerState(uid);
            auto last = lastIceState.find(uid);
            if (last != lastIceState.end() && last->second == int(st))
                continue;
            lastIceState[uid] = int(st);
            log("lobby: ICE peer %llu %s%s%s", (unsigned long long)uid, iceStateName(st),
                mesh.peerConnected(uid) ? " " : "", mesh.peerSelectedAddresses(uid).c_str());
            const bool connected = mesh.peerConnected(uid);
            const bool failed = st == IceState::Failed;
            if (failed && iceRestarts[uid] < kMaxIceAutoRestarts)
            {
                peerIceEvent(uid, 0);
                toRestart.push_back(uid);
                continue;
            }
            if (st == IceState::Completed)
                iceRestarts[uid] = 0;
            peerIceEvent(uid, connected ? 1 : failed ? -1 : 0);
            if (connected)
            {
                requestPingProbe(uid);   // RollbackLobbyDialog.cpp:4740-4751
                if (uid == iceHostId)
                    for (const auto& kv : measuredPing)
                        sendRoomPingReport(kv.first, kv.second);
            }
        }
        for (uint64_t uid : toRestart)
            restartIcePeer(uid, iceGeneration[uid] + 1, true);
    }

    void probeTimers(uint64_t now)   // LobbyClient.cpp:1907-1949
    {
        for (auto it = probes.begin(); it != probes.end();)
        {
            if (now < it->second.nextAttemptMs)
            {
                ++it;
                continue;
            }
            if (it->second.attempt >= kProbeAttempts)
            {
                it = probes.erase(it);
                continue;
            }
            it->second.attempt += 1;
            it->second.attemptSendMs = now;
            it->second.nextAttemptMs = now + kProbeRetryMs;
            sendIcePing(it->second.target, it->first);
            ++it;
        }
        if (now >= nextPingTick)
        {
            nextPingTick = now + kPingTickMs;
            if (roomId != 0)
                for (const Json& p : roomState["players"].elements())
                {
                    const uint64_t uid = p["userId"].toU64();
                    if (uid && uid != selfId)
                        requestPingProbe(uid);
                }
        }
    }

    int worstRoomPing(bool& complete) const   // LobbyClient.cpp:3230-3270
    {
        std::vector<uint64_t> ids;
        for (const Json& p : roomState["players"].elements())
            if (uint64_t uid = p["userId"].toU64())
                ids.push_back(uid);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        complete = ids.size() >= 2;
        int worst = -1;
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j)
            {
                int ping = -1;
                if (ids[i] == selfId || ids[j] == selfId)
                {
                    const uint64_t remote = ids[i] == selfId ? ids[j] : ids[i];
                    auto it = measuredPing.find(remote);
                    ping = it == measuredPing.end() ? -1 : it->second;
                }
                else
                {
                    auto a = roomPeerPings.find(ids[i]);
                    if (a != roomPeerPings.end())
                    {
                        auto b = a->second.find(ids[j]);
                        if (b != a->second.end())
                            ping = b->second;
                    }
                }
                if (ping < 0)
                    complete = false;
                else
                    worst = std::max(worst, ping);
            }
        return worst;
    }

    bool canStart() const   // RollbackLobbyDialog.cpp:4370-4399
    {
        if (conn != Conn::Connected || roomId == 0 || roomState["hostId"].toU64() != selfId ||
            roomState["state"].str() != "waiting")
            return false;
        int seated = 0;
        bool saveDataBlocked = false;
        const bool saveDataOn = roomState["saveData"]["enabled"].toBool();
        for (const Json& p : roomState["players"].elements())
        {
            if (!p["userId"].toU64())
                continue;
            ++seated;
            if (saveDataOn && !p["saveData"].toBool())
                saveDataBlocked = true;
        }
        const bool alone = seated == 1 && roomState["canStartAlone"].toBool();
        bool pingsReady = alone;
        if (!alone)
            worstRoomPing(pingsReady);
        return (seated >= 2 || alone) && pingsReady && !saveDataBlocked;
    }

    // ---- match (RollbackLobbyDialog.cpp:5765-6000, LobbyClient.cpp:2530-2749) -------------
    void matchBegin(const Json& d)
    {
        matchId = d["matchId"].toU64();
        matchPeers.clear();
        for (const Json& o : d["peers"].elements())
        {
            MatchPeer p;
            p.userId = o["userId"].toU64();
            p.username = o["username"].str();
            p.slot = o["slot"].toInt();
            p.iceSupported = o["iceSupported"].toBool();
            matchPeers.push_back(p);
        }
        matchSeats = std::max(0, std::min(4, d["seats"].toInt()));
        matchHostId = roomState["hostId"].toU64();
        matchPrediction = std::max(1, std::min(9, roomState["prediction"].toInt(kDefaultPrediction)));
        matchPacing = roomState["pacing"].toInt() == 1 ? 1 : 0;
        phase = MatchPhase::WaitIce;
        phaseDeadline = now_ms() + kMatchIceWaitMs;
        log("lobby: MATCH_BEGIN match %llu with %zu player(s)", (unsigned long long)matchId, matchPeers.size());
        push(ZNX_LOBBY_EV_MATCH_BEGIN).match_id = matchId;
    }

    void abortMatch(const std::string& reason)   // RollbackLobbyDialog.cpp:5734-5763
    {
        log("lobby: match start failed: %s", reason.c_str());
        const uint64_t id = matchId;
        phase = MatchPhase::None;
        matchId = 0;
        setMatchTransportActive(false);
        if (id)
        {
            Json d = Json::object();
            d.set("matchId", Json::uinteger(id));
            sendEnvelope("MATCH_FINISHED", d);
        }
        auto& ev = push(ZNX_LOBBY_EV_MATCH_FAILED);
        ev.match_id = id;
        copyStr(ev.text, sizeof(ev.text), reason);
    }

    bool allIcePeersConnected(std::string& error)   // LobbyClient.cpp:3272-3298
    {
        for (const MatchPeer& p : matchPeers)
        {
            if (p.userId == selfId)
                continue;
            const std::string who = p.username.empty() ? std::to_string(p.userId) : p.username;
            if (!p.iceSupported)
            {
                error = who + " is using a lobby client without ICE support.";
                return false;
            }
            if (!mesh.peerConnected(p.userId))
            {
                error = "Waiting for a direct ICE connection to " + who + ".";
                return false;
            }
        }
        return true;
    }

    void startPrematch()
    {
        const MatchPeer* local = nullptr;
        for (const MatchPeer& p : matchPeers)
            if (p.userId == selfId)
                local = &p;
        if (!local)
        {
            abortMatch("Match start failed: missing local peer.");
            return;
        }
        if (matchPeers.size() < 2 && matchSeats == 0)
        {
            abortMatch("Match start failed: missing remote peer.");
            return;
        }
        bool hostFound = false;
        for (const MatchPeer& p : matchPeers)
            if (matchHostId ? p.userId == matchHostId : p.slot == 1)
            {
                matchHostId = p.userId;
                hostFound = true;
            }
        if (!hostFound)
        {
            abortMatch("Pre-match sync failed: incomplete peer list");
            return;
        }
        matchLocalSlot = local->slot;
        if (matchPeers.size() == 1 && delayAuto)
            localDelay = 0;   // playing alone (RollbackLobbyDialog.cpp:5807-5812)
        matchDelay = localDelay;
        mesh.takePackets(uint8_t(IceChannel::Prematch));
        phase = MatchPhase::Prematch;
        phaseDeadline = now_ms() + kPrematchTimeoutMs;
        fragHash = 0;
        fragCount = 0;
        fragTotal = 0;
        fragParts.clear();
        outbound.clear();
        pendingAcks.clear();
        if (matchHostId == selfId)
        {
            matchManifest = prematchManifest;
            matchManifestHash = fnv1a64(matchManifest);
            std::string packet(kAnchorMagic, 4);
            packet.push_back(char(kOpPrematchManifest));
            putBe64(packet, selfId);
            putLe(packet, matchManifestHash, 8);
            packet += matchManifest;
            if (packet.size() <= kFragmentPayload)
                outbound.push_back(packet);
            else
            {
                const size_t count = (packet.size() + kFragmentPayload - 1) / kFragmentPayload;
                if (count > 2048)
                {
                    abortMatch("Pre-match sync failed: settings manifest is too large.");
                    return;
                }
                for (size_t i = 0; i < count; ++i)   // LobbyClient.cpp:443-471
                {
                    std::string f(kAnchorMagic, 4);
                    f.push_back(char(kOpPrematchFragment));
                    putBe64(f, selfId);
                    putLe(f, matchManifestHash, 8);
                    putLe(f, i, 2);
                    putLe(f, count, 2);
                    putLe(f, packet.size(), 4);
                    f += packet.substr(i * kFragmentPayload, kFragmentPayload);
                    outbound.push_back(f);
                }
            }
            for (const MatchPeer& p : matchPeers)
                if (p.userId != selfId)
                    pendingAcks.insert(p.userId);
            nextPrematchSend = 0;
        }
        log("lobby: pre-match sync as %s", matchHostId == selfId ? "host" : "player");
    }

    void finishPrematch()
    {
        phase = MatchPhase::Ready;
        // From here until match_finished the game owns the agents (RollbackLobbyDialog.cpp:5995-5999).
        setMatchTransportActive(true);
        auto& ev = push(ZNX_LOBBY_EV_MATCH_READY);
        ev.match_id = matchId;
        ev.value = matchLocalSlot;
        log("lobby: pre-match sync complete (manifest %zu bytes, hash %016llx); slot %d delay %d prediction %d",
            matchManifest.size(), (unsigned long long)matchManifestHash, matchLocalSlot, matchDelay, matchPrediction);
    }

    void prematchStep(uint64_t now)
    {
        if (now > phaseDeadline)
        {
            abortMatch(matchHostId == selfId
                ? "Pre-match sync timed out - a player's direct ICE connection stopped responding."
                : "Pre-match sync timed out - never heard from the host over ICE.");
            return;
        }
        if (matchHostId == selfId)
        {
            if (now >= nextPrematchSend)
            {
                for (uint64_t uid : pendingAcks)
                    for (const std::string& o : outbound)
                        mesh.send(uid, uint8_t(IceChannel::Prematch), o.data(), o.size());
                nextPrematchSend = now + kPrematchResendMs;
            }
            for (IcePacket& pk : mesh.takePackets(uint8_t(IceChannel::Prematch)))
            {
                if (pk.data.size() < 21 || std::memcmp(pk.data.data(), kAnchorMagic, 4) != 0 ||
                    uint8_t(pk.data[4]) != kOpPrematchAck)
                    continue;
                if (getBe64(pk.data.data() + 5) == pk.peerUserId && getLe(pk.data.data() + 13, 8) == matchManifestHash)
                    pendingAcks.erase(pk.peerUserId);
            }
            if (pendingAcks.empty())
                finishPrematch();
            return;
        }
        for (IcePacket& pk : mesh.takePackets(uint8_t(IceChannel::Prematch)))
        {
            if (pk.peerUserId != matchHostId)
                continue;
            std::string dg(pk.data.begin(), pk.data.end());
            if (dg.size() >= 29 && std::memcmp(dg.data(), kAnchorMagic, 4) == 0 &&
                uint8_t(dg[4]) == kOpPrematchFragment)
            {
                const uint64_t sender = getBe64(dg.data() + 5);
                const uint64_t hash = getLe(dg.data() + 13, 8);
                const unsigned index = unsigned(getLe(dg.data() + 21, 2));
                const unsigned count = unsigned(getLe(dg.data() + 23, 2));
                const uint32_t total = uint32_t(getLe(dg.data() + 25, 4));
                if (sender != matchHostId || count == 0 || count > 2048 || index >= count || total == 0 ||
                    total > 2u * 1024 * 1024)
                    continue;
                if (fragHash != hash || fragCount != int(count) || fragTotal != total)
                {
                    fragHash = hash;
                    fragCount = int(count);
                    fragTotal = total;
                    fragParts.assign(count, std::string());
                }
                fragParts[index] = dg.substr(29);
                bool complete = true;
                for (const std::string& part : fragParts)
                    complete = complete && !part.empty();
                if (!complete)
                    continue;
                dg.clear();
                for (const std::string& part : fragParts)
                    dg += part;
                if (dg.size() != fragTotal)
                    continue;
            }
            if (dg.size() < 21 || std::memcmp(dg.data(), kAnchorMagic, 4) != 0 || uint8_t(dg[4]) != kOpPrematchManifest)
                continue;
            if (getBe64(dg.data() + 5) != matchHostId)
                continue;
            const uint64_t hash = getLe(dg.data() + 13, 8);
            if (fragHash != 0 && hash != fragHash)
                continue;
            std::string manifest = dg.substr(21);
            if (fnv1a64(manifest) != hash)
            {
                abortMatch("Pre-match sync failed: settings manifest was corrupted in transit.");
                return;
            }
            if (!manifestWellFormed(manifest))
            {
                abortMatch("Pre-match sync failed: invalid settings manifest.");
                return;
            }
            matchManifest = std::move(manifest);
            matchManifestHash = hash;
            std::string ack(kAnchorMagic, 4);
            ack.push_back(char(kOpPrematchAck));
            putBe64(ack, selfId);
            putLe(ack, hash, 8);
            for (int i = 0; i < kPrematchAckRepeats; ++i)
                mesh.send(matchHostId, uint8_t(IceChannel::Prematch), ack.data(), ack.size());
            finishPrematch();
            return;
        }
    }

    void matchStep(uint64_t now)
    {
        if (phase == MatchPhase::WaitIce)
        {
            std::string error;
            if (allIcePeersConnected(error))
            {
                startPrematch();
                return;
            }
            bool unsupported = false;
            for (const MatchPeer& p : matchPeers)
                unsupported = unsupported || (p.userId != selfId && !p.iceSupported);
            if (unsupported)
                abortMatch("Match start failed: " + error);
            else if (now >= phaseDeadline)
                abortMatch("Match start failed: direct ICE negotiation timed out. No TURN relay is configured.");
        }
        else if (phase == MatchPhase::Prematch)
            prematchStep(now);
    }

    // ---- poll ------------------------------------------------------------------------------
    void step()
    {
        WsEvent wev;
        while (ws.poll(wev))
        {
            if (wev.type == WsEvent::Opened)
                onOpened();
            else if (wev.type == WsEvent::Message)
                handle(wev.data);
            else
                onClosed(wev.data);
        }
        const uint64_t now = now_ms();
        if (conn == Conn::Connected && now >= nextHeartbeat)
        {
            nextHeartbeat = now + kHeartbeatMs;
            Json d = Json::object();
            d.set("away", Json::boolean(false));
            sendEnvelope("HEARTBEAT", d);
        }
        if (selfId != 0)
        {
            flushIce();
            probeTimers(now);
            matchStep(now);
        }
    }
};

// =====================================================================================
extern "C" {

znx_lobby* znx_lobby_create(const znx_lobby_config* config)
{
    auto* lb = new znx_lobby();
    if (config)
    {
        if (config->stun_host)
        {
            lb->stunOverridden = true;
            lb->stunHostOverride = config->stun_host;
            lb->stunPortOverride = static_cast<uint16_t>(config->stun_port > 0 ? config->stun_port : 19302);
        }
        lb->useServerTurn = config->use_server_turn != 0;
        if (config->bind_address) lb->bindAddress = config->bind_address;
        if (config->local_ip) lb->localIp = config->local_ip;
        if (config->connection) lb->connection = config->connection;
        lb->tzOffset = config->tz_offset_sec;
        lb->anonymous = config->anonymous != 0;
        lb->saveDataCapable = config->save_data != 0;
        lb->logFn = config->log;
        lb->logUser = config->log_user;
    }
    return lb;
}

void znx_lobby_destroy(znx_lobby* lobby)
{
    if (!lobby)
        return;
    lobby->matchTransportActive = false;
    lobby->disconnect();
    delete lobby;
}

int znx_lobby_connect(znx_lobby* lobby, const char* server_url, const char* username, const char* rom_md5,
                      const char* client_version)
{
    return lobby ? lobby->connect(server_url, username, rom_md5, client_version) : -1;
}

void znx_lobby_disconnect(znx_lobby* lobby)
{
    if (lobby)
        lobby->disconnect();
}

int znx_lobby_connected(const znx_lobby* lobby)
{
    return lobby && lobby->conn == znx_lobby::Conn::Connected;
}

uint64_t znx_lobby_self_id(const znx_lobby* lobby)
{
    return lobby ? lobby->selfId : 0;
}

int znx_lobby_poll(znx_lobby* lobby, znx_lobby_event* ev)
{
    if (!lobby)
        return 0;
    if (lobby->events.empty())
        lobby->step();
    if (lobby->events.empty())
        return 0;
    if (ev)
        *ev = lobby->events.front();
    lobby->events.pop_front();
    return 1;
}

int znx_lobby_user_count(const znx_lobby* lobby)
{
    return lobby ? int(lobby->users.size()) : 0;
}

int znx_lobby_user(const znx_lobby* lobby, int index, znx_lobby_user_info* out)
{
    if (!lobby || !out || index < 0 || index >= int(lobby->users.size()))
        return -1;
    auto it = lobby->users.begin();
    std::advance(it, index);
    *out = it->second;
    return 0;
}

int znx_lobby_room_count(const znx_lobby* lobby)
{
    return lobby ? int(lobby->rooms.size()) : 0;
}

int znx_lobby_room(const znx_lobby* lobby, int index, znx_lobby_room_info* out)
{
    if (!lobby || !out || index < 0 || index >= int(lobby->rooms.size()))
        return -1;
    *out = lobby->rooms[size_t(index)];
    return 0;
}

int znx_lobby_current_room(const znx_lobby* lobby, znx_lobby_room_detail* out)
{
    if (!lobby || !out)
        return -1;
    std::memset(out, 0, sizeof(*out));
    if (lobby->roomId == 0 || !lobby->roomSeen)
        return -1;
    auto* lb = const_cast<znx_lobby*>(lobby);
    const Json& rs = lobby->roomState;
    out->id = lobby->roomId;
    copyStr(out->name, sizeof(out->name), rs["name"].str());
    out->host_id = rs["hostId"].toU64();
    copyStr(out->rom_name, sizeof(out->rom_name), rs["rom"]["name"].str());
    copyStr(out->rom_md5, sizeof(out->rom_md5), rs["rom"]["md5"].str());
    copyStr(out->rom_region, sizeof(out->rom_region), rs["rom"]["region"].str());
    out->max_players = rs["maxPlayers"].toInt();
    copyStr(out->state, sizeof(out->state), rs["state"].str());
    out->prediction = std::max(1, std::min(9, rs["prediction"].toInt(kDefaultPrediction)));
    out->prediction_auto = rs["predictionAuto"].toBool(true);
    out->pacing = rs["pacing"].toInt() == 1 ? 1 : 0;
    for (const Json& p : rs["players"].elements())
    {
        const int slot = p["slot"].toInt();
        if (slot < 1 || slot > 4)
            continue;
        znx_lobby_seat& s = out->seats[slot - 1];
        s.user_id = p["userId"].toU64();
        copyStr(s.username, sizeof(s.username), p["username"].str());
        const bool self = s.user_id == lobby->selfId;
        s.frame_delay = self ? lobby->localDelay : p["frameDelay"].toInt(-1);
        if (self)
        {
            s.ice = 1;
            s.ping_ms = 0;
        }
        else
        {
            const IceState st = lb->mesh.peerState(s.user_id);
            s.ice = lb->mesh.peerConnected(s.user_id) ? 1 : st == IceState::Failed ? -1 : 0;
            auto it = lobby->measuredPing.find(s.user_id);
            s.ping_ms = it == lobby->measuredPing.end() ? -1 : it->second;
        }
    }
    out->local_delay = lobby->localDelay;
    out->local_delay_auto = lobby->delayAuto;
    return 0;
}

int znx_lobby_create_room(znx_lobby* lobby, const char* name, const char* rom_name, const char* rom_md5,
                          const char* rom_region, int max_players, int delay, int prediction, const char* password)
{
    if (!lobby || lobby->conn != znx_lobby::Conn::Connected)
        return -1;
    Json rom = Json::object();   // LobbyClient.cpp:3359-3379
    rom.set("name", Json::string(rom_name ? rom_name : ""));
    rom.set("md5", Json::string(rom_md5 ? rom_md5 : lobby->romMd5));
    rom.set("region", Json::string(rom_region ? rom_region : ""));
    Json d = Json::object();
    d.set("name", Json::string(name ? name : ""));
    d.set("rom", rom);
    d.set("maxPlayers", Json::integer(std::max(2, std::min(4, max_players))));
    d.set("delay", Json::integer(delay));
    d.set("prediction", Json::integer(prediction));
    d.set("pacing", Json::integer(1));   // fixed to Smooth (RollbackLobbyDialog.cpp:3374-3376)
    if (password && *password)
        d.set("password", Json::string(password));
    return lobby->sendEnvelope("ROOM_CREATE", d) ? 0 : -1;
}

int znx_lobby_join_room(znx_lobby* lobby, uint64_t room_id, const char* password)
{
    if (!lobby)
        return -1;
    Json d = Json::object();
    d.set("roomId", Json::uinteger(room_id));
    if (password && *password)
        d.set("password", Json::string(password));
    return lobby->sendEnvelope("ROOM_JOIN", d) ? 0 : -1;
}

int znx_lobby_leave_room(znx_lobby* lobby)
{
    return lobby && lobby->sendEnvelope("ROOM_LEAVE") ? 0 : -1;
}

int znx_lobby_send_chat(znx_lobby* lobby, const char* channel, const char* text)
{
    if (!lobby || !text)
        return -1;
    Json d = Json::object();
    d.set("channel", Json::string(channel ? channel : "room"));
    d.set("message", Json::string(text));
    return lobby->sendEnvelope("CHAT_SEND", d) ? 0 : -1;
}

int znx_lobby_set_delay(znx_lobby* lobby, int delay)
{
    if (!lobby)
        return -1;
    lobby->delayAuto = delay < 0;
    if (delay >= 0)
        lobby->localDelay = std::min(delay, 9);
    lobby->applyLocalDelay(true);
    return 0;
}

int znx_lobby_set_prediction(znx_lobby* lobby, int prediction)
{
    if (!lobby)
        return -1;
    Json d = Json::object();   // LobbyClient.cpp:3437-3443
    d.set("prediction", Json::integer(prediction <= 0 ? kDefaultPrediction : std::min(prediction, 9)));
    d.set("predictionAuto", Json::boolean(prediction <= 0));
    return lobby->sendEnvelope("ROOM_UPDATE_PREDICTION", d) ? 0 : -1;
}

int znx_lobby_can_start(const znx_lobby* lobby)
{
    return lobby && lobby->canStart();
}

int znx_lobby_start_match(znx_lobby* lobby)
{
    return lobby && lobby->sendEnvelope("ROOM_START") ? 0 : -1;
}

int znx_lobby_swap_seats(znx_lobby* lobby, int slot_a, int slot_b)
{
    if (!lobby)
        return -1;
    Json d = Json::object();
    d.set("slotA", Json::integer(slot_a));
    d.set("slotB", Json::integer(slot_b));
    return lobby->sendEnvelope("ROOM_SWAP_SLOTS", d) ? 0 : -1;
}

int znx_lobby_kick(znx_lobby* lobby, uint64_t user_id)
{
    if (!lobby)
        return -1;
    Json d = Json::object();
    d.set("userId", Json::uinteger(user_id));
    return lobby->sendEnvelope("ROOM_KICK", d) ? 0 : -1;
}

int znx_lobby_set_prematch_manifest(znx_lobby* lobby, const uint8_t* data, size_t size)
{
    if (!lobby)
        return -1;
    if (!data || size == 0)
    {
        lobby->prematchManifest = emptyManifest();
        return 0;
    }
    std::string m(reinterpret_cast<const char*>(data), size);
    if (!manifestWellFormed(m))
        return -1;
    lobby->prematchManifest = std::move(m);
    return 0;
}

int znx_lobby_match(const znx_lobby* lobby, znx_lobby_match_info* out)
{
    if (!lobby || !out || (lobby->phase != znx_lobby::MatchPhase::Ready &&
                           lobby->phase != znx_lobby::MatchPhase::Running))
        return -1;
    std::memset(out, 0, sizeof(*out));
    out->match_id = lobby->matchId;
    out->host_id = lobby->matchHostId;
    out->local_slot = lobby->matchLocalSlot;
    out->seats = lobby->matchSeats;
    int total = std::max(lobby->matchLocalSlot, lobby->matchSeats);   // Emulation.cpp:1188-1202
    for (const auto& p : lobby->matchPeers)
    {
        if (p.userId == lobby->selfId)
            continue;
        total = std::max(total, p.slot);
        if (out->num_peers < 3)
        {
            znx_lobby_match_peer& mp = out->peers[out->num_peers++];
            mp.user_id = p.userId;
            mp.slot = p.slot;
            copyStr(mp.username, sizeof(mp.username), p.username);
        }
    }
    out->total_seats = total;
    out->local_delay = lobby->matchDelay;
    out->prediction = lobby->matchPrediction;
    out->pacing = lobby->matchPacing;
    out->manifest = reinterpret_cast<const uint8_t*>(lobby->matchManifest.data());
    out->manifest_size = lobby->matchManifest.size();
    out->manifest_hash = lobby->matchManifestHash;
    return 0;
}

void znx_lobby_match_started(znx_lobby* lobby)
{
    if (!lobby || lobby->phase != znx_lobby::MatchPhase::Ready)
        return;
    lobby->phase = znx_lobby::MatchPhase::Running;
    Json d = Json::object();   // RollbackLobbyDialog.cpp:4472, LobbyClient.cpp:3490-3496
    d.set("matchId", Json::uinteger(lobby->matchId));
    d.set("peerUserId", Json::uinteger(0));
    lobby->sendEnvelope("MATCH_CONNECTED", d);
}

void znx_lobby_match_finished(znx_lobby* lobby)
{
    if (!lobby || lobby->matchId == 0)
        return;
    const uint64_t id = lobby->matchId;
    lobby->phase = znx_lobby::MatchPhase::None;
    lobby->matchId = 0;
    lobby->setMatchTransportActive(false);
    Json d = Json::object();
    d.set("matchId", Json::uinteger(id));
    lobby->sendEnvelope("MATCH_FINISHED", d);
}

int znx_lobby_send(znx_lobby* lobby, uint64_t user_id, int channel, const void* data, size_t size)
{
    if (!lobby || channel < 0 || channel > 255)
        return -1;
    return lobby->mesh.send(user_id, uint8_t(channel), data, size) ? 0 : -1;
}

int znx_lobby_receive(znx_lobby* lobby, int channel, uint64_t* from_user_id, void* buf, size_t cap)
{
    if (!lobby || channel < 0 || channel > 255)
        return -1;
    // takePackets drains a channel; the rest waits here for the next call.
    std::lock_guard<std::mutex> lock(lobby->rxMutex);
    std::deque<IcePacket>& q = lobby->rxPending[channel];
    if (q.empty())
        for (IcePacket& pk : lobby->mesh.takePackets(uint8_t(channel)))
            q.push_back(std::move(pk));
    if (q.empty())
        return -1;
    IcePacket pk = std::move(q.front());
    q.pop_front();
    if (from_user_id)
        *from_user_id = pk.peerUserId;
    if (buf && cap)
        std::memcpy(buf, pk.data.data(), std::min(cap, pk.data.size()));
    return int(pk.data.size());
}

int znx_lobby_peer_connected(znx_lobby* lobby, uint64_t user_id)
{
    return lobby && lobby->mesh.peerConnected(user_id) ? 1 : 0;
}

int znx_lobby_peer_address(uint64_t user_id, char* out, size_t cap)
{
    if (!out || cap == 0)
        return -1;
    const int n = std::snprintf(out, cap, "ice:%llu", (unsigned long long)user_id);
    return (n < 0 || size_t(n) >= cap) ? -1 : n;
}

} // extern "C"
