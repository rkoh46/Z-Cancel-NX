// SPDX-License-Identifier: GPL-3.0-or-later
/* Follows RMG-K's Source/RMG-Core/LobbyIce.cpp (GPL-3.0, Rosalie Wanders and the RMG-K authors)
 * closely, method for method; see ice_mesh.h for what differs. */
#include "ice_mesh.h"

#include <juice/juice.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

#include "net_io.h"

namespace znxlobby
{

struct IceMesh::Peer
{
    IceMesh* mesh = nullptr;
    uint64_t userId = 0;
    juice_agent_t* agent = nullptr;
    std::mutex agentMutex;
    std::atomic<IceState> state{IceState::Disconnected};
    std::atomic<bool> active{true};
    std::mutex pingMutex;
    std::map<uint64_t, uint64_t> pingSentAtUs;
    bool hasRemoteDescription = false;
    bool remoteGatheringDone = false;
    std::vector<std::string> pendingCandidates;
};

namespace
{
constexpr size_t kMaxQueuedPackets = 8192;
constexpr size_t kMaxPayload = 4000;

uint64_t read_be_u64(const char* data)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(data[i]);
    return value;
}

IceState translate(juice_state_t state)
{
    switch (state)
    {
    case JUICE_STATE_GATHERING: return IceState::Gathering;
    case JUICE_STATE_CONNECTING: return IceState::Connecting;
    case JUICE_STATE_CONNECTED: return IceState::Connected;
    case JUICE_STATE_COMPLETED: return IceState::Completed;
    case JUICE_STATE_FAILED: return IceState::Failed;
    case JUICE_STATE_DISCONNECTED:
    default: return IceState::Disconnected;
    }
}

bool known_channel(uint8_t c)
{
    return c == uint8_t(IceChannel::Ping) || c == uint8_t(IceChannel::Prematch) ||
           c == uint8_t(IceChannel::Gekko) || c == uint8_t(IceChannel::LagStats) ||
           (c >= uint8_t(IceChannel::GekkoEpochFirst) && c <= uint8_t(IceChannel::GekkoEpochLast));
}
} // namespace

struct IceCallbacks
{
    static void stateChanged(juice_agent_t*, juice_state_t state, void* user)
    {
        auto* peer = static_cast<IceMesh::Peer*>(user);
        if (peer && peer->active.load(std::memory_order_relaxed))
            peer->state.store(translate(state), std::memory_order_relaxed);
    }

    static void candidate(juice_agent_t*, const char* sdp, void* user)
    {
        auto* peer = static_cast<IceMesh::Peer*>(user);
        if (!peer || !sdp || !peer->active.load(std::memory_order_relaxed))
            return;
        std::lock_guard<std::mutex> lock(peer->mesh->m_mutex);
        peer->mesh->m_localSignals.push_back({peer->userId, "candidate", sdp});
    }

    static void gatheringDone(juice_agent_t*, void* user)
    {
        auto* peer = static_cast<IceMesh::Peer*>(user);
        if (!peer || !peer->active.load(std::memory_order_relaxed))
            return;
        std::lock_guard<std::mutex> lock(peer->mesh->m_mutex);
        peer->mesh->m_localSignals.push_back({peer->userId, "gathering_done", {}});
    }

