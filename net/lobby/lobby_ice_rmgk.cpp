// SPDX-License-Identifier: GPL-3.0-or-later
/* Hook for running RMG-K's own session driver over this library: RMG-Core/rmgk_gekko.cpp compiled
 * with -DNETPLAY (its ICE path, start_lobby_session and the "ice:" adapter, rmgk_gekko.cpp:1555-1634
 * and 3737-3990) calls exactly three LobbyIce functions: send, take_packets and peer_connected
 * (checked with nm on such an object). They are defined here, forwarding to the znx_lobby bound
 * with znx_lobby_bind_rmgk(). The rest of RMG-K's LobbyIce (the mesh itself) is ice_mesh.cpp.
 * Linked next to rmgk_gekko.cpp in znx-net, znx-game and the Switch program.
 *
 * Tests only: znx_lobby_rmgk_netsim adds latency, jitter and loss to what the session sends (net/znx-net.cpp
 * --sim-*), the lobby counterpart of net/netsim.cpp on the n02 socket. Off unless a test turns it on. */
#include "LobbyIce.hpp"   // RMG-K's header (build/src/RMG-Core), so the declarations are RMG-K's

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

#include "znx_lobby.h"

namespace
{
std::atomic<znx_lobby*> g_bound{nullptr};

struct Delayed
{
    std::chrono::steady_clock::time_point due;
    uint64_t seq;
    uint64_t peer;
    int channel;
    std::vector<char> data;
    bool operator<(const Delayed& o) const { return due != o.due ? due > o.due : seq > o.seq; }
};

struct Netsim
{
    std::mutex mutex;
    std::condition_variable cv;
    std::priority_queue<Delayed> queue;
    std::thread thread;
    bool on = false, stop = false;
    int latencyMs = 0, jitterMs = 0;
    double lossPct = 0;
    std::mt19937 rng;
    uint64_t seq = 0, sent = 0, dropped = 0;
} g_sim;

void netsim_main()
{
    std::unique_lock<std::mutex> lock(g_sim.mutex);
    while (!g_sim.stop) {
        if (g_sim.queue.empty()) {
            g_sim.cv.wait(lock);
            continue;
        }
        const auto due = g_sim.queue.top().due;
        if (std::chrono::steady_clock::now() < due) {
            g_sim.cv.wait_until(lock, due);
            continue;
        }
        Delayed d = g_sim.queue.top();
        g_sim.queue.pop();
        lock.unlock();
        if (znx_lobby* lb = g_bound.load())
            znx_lobby_send(lb, d.peer, d.channel, d.data.data(), d.data.size());
        lock.lock();
    }
}
} // namespace

extern "C" void znx_lobby_bind_rmgk(znx_lobby* lobby)
{
    g_bound.store(lobby);
}

extern "C" void znx_lobby_rmgk_netsim(int latency_ms, int jitter_ms, double loss_pct, unsigned seed)
{
    std::lock_guard<std::mutex> lock(g_sim.mutex);
    g_sim.latencyMs = latency_ms < 0 ? 0 : latency_ms;
    g_sim.jitterMs = jitter_ms < 0 ? 0 : jitter_ms;
    g_sim.lossPct = loss_pct < 0 ? 0 : loss_pct;
    g_sim.rng.seed(seed ? seed : 1u);
    g_sim.on = g_sim.latencyMs > 0 || g_sim.jitterMs > 0 || g_sim.lossPct > 0;
    if (g_sim.on && !g_sim.thread.joinable()) {
        g_sim.stop = false;
        g_sim.thread = std::thread(netsim_main);
    }
}

extern "C" void znx_lobby_rmgk_netsim_stats(uint64_t* sent, uint64_t* dropped)
{
    std::lock_guard<std::mutex> lock(g_sim.mutex);
    *sent = g_sim.sent;
    *dropped = g_sim.dropped;
}

extern "C" void znx_lobby_rmgk_netsim_shutdown(void)
{
    {
        std::lock_guard<std::mutex> lock(g_sim.mutex);
        g_sim.stop = true;
        g_sim.on = false;
    }
    g_sim.cv.notify_all();
    if (g_sim.thread.joinable())
        g_sim.thread.join();
}

bool LobbyIce::send(std::uint64_t peerUserId, LobbyIceChannel channel, const char* data, std::size_t size)
{
    znx_lobby* lb = g_bound.load();
    if (!lb)
        return false;
    {
        std::unique_lock<std::mutex> lock(g_sim.mutex);
        if (g_sim.on) {
            g_sim.sent++;
            if (std::uniform_real_distribution<double>(0.0, 100.0)(g_sim.rng) < g_sim.lossPct) {
                g_sim.dropped++;
                return true;   /* lost on the way: the sender cannot tell */
            }
            int ms = g_sim.latencyMs;
            if (g_sim.jitterMs > 0)
                ms += std::uniform_int_distribution<int>(-g_sim.jitterMs, g_sim.jitterMs)(g_sim.rng);
            Delayed d;
            d.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms < 0 ? 0 : ms);
            d.seq = g_sim.seq++;
            d.peer = peerUserId;
            d.channel = int(channel);
            d.data.assign(data, data + size);
            g_sim.queue.push(std::move(d));
            lock.unlock();
            g_sim.cv.notify_one();
            return true;
        }
    }
    return znx_lobby_send(lb, peerUserId, int(channel), data, size) == 0;
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
