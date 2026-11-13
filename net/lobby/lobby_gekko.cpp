// SPDX-License-Identifier: GPL-3.0-or-later
/* GekkoNet's network adapter over the lobby's ICE mesh: RMG-K's ice adapter
 * (Source/RMG-Core/rmgk_gekko.cpp:1555-1634) for any znx_lobby. Remote actors are added with the
 * address text "ice:<userId>" (rmgk_gekko.cpp:2445-2450); each datagram is the GekkoNet packet
 * behind one channel byte (3 for a match's first session), nothing else, so it interoperates with
 * RMG-K's own adapter byte for byte.
 *
 * GekkoNetAdapter's functions take no user pointer, so each lobby that hands out an adapter gets
 * one of a few fixed slots. The functions run on whatever thread drives the GekkoNet session. */
#include "znx_lobby.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gekkonet.h"

namespace
{
constexpr int kSlots = 4;

struct Slot
{
    znx_lobby* lobby = nullptr;
    int channel = 3;
    std::vector<GekkoNetResult*> results;
    GekkoNetAdapter adapter{};
};

Slot g_slots[kSlots];
std::mutex g_slotMutex;

uint64_t userIdOf(const GekkoNetAddress* addr)
{
    if (!addr || !addr->data || addr->size < 5 || addr->size > 32)
        return 0;
    const std::string text(static_cast<const char*>(addr->data), addr->size);
    if (text.compare(0, 4, "ice:") != 0)
        return 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str() + 4, &end, 10);
    return (end && *end == '\0') ? v : 0;
}

template <int N>
void sendData(GekkoNetAddress* addr, const char* data, int length)
{
    Slot& s = g_slots[N];
    const uint64_t uid = userIdOf(addr);
    if (s.lobby && uid && length >= 0)
        znx_lobby_send(s.lobby, uid, s.channel, data, size_t(length));
}

template <int N>
GekkoNetResult** receiveData(int* length)
{
    Slot& s = g_slots[N];
    s.results.clear();
    if (!length)
        return s.results.data();
    if (s.lobby)
    {
        char buf[4096];
        uint64_t from = 0;
        int n;
        while ((n = znx_lobby_receive(s.lobby, s.channel, &from, buf, sizeof(buf))) >= 0)
        {
            if (n > int(sizeof(buf)))
                continue;
            char addr[32];
            const int alen = znx_lobby_peer_address(from, addr, sizeof(addr));
            if (alen <= 0)
                continue;
            auto* r = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
            if (!r)
                break;
            r->addr.data = std::malloc(size_t(alen));
            r->data = std::malloc(n > 0 ? size_t(n) : 1);
            if (!r->addr.data || !r->data)
            {
                std::free(r->addr.data);
                std::free(r->data);
                std::free(r);
                break;
            }
            std::memcpy(r->addr.data, addr, size_t(alen));
            r->addr.size = unsigned(alen);
            if (n > 0)
                std::memcpy(r->data, buf, size_t(n));
            r->data_len = unsigned(n);
            s.results.push_back(r);
        }
    }
    *length = int(s.results.size());
    return s.results.data();
}

void freeData(void* p)
{
    std::free(p);
}

template <int N>
void initSlot()
{
    g_slots[N].adapter.send_data = sendData<N>;
    g_slots[N].adapter.receive_data = receiveData<N>;
    g_slots[N].adapter.free_data = freeData;
}
} // namespace

extern "C" void* znx_lobby_gekko_adapter(znx_lobby* lobby, int channel)
{
    if (!lobby || channel < 0 || channel > 255)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_slotMutex);
    static bool inited = false;
    if (!inited)
    {
        initSlot<0>();
        initSlot<1>();
        initSlot<2>();
        initSlot<3>();
        inited = true;
    }
    int free = -1;
    for (int i = 0; i < kSlots; ++i)
    {
        if (g_slots[i].lobby == lobby)
        {
            g_slots[i].channel = channel;
            return &g_slots[i].adapter;
        }
        if (!g_slots[i].lobby && free < 0)
            free = i;
    }
    if (free < 0)
        return nullptr;
    g_slots[free].lobby = lobby;
    g_slots[free].channel = channel;
    return &g_slots[free].adapter;
}

extern "C" void znx_lobby_gekko_adapter_release(znx_lobby* lobby)
{
    std::lock_guard<std::mutex> lock(g_slotMutex);
    for (Slot& s : g_slots)
        if (s.lobby == lobby)
            s.lobby = nullptr;
}