    static void recv(juice_agent_t* agent, const char* data, size_t size, void* user)
    {
        auto* peer = static_cast<IceMesh::Peer*>(user);
        if (!peer || !data || size < 1 || !peer->active.load(std::memory_order_relaxed))
            return;
        const uint8_t channel = static_cast<uint8_t>(data[0]);
        if (!known_channel(channel))
            return;

        // Ping request: echo straight from the receive callback (LobbyIce.cpp:193-205), so the
        // poll interval of whoever drains the queue is not counted as network latency.
        if (channel == uint8_t(IceChannel::Ping) && size == 10 && data[1] == 1)
        {
            std::vector<char> reply(data, data + size);
            reply[1] = 2;
            if (juice_send(agent, reply.data(), reply.size()) == JUICE_ERR_SUCCESS)
                return;
        }

        IceMesh::Queued q{peer->userId, channel, 0, {}};
        if (channel == uint8_t(IceChannel::Ping) && size == 10 && data[1] == 2)
        {
            const uint64_t nonce = read_be_u64(data + 2);
            const uint64_t at = now_us();
            std::lock_guard<std::mutex> pingLock(peer->pingMutex);
            const auto it = peer->pingSentAtUs.find(nonce);
            if (it != peer->pingSentAtUs.end())
            {
                if (at >= it->second)
                    q.rttUs = at - it->second;
                peer->pingSentAtUs.erase(it);
            }
        }
        q.data.assign(data + 1, data + size);
        std::lock_guard<std::mutex> lock(peer->mesh->m_mutex);
        if (peer->mesh->m_packets.size() >= kMaxQueuedPackets)
            peer->mesh->m_packets.pop_front();
        peer->mesh->m_packets.push_back(std::move(q));
    }
};

namespace
{
std::atomic<void (*)(const char*)> g_juiceLog{nullptr};

void juiceLog(juice_log_level_t level, const char* message)
{
    static const char* const names[] = { "verbose", "debug", "info", "warn", "error", "fatal" };
    void (*fn)(const char*) = g_juiceLog.load();
    if (!fn || !message)
        return;
    char line[600];
    std::snprintf(line, sizeof(line), "juice %s: %s", level >= 0 && level <= 5 ? names[level] : "?", message);
    fn(line);
}
} // namespace

void ice_log_to(void (*fn)(const char*), bool verbose)
{
    g_juiceLog.store(fn);
    if (fn) {
        juice_set_log_handler(juiceLog);
        juice_set_log_level(verbose ? JUICE_LOG_LEVEL_INFO : JUICE_LOG_LEVEL_WARN);
    } else {
        juice_set_log_handler(nullptr);
        juice_set_log_level(JUICE_LOG_LEVEL_WARN);
    }
}

IceMesh::IceMesh() = default;

IceMesh::~IceMesh()
{
    reset();
}

bool IceMesh::configure(uint64_t localUserId, const IceConfig& config)
{
    reset();
    if (localUserId == 0)
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_localUserId = localUserId;
    m_config = config;
    m_configured = true;
    return true;
}

void IceMesh::reset()
{
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& kv : m_peers)
        {
            kv.second->active.store(false, std::memory_order_relaxed);
            peers.push_back(kv.second);
        }
        m_peers.clear();
        m_nextUpperRange.clear();
        m_pendingRemote.clear();
        m_localSignals.clear();
        m_packets.clear();
        m_localUserId = 0;
        m_configured = false;
    }
    for (const auto& peer : peers)
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        if (peer->agent)
        {
            juice_destroy(peer->agent);
            peer->agent = nullptr;
        }
    }
}

