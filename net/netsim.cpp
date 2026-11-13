// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-net: a bad network between two programs on one machine.
 *
 * RMG-K's n02 socket (n02/common/k_socket.cpp) is compiled for znx-net with -Dsendto=... and
 * -Dbind=..., so every datagram the n02 client sends (its own instructions and the GekkoNet
 * packets it tunnels) comes through znx_netsim_sendto, and its socket is bound through
 * znx_netsim_bind. Nothing in RMG-K's sources changes.
 *
 * - latency/jitter: each datagram leaves after latency ms, GekkoNet's after latency +- jitter ms
 *   (uniform), from a sender thread: the two directions add up to the round trip, and GekkoNet's
 *   datagrams can overtake each other (the n02 instructions keep their order, as n02 expects).
 * - loss: only GekkoNet's datagrams (the n02 tunnel's 0xff "RMGKRB1" prefix) are dropped. The
 *   n02 lobby handshake that comes before the game has no retransmission (p2p_core.cpp sends
 *   LOGN once), so dropping it would only test the lobby, which is not what this step is about.
 * - loopback_only: the socket is bound to 127.0.0.1, not every interface, so this step never
 *   listens on the network (and macOS's firewall does not ask).
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "znx_net.h"

namespace
{
struct Pending {
    std::chrono::steady_clock::time_point due;
    int sock;
    std::vector<char> data;
    sockaddr_storage addr;
    socklen_t addr_len;
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::vector<Pending> g_queue;
std::thread g_thread;
bool g_running = false;
znx_netsim_config g_config = {};
std::mt19937 g_rng;
uint64_t g_sent, g_dropped, g_delayed;

bool is_gekko_datagram(const void* data, size_t len)
{
    static const char prefix[] = { (char)0xff, 'R', 'M', 'G', 'K', 'R', 'B', '1' };
    return len >= sizeof(prefix) && std::memcmp(data, prefix, sizeof(prefix)) == 0;
}

void sender_thread()
{
    std::unique_lock<std::mutex> lock(g_mutex);
    while (g_running || !g_queue.empty()) {
        if (g_queue.empty()) {
            g_wake.wait(lock);
            continue;
        }
        auto next = g_queue.begin();
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
            if (it->due < next->due)
                next = it;
        const auto now = std::chrono::steady_clock::now();
        if (next->due > now && g_running) {
            g_wake.wait_until(lock, next->due);
            continue;
        }
        Pending p = std::move(*next);
        g_queue.erase(next);
        lock.unlock();
        ::sendto(p.sock, p.data.data(), p.data.size(), 0, (const sockaddr*)&p.addr, p.addr_len);
        lock.lock();
    }
}
} // namespace

extern "C" void znx_netsim_configure(const znx_netsim_config* config)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_config = *config;
    g_rng.seed(config->seed);
    if ((g_config.latency_ms > 0 || g_config.jitter_ms > 0) && !g_running) {
        g_running = true;
        g_thread = std::thread(sender_thread);
    }
}

extern "C" void znx_netsim_shutdown(void)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running)
            return;
        g_running = false;
        g_queue.clear();
    }
    g_wake.notify_all();
    g_thread.join();
}

extern "C" void znx_netsim_stats(uint64_t* sent, uint64_t* dropped, uint64_t* delayed)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    *sent = g_sent;
    *dropped = g_dropped;
    *delayed = g_delayed;
}

extern "C" ssize_t znx_netsim_sendto(int sock, const void* data, size_t len, int flags,
    const struct sockaddr* addr, socklen_t addr_len)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_sent++;
    if (g_config.loss_pct > 0 && is_gekko_datagram(data, len) &&
        std::uniform_real_distribution<double>(0.0, 100.0)(g_rng) < g_config.loss_pct) {
        g_dropped++;
        return (ssize_t)len;   /* the sender cannot tell a lost datagram from a sent one */
    }
    if (!g_running || addr == nullptr || addr_len > sizeof(sockaddr_storage)) {
        lock.unlock();
        return ::sendto(sock, data, len, flags, addr, addr_len);
    }
    int delay = g_config.latency_ms;
    if (g_config.jitter_ms > 0 && is_gekko_datagram(data, len))
        delay += std::uniform_int_distribution<int>(-g_config.jitter_ms, g_config.jitter_ms)(g_rng);
    if (delay < 0)
        delay = 0;
    Pending p;
    p.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    p.sock = sock;
    p.data.assign((const char*)data, (const char*)data + len);
    std::memcpy(&p.addr, addr, addr_len);
    p.addr_len = addr_len;
    g_queue.push_back(std::move(p));
    g_delayed++;
    lock.unlock();
    g_wake.notify_all();
    return (ssize_t)len;
}

extern "C" int znx_netsim_bind(int sock, const struct sockaddr* addr, socklen_t addr_len)
{
    bool loopback;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        loopback = g_config.loopback_only != 0;
    }
    if (loopback && addr != nullptr && addr->sa_family == AF_INET && addr_len >= sizeof(sockaddr_in)) {
        sockaddr_in local;
        std::memcpy(&local, addr, sizeof(local));
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::bind(sock, (const sockaddr*)&local, sizeof(local));
    }
    return ::bind(sock, addr, addr_len);
}
