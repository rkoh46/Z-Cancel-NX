// SPDX-License-Identifier: GPL-3.0-or-later
/* The room's ICE mesh: one libjuice agent per other room member, as RMG-K's LobbyIce
 * (Source/RMG-Core/LobbyIce.cpp) builds it, with the same datagram framing on the wire: every
 * datagram starts with a channel byte (1 ping, 2 pre-match, 3 GekkoNet, 4 lag stats, 0x10-0x1f
 * GekkoNet after a late join; LobbyIce.hpp:16-30) and carries at most 4000 bytes after it
 * (LobbyIce.cpp:571).
 *
 * Differences from LobbyIce, none of which shows on the wire: one mesh per object instead of one
 * per process (so a test can run two clients in one process), STUN can be switched off (empty
 * host), and the agents can be bound to one local address (the loopback test binds 127.0.0.1,
 * which makes libjuice offer exactly that host candidate, udp.c:443-447).
 *
 * Thread safety: libjuice calls back on its own thread (JUICE_CONCURRENCY_MODE_POLL, one thread
 * for all agents); every method here may be called from any thread. */
#ifndef ZNX_LOBBY_ICE_MESH_H
#define ZNX_LOBBY_ICE_MESH_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <string>
#include <vector>

namespace znxlobby
{

enum class IceChannel : uint8_t
{
    Ping = 1,
    Prematch = 2,
    Gekko = 3,
    LagStats = 4,
    GekkoEpochFirst = 0x10,
    GekkoEpochLast = 0x1f,
};

enum class IceState { Disconnected = 0, Gathering, Connecting, Connected, Completed, Failed };

struct IceTurnServer
{
    std::string host;
    uint16_t port = 3478;
    std::string username;
    std::string password;
};

struct IceConfig
{
    std::string stunHost;          // empty: no STUN (host candidates, and TURN if given)
    uint16_t stunPort = 19302;
    std::vector<IceTurnServer> turn;
    std::string bindAddress;       // empty: every interface (RMG-K's behaviour)
    // RMG-K alternates each peer's agents between 49152-57343 and 57344-65535 so an ICE restart
    // never reuses the previous local port (LobbyIce.cpp:57-71, 419-422).
    uint16_t portRangeBegin = 49152;
    uint16_t portRangeMiddle = 57344;
    uint16_t portRangeEnd = 65535;
};

struct IceSignal
{
    uint64_t peerUserId = 0;
    std::string kind;   // "description", "candidate" or "gathering_done"
    std::string value;
};

struct IcePacket
{
    uint64_t peerUserId = 0;
    uint64_t roundTripUs = 0;   // set on a ping reply this mesh timed (LobbyIce.cpp:210-222)
    std::vector<char> data;     // without the channel byte
};

class IceMesh
{
  public:
    IceMesh();
    ~IceMesh();
    IceMesh(const IceMesh&) = delete;
    IceMesh& operator=(const IceMesh&) = delete;

    bool configure(uint64_t localUserId, const IceConfig& config);
    void reset();

    bool addPeer(uint64_t peerUserId);
    void removePeer(uint64_t peerUserId);
    bool hasPeer(uint64_t peerUserId);
    IceState peerState(uint64_t peerUserId);
    bool peerConnected(uint64_t peerUserId);
    std::string peerSelectedAddresses(uint64_t peerUserId);

    std::vector<IceSignal> takeLocalSignals();
    bool handleRemoteSignal(uint64_t peerUserId, const std::string& kind, const std::string& value);

    bool send(uint64_t peerUserId, uint8_t channel, const void* data, size_t size);
    std::vector<IcePacket> takePackets(uint8_t channel);

    struct Peer;   // libjuice's callbacks need the definition (ice_mesh.cpp)

  private:
    friend struct IceCallbacks;
    std::shared_ptr<Peer> findPeer(uint64_t peerUserId);
    bool applyRemoteSignal(const std::shared_ptr<Peer>& peer, const std::string& kind, const std::string& value);

    std::mutex m_mutex;
    uint64_t m_localUserId = 0;
    bool m_configured = false;
    IceConfig m_config;
    std::map<uint64_t, std::shared_ptr<Peer>> m_peers;
    std::map<uint64_t, bool> m_nextUpperRange;
    struct PendingSignal { std::string kind, value; };
    std::map<uint64_t, std::vector<PendingSignal>> m_pendingRemote;
    std::deque<IceSignal> m_localSignals;
    struct Queued { uint64_t peer; uint8_t channel; uint64_t rttUs; std::vector<char> data; };
    std::deque<Queued> m_packets;
};

} // namespace znxlobby

#endif