bool IceMesh::addPeer(uint64_t peerUserId)
{
    IceConfig config;
    bool upper = false;
    std::vector<PendingSignal> pending;
    auto peer = std::make_shared<Peer>();
    peer->mesh = this;
    peer->userId = peerUserId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (peerUserId == 0 || peerUserId == m_localUserId || !m_configured)
            return false;
        if (m_peers.count(peerUserId))
            return true;
        upper = m_nextUpperRange[peerUserId];
        m_nextUpperRange[peerUserId] = !upper;
        config = m_config;
        auto it = m_pendingRemote.find(peerUserId);
        if (it != m_pendingRemote.end())
        {
            pending = std::move(it->second);
            m_pendingRemote.erase(it);
        }
        m_peers.emplace(peerUserId, peer);
    }

    std::vector<juice_turn_server_t> turn;
    for (const IceTurnServer& t : config.turn)
        turn.push_back({t.host.c_str(), t.username.c_str(), t.password.c_str(), t.port});

    juice_config_t jc{};
    jc.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    jc.stun_server_host = config.stunHost.empty() ? nullptr : config.stunHost.c_str();
    jc.stun_server_port = config.stunHost.empty() ? 0 : config.stunPort;
    jc.bind_address = config.bindAddress.empty() ? nullptr : config.bindAddress.c_str();
    jc.local_port_range_begin = upper ? config.portRangeMiddle : config.portRangeBegin;
    jc.local_port_range_end = upper ? config.portRangeEnd : static_cast<uint16_t>(config.portRangeMiddle - 1);
    jc.turn_servers = turn.empty() ? nullptr : turn.data();
    jc.turn_servers_count = static_cast<int>(turn.size());
    jc.cb_state_changed = IceCallbacks::stateChanged;
    jc.cb_candidate = IceCallbacks::candidate;
    jc.cb_gathering_done = IceCallbacks::gatheringDone;
    jc.cb_recv = IceCallbacks::recv;
    jc.user_ptr = peer.get();
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        peer->agent = juice_create(&jc);
    }
    char description[JUICE_MAX_SDP_STRING_LEN] = {};
    bool haveDescription = false;
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        haveDescription = peer->agent &&
            juice_get_local_description(peer->agent, description, sizeof(description)) == JUICE_ERR_SUCCESS;
    }
    if (!haveDescription || description[0] == '\0')
    {
        removePeer(peerUserId);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_localSignals.push_back({peerUserId, "description", description});
    }
    for (const PendingSignal& s : pending)
        applyRemoteSignal(peer, s.kind, s.value);
    int gather = JUICE_ERR_FAILED;
    {
        std::lock_guard<std::mutex> agentLock(peer->agentMutex);
        if (peer->agent)
            gather = juice_gather_candidates(peer->agent);
    }
    if (gather != JUICE_ERR_SUCCESS)
    {
        removePeer(peerUserId);
        return false;
    }
    return true;
}

void IceMesh::removePeer(uint64_t peerUserId)
{
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Also the generation boundary for signals that arrived before the agent existed
        // (LobbyIce.cpp:474-478).
        m_pendingRemote.erase(peerUserId);
        auto it = m_peers.find(peerUserId);
        if (it == m_peers.end())
            return;
        peer = it->second;
        peer->active.store(false, std::memory_order_relaxed);
        m_peers.erase(it);
        m_localSignals.erase(std::remove_if(m_localSignals.begin(), m_localSignals.end(),
            [peerUserId](const IceSignal& s) { return s.peerUserId == peerUserId; }), m_localSignals.end());
        m_packets.erase(std::remove_if(m_packets.begin(), m_packets.end(),
            [peerUserId](const Queued& q) { return q.peer == peerUserId; }), m_packets.end());
    }
    std::lock_guard<std::mutex> agentLock(peer->agentMutex);
    if (peer->agent)
    {
        juice_destroy(peer->agent);
        peer->agent = nullptr;
    }
}

std::shared_ptr<IceMesh::Peer> IceMesh::findPeer(uint64_t peerUserId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_peers.find(peerUserId);
    return it == m_peers.end() ? nullptr : it->second;
}

bool IceMesh::hasPeer(uint64_t peerUserId)
{
    return findPeer(peerUserId) != nullptr;
}

IceState IceMesh::peerState(uint64_t peerUserId)
{
    auto peer = findPeer(peerUserId);
    return peer ? peer->state.load(std::memory_order_relaxed) : IceState::Disconnected;
}

bool IceMesh::peerConnected(uint64_t peerUserId)
{
    const IceState s = peerState(peerUserId);
    return s == IceState::Connected || s == IceState::Completed;
}

std::string IceMesh::peerSelectedAddresses(uint64_t peerUserId)
{
    auto peer = findPeer(peerUserId);
    if (!peer || !peerConnected(peerUserId))
        return {};
    std::lock_guard<std::mutex> agentLock(peer->agentMutex);
    if (!peer->agent || !peer->active.load(std::memory_order_relaxed))
        return {};
    char local[JUICE_MAX_ADDRESS_STRING_LEN] = {};
    char remote[JUICE_MAX_ADDRESS_STRING_LEN] = {};
    if (juice_get_selected_addresses(peer->agent, local, sizeof(local), remote, sizeof(remote)) != JUICE_ERR_SUCCESS)
        return {};
    return std::string(local) + " -> " + remote;
}

