// SPDX-License-Identifier: GPL-3.0-or-later
/* Hook for running RMG-K's own session driver over this library: RMG-Core/rmgk_gekko.cpp compiled
 * with -DNETPLAY (its ICE path, start_lobby_session and the "ice:" adapter, rmgk_gekko.cpp:1555-1634
 * and 3737-3990) calls exactly three LobbyIce functions: send, take_packets and peer_connected
 * (checked with nm on such an object). They are defined here, forwarding to the znx_lobby bound
 * with znx_lobby_bind_rmgk(). The rest of RMG-K's LobbyIce (the mesh itself) is ice_mesh.cpp.
 * Not linked into anything yet: the Switch program links it next to rmgk_gekko.cpp. */
#include "LobbyIce.hpp"   // RMG-K's header (build/src/RMG-Core), so the declarations are RMG-K's

#include <atomic>

#include "znx_lobby.h"

namespace
{
std::atomic<znx_lobby*> g_bound{nullptr};
}

extern "C" void znx_lobby_bind_rmgk(znx_lobby* lobby)
{
    g_bound.store(lobby);
}

bool LobbyIce::send(std::uint64_t peerUserId, LobbyIceChannel channel, const char* data, std::size_t size)
{
    znx_lobby* lb = g_bound.load();
    return lb && znx_lobby_send(lb, peerUserId, int(channel), data, size) == 0;
}

std::vector<LobbyIcePacket> LobbyIce::take_packets(LobbyIceChannel channel)
{
    std::vector<LobbyIcePacket> out;
    znx_lobby* lb = g_bound.load();
    if (!lb)
        return out;
    char buf[4096];
    uint64_t from = 0;
    int n;
    while ((n = znx_lobby_receive(lb, int(channel), &from, buf, sizeof(buf))) >= 0)
    {
        if (n > int(sizeof(buf)))
            continue;
        LobbyIcePacket p;
        p.peerUserId = from;
        p.data.assign(buf, buf + n);
        out.push_back(std::move(p));
    }
    return out;
}

bool LobbyIce::peer_connected(std::uint64_t peerUserId)
{
    znx_lobby* lb = g_bound.load();
    return lb && znx_lobby_peer_connected(lb, peerUserId);
}