std::vector<IceSignal> IceMesh::takeLocalSignals()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<IceSignal> out(m_localSignals.begin(), m_localSignals.end());
    m_localSignals.clear();
    return out;
}

bool IceMesh::applyRemoteSignal(const std::shared_ptr<Peer>& peer, const std::string& kind, const std::string& value)
{
    if (!peer)
        return false;
    std::lock_guard<std::mutex> agentLock(peer->agentMutex);
    if (!peer->agent || !peer->active.load(std::memory_order_relaxed))
        return false;
    if (kind == "description")
    {
        if (value.empty() || juice_set_remote_description(peer->agent, value.c_str()) != JUICE_ERR_SUCCESS)
            return false;
        peer->hasRemoteDescription = true;
        for (const std::string& c : peer->pendingCandidates)
            juice_add_remote_candidate(peer->agent, c.c_str());
        peer->pendingCandidates.clear();
        if (peer->remoteGatheringDone)
            juice_set_remote_gathering_done(peer->agent);
        return true;
    }
    if (kind == "candidate")
    {
        if (value.empty())
            return false;
        if (!peer->hasRemoteDescription)
        {
            peer->pendingCandidates.push_back(value);
            return true;
        }
        return juice_add_remote_candidate(peer->agent, value.c_str()) == JUICE_ERR_SUCCESS;
    }
    if (kind == "gathering_done")
    {
        peer->remoteGatheringDone = true;
        return !peer->hasRemoteDescription || juice_set_remote_gathering_done(peer->agent) == JUICE_ERR_SUCCESS;
    }
    return false;
}

bool IceMesh::handleRemoteSignal(uint64_t peerUserId, const std::string& kind, const std::string& value)
{
    auto peer = findPeer(peerUserId);
    if (!peer)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingRemote[peerUserId].push_back({kind, value});
        return true;
    }
    return applyRemoteSignal(peer, kind, value);
}

bool IceMesh::send(uint64_t peerUserId, uint8_t channel, const void* data, size_t size)
{
    auto peer = findPeer(peerUserId);
    if (!peer || !peerConnected(peerUserId) || (data == nullptr && size != 0) || size > kMaxPayload)
        return false;
    std::vector<char> framed(size + 1);
    framed[0] = static_cast<char>(channel);
    if (size)
        std::copy_n(static_cast<const char*>(data), size, framed.data() + 1);
    std::lock_guard<std::mutex> agentLock(peer->agentMutex);
    if (!peer->agent || !peer->active.load(std::memory_order_relaxed))
        return false;

    uint64_t nonce = 0, sentAt = 0;
    if (channel == uint8_t(IceChannel::Ping) && size == 9 && framed[1] == 1)
    {
        nonce = read_be_u64(framed.data() + 2);
        sentAt = now_us();
        std::lock_guard<std::mutex> pingLock(peer->pingMutex);
        const uint64_t cutoff = sentAt > 60000000ull ? sentAt - 60000000ull : 0;
        for (auto it = peer->pingSentAtUs.begin(); it != peer->pingSentAtUs.end();)
            it = it->second < cutoff ? peer->pingSentAtUs.erase(it) : std::next(it);
        peer->pingSentAtUs[nonce] = sentAt;
    }
    const bool sent = juice_send(peer->agent, framed.data(), framed.size()) == JUICE_ERR_SUCCESS;
    if (!sent && sentAt)
    {
        std::lock_guard<std::mutex> pingLock(peer->pingMutex);
        auto it = peer->pingSentAtUs.find(nonce);
        if (it != peer->pingSentAtUs.end() && it->second == sentAt)
            peer->pingSentAtUs.erase(it);
    }
    return sent;
}

std::vector<IcePacket> IceMesh::takePackets(uint8_t channel)
{
    std::vector<IcePacket> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_packets.begin(); it != m_packets.end();)
    {
        if (it->channel == channel)
        {
            out.push_back({it->peer, it->rttUs, std::move(it->data)});
            it = m_packets.erase(it);
        }
        else
            ++it;
    }
    return out;
}

} // namespace znxlobby
