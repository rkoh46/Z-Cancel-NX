// SPDX-License-Identifier: GPL-3.0-or-later
/* See lobby_play.h. File:line references are to ref/RMG-K/Source (v0.9.16, 2f5454a): RLD is
 * RMG/UserInterface/Dialog/Lobby/RollbackLobbyDialog.cpp, LC its LobbyClient.cpp. */

#include "lobby_play.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

#include "Cheats.hpp"
#include "NetplaySave.hpp"
#include "m64p/Api.hpp"

/* NetplaySave.hpp declares it for RMG-Core's own files only (CORE_INTERNAL). */
bool CoreApplyNetplaySaveData(bool enabled);

namespace
{
using Clock = std::chrono::steady_clock;

std::recursive_mutex g_lock;
znx_lobby* g_lb = nullptr;
/* The lobby is polled every 5 ms by its own thread while it is open, so the WebSocket (heartbeats), ICE
 * signalling and pings keep moving while the menu shows a message or the system keyboard is up, and
 * during the match (the menu's thread runs the game then). */
std::thread g_thread;
std::atomic<bool> g_thread_run{false};
int g_phase = ZNX_LP_CLOSED;
Clock::time_point g_phase_since;
std::string g_server, g_username, g_md5, g_rom_name, g_bind, g_local_ip, g_connection, g_stun;
void (*g_log)(const char*) = nullptr;

int g_problem = ZNX_LPP_NONE;
std::string g_problem_title, g_problem_text;

std::deque<znx_lobby_play_chat_line> g_chat;
std::map<uint64_t, std::string> g_seated;          /* who sat in the room at the last ROOM_STATE */
std::map<uint64_t, int> g_ice_reported;            /* last ICE value announced in the chat */
bool g_leaving = false;

/* STARTING */
Clock::time_point g_start_since;
bool g_prematch = false;

/* the match */
znx_lobby_match_info g_match{};
std::vector<uint8_t> g_manifest;
bool g_match_connected_sent = false, g_match_finished_sent = false, g_room_back_reported = false;
std::string g_end_title, g_end_text;
bool g_lost_in_game = false;

constexpr size_t kChatLines = 64;

void logf(const char* format, ...)
{
    if (g_log == nullptr)
        return;
    char line[700];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_log(line);
}

void lobby_log(void*, const char* line)
{
    if (g_log != nullptr)
        g_log(line);
}

void copy(char* dst, size_t cap, const std::string& s)
{
    std::snprintf(dst, cap, "%s", s.c_str());
}

void set_phase(int phase)
{
    if (g_phase != phase) {
        static const char* const names[] = { "closed", "connecting", "rooms", "entering", "room", "starting",
                                             "ready", "in game", "failed" };
        logf("lobby: %s -> %s", names[g_phase], names[phase]);
    }
    g_phase = phase;
    g_phase_since = Clock::now();
}

void problem(int kind, const std::string& title, const std::string& text)
{
    logf("lobby: %s: %s", title.c_str(), text.c_str());
    g_problem = kind;
    g_problem_title = title;
    g_problem_text = text;
}

void chat_line(const std::string& name, const std::string& text, bool self)
{
    znx_lobby_play_chat_line line;
    std::memset(&line, 0, sizeof(line));
    copy(line.name, sizeof(line.name), name);
    copy(line.text, sizeof(line.text), text);
    line.self = self ? 1 : 0;
    g_chat.push_back(line);
    while (g_chat.size() > kChatLines)
        g_chat.pop_front();
}

void system_line(const std::string& text)
{
    chat_line("", text, false);
}

bool same_md5(const char* a, const std::string& b)
{
    if (std::strlen(a) != b.size())
        return false;
    for (size_t i = 0; i < b.size(); ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i]))
            return false;
    return true;
}

/* LobbyRegions.hpp: RMG-K's round-trip estimate between two regions, the room list's ping column until
 * a player is measured. */
int region_estimate(const char* a, const char* b)
{
    static const char* const codes[9] = { "us-east", "us-central", "us-west", "sa", "eu-west", "eu-east",
                                          "asia-east", "asia-se", "oceania" };
    static const int rtt[9][9] = {
        { 30, 40, 80, 140, 90, 120, 180, 220, 200 },
        { 40, 30, 50, 150, 110, 140, 170, 210, 190 },
        { 80, 50, 30, 170, 140, 170, 140, 180, 160 },
        { 140, 150, 170, 30, 180, 210, 240, 270, 220 },
        { 90, 110, 140, 180, 30, 50, 220, 200, 280 },
        { 120, 140, 170, 210, 50, 30, 200, 180, 300 },
        { 180, 170, 140, 240, 220, 200, 30, 60, 110 },
        { 220, 210, 180, 270, 200, 180, 60, 30, 100 },
        { 200, 190, 160, 220, 280, 300, 110, 100, 30 },
    };
    int ia = -1, ib = -1;
    for (int i = 0; i < 9; ++i) {
        if (!std::strcmp(a, codes[i])) ia = i;
        if (!std::strcmp(b, codes[i])) ib = i;
    }
    return ia < 0 || ib < 0 ? 0 : rtt[ia][ib];
}

std::string user_name(uint64_t id)
{
    const int n = znx_lobby_user_count(g_lb);
    for (int i = 0; i < n; ++i) {
        znx_lobby_user_info u;
        if (znx_lobby_user(g_lb, i, &u) == 0 && u.id == id)
            return u.username;
    }
    znx_lobby_room_detail room;
    if (znx_lobby_current_room(g_lb, &room) == 0)
        for (const znx_lobby_seat& s : room.seats)
            if (s.user_id == id)
                return s.username;
    return "player " + std::to_string((unsigned long long)id);
}

/* The friendly sentence for a HELLO_FAIL reason (RLD:2738-2747). */
void hello_failed(const std::string& reason)
{
    if (reason == "version_mismatch")
        problem(ZNX_LPP_VERSION, "The lobby wants a newer version",
                "The server says this client is too old. Z-Cancel NX speaks as RMG-K " ZNX_LOBBY_CLIENT_VERSION
                "; the lobby has moved past it. Peer to peer still works.");
    else if (reason == "username_taken")
        problem(ZNX_LPP_REFUSED, "That name is taken",
                "Someone in the lobby already uses \"" + g_username + "\". Change your name in Settings and try again.");
    else if (reason == "server_full")
        problem(ZNX_LPP_REFUSED, "The lobby is full", "The server has no room for another player. Try again later.");
    else if (reason == "invalid_payload" || reason == "invalid_hello")
        problem(ZNX_LPP_REFUSED, "The lobby refused the name",
                "Names are 2 to 20 letters, digits, dots, dashes or underscores. Change your name in Settings.");
    else
        problem(ZNX_LPP_REFUSED, "The lobby refused to log in", reason.empty() ? "No reason was given." : reason);
}

void room_state_changed()
{
    znx_lobby_room_detail room;
    if (znx_lobby_current_room(g_lb, &room) != 0)
        return;
    const uint64_t self = znx_lobby_self_id(g_lb);
    std::map<uint64_t, std::string> now;
    for (const znx_lobby_seat& s : room.seats)
        if (s.user_id != 0)
            now[s.user_id] = s.username;
    if (!g_seated.empty()) {
        for (const auto& kv : now)
            if (!g_seated.count(kv.first) && kv.first != self)
                system_line(kv.second + " joined");
        for (const auto& kv : g_seated)
            if (!now.count(kv.first) && kv.first != self) {
                system_line(kv.second + " left");
                g_ice_reported.erase(kv.first);
            }
    }
    g_seated = now;
    if (g_phase == ZNX_LP_ENTERING)
        set_phase(ZNX_LP_ROOM);
}

void handle(const znx_lobby_event& ev)
{
    switch (ev.type) {
    case ZNX_LOBBY_EV_CONNECTED:
        set_phase(ZNX_LP_ROOMS);
        break;
    case ZNX_LOBBY_EV_HELLO_FAILED:
        hello_failed(ev.text);
        set_phase(ZNX_LP_FAILED);
        break;
    case ZNX_LOBBY_EV_DISCONNECTED:
        if (g_phase == ZNX_LP_IN_GAME || g_phase == ZNX_LP_READY) {
            /* RLD:2628-2640: the match plays on over ICE; the room is gone afterwards */
            g_lost_in_game = true;
            znx_p2p_lobby_server_up(0);
            logf("lobby: connection lost during the match: %s", ev.text);
            break;
        }
        if (g_phase == ZNX_LP_FAILED)
            break;   /* after HELLO_FAIL the socket closes too */
        if (g_phase == ZNX_LP_CONNECTING)
            problem(ZNX_LPP_UNREACHABLE, "The lobby server did not answer",
                    "Nothing came back from " + g_server + " (" + std::string(ev.text) +
                    "). Check the console's internet connection, or the lobby_server line in z-cancel-nx.ini.");
        else
            problem(ZNX_LPP_DISCONNECTED, "The lobby connection dropped",
                    std::string("The server closed the connection (") + ev.text + "). Open the lobby again to reconnect.");
        set_phase(ZNX_LP_FAILED);
        break;
    case ZNX_LOBBY_EV_ROOM_CREATED:
    case ZNX_LOBBY_EV_ROOM_JOINED:
        g_chat.clear();
        g_seated.clear();
        g_ice_reported.clear();
        system_line(ev.type == ZNX_LOBBY_EV_ROOM_CREATED ? "You opened the room" : "You joined the room");
        set_phase(ZNX_LP_ROOM);
        room_state_changed();
        break;
    case ZNX_LOBBY_EV_ROOM_CREATE_FAILED:
        problem(ZNX_LPP_CREATE_FAILED, "The room was not created", ev.text[0] ? ev.text : "The server said no.");
        set_phase(ZNX_LP_ROOMS);
        break;
    case ZNX_LOBBY_EV_ROOM_JOIN_FAILED:
        problem(ZNX_LPP_JOIN_FAILED, "Could not join the room", ev.text[0] ? ev.text : "The server said no.");
        set_phase(ZNX_LP_ROOMS);
        break;
    case ZNX_LOBBY_EV_ROOM_STATE:
        room_state_changed();
        break;
    case ZNX_LOBBY_EV_ROOM_LEFT:
        if (!std::strcmp(ev.text, "kicked"))
            problem(ZNX_LPP_KICKED, "You were removed from the room", "The host removed you. Pick another room or open one.");
        else if (!g_leaving && ev.text[0] && std::strcmp(ev.text, "left") != 0)
            problem(ZNX_LPP_ROOM_CLOSED, "You are no longer in the room", std::string("The server says: ") + ev.text);
        g_leaving = false;
        g_seated.clear();
        if (g_phase != ZNX_LP_IN_GAME && g_phase != ZNX_LP_FAILED)
            set_phase(ZNX_LP_ROOMS);
        break;
    case ZNX_LOBBY_EV_CHAT:
        if (!std::strcmp(ev.channel, "room"))
            chat_line(ev.name, ev.text, ev.user_id == znx_lobby_self_id(g_lb));
        break;
    case ZNX_LOBBY_EV_PEER_ICE: {
        const int before = g_ice_reported.count(ev.user_id) ? g_ice_reported[ev.user_id] : 0;
        if (ev.value == 1 && before != 1)
            system_line("Direct connection to " + user_name(ev.user_id));
        else if (ev.value == -1 && before != -1) {
            system_line("No direct connection to " + user_name(ev.user_id) + " after " + std::to_string(ev.value2) +
                        " attempts");
            if (g_phase == ZNX_LP_ROOM || g_phase == ZNX_LP_STARTING)
                problem(ZNX_LPP_ICE_FAILED, "No connection to " + user_name(ev.user_id),
                        "The two consoles could not reach each other directly, even after RMG-K's 20 attempts. "
                        "Behind strict routers a TURN relay is needed; the lobby offers none for this room.");
        }
        if (ev.value != 0)
            g_ice_reported[ev.user_id] = ev.value;
        break;
    }
    case ZNX_LOBBY_EV_MATCH_BEGIN:
        if (g_phase == ZNX_LP_IN_GAME)
            break;
        g_start_since = Clock::now();
        g_prematch = false;
        system_line("The match is starting");
        set_phase(ZNX_LP_STARTING);
        break;
    case ZNX_LOBBY_EV_MATCH_READY:
        if (g_phase != ZNX_LP_STARTING) {
            /* the player left the room while the match was being set up */
            znx_lobby_match_finished(g_lb);
            break;
        }
        if (znx_lobby_match(g_lb, &g_match) == 0) {
            g_manifest.assign(g_match.manifest, g_match.manifest + g_match.manifest_size);
            g_match.manifest = nullptr;
            g_match_connected_sent = g_match_finished_sent = g_room_back_reported = false;
            g_lost_in_game = false;
            system_line("Pre-match sync complete");
            set_phase(ZNX_LP_READY);
        }
        break;
    case ZNX_LOBBY_EV_MATCH_FAILED:
        if (g_phase == ZNX_LP_IN_GAME)
            break;
        problem(ZNX_LPP_MATCH_FAILED, "The match did not start", ev.text);
        system_line(ev.text);
        if (g_phase == ZNX_LP_STARTING || g_phase == ZNX_LP_READY)
            set_phase(ZNX_LP_ROOM);
        break;
    case ZNX_LOBBY_EV_MATCH_PEER_LEFT:
        if (g_phase == ZNX_LP_IN_GAME && ev.match_id == g_match.match_id)
            znx_p2p_lobby_peer_left(ev.value, user_name(ev.user_id).c_str());
        break;
    case ZNX_LOBBY_EV_NOTICE:
        system_line(std::string("Server: ") + ev.text);
        break;
    default:
        break;
    }
}

void poll_locked()
{
    if (g_lb == nullptr)
        return;
    znx_lobby_event ev;
    while (znx_lobby_poll(g_lb, &ev))
        handle(ev);
    if (g_phase == ZNX_LP_STARTING && !g_prematch) {
        /* after every ICE path is up the library goes on to the pre-match sync (RLD:5767-5793) */
        znx_lobby_room_detail room;
        bool all = true;
        if (znx_lobby_current_room(g_lb, &room) == 0)
            for (const znx_lobby_seat& s : room.seats)
                if (s.user_id != 0 && s.ice != 1)
                    all = false;
        if (all) {
            g_prematch = true;
            g_start_since = Clock::now();
        }
    }
}

/* ---- the pre-match manifest (LC:334-416) ---- */

uint32_t get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void put_u32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s.push_back((char)((v >> (8 * i)) & 0xff));
}

/* Qt's qUncompress: a big-endian u32 length, then a zlib stream. */
bool q_uncompress(const uint8_t* data, size_t size, std::string& out)
{
    if (size < 4)
        return false;
    const uLongf expected = ((uLongf)data[0] << 24) | ((uLongf)data[1] << 16) | ((uLongf)data[2] << 8) | data[3];
    if (expected == 0 || expected > 4u * 1024 * 1024)
        return false;
    out.assign(expected, '\0');
    uLongf length = expected;
    if (uncompress(reinterpret_cast<Bytef*>(&out[0]), &length, data + 4, (uLong)(size - 4)) != Z_OK || length != expected)
        return false;
    return true;
}

/* Qt's qCompress(data, 9). */
std::string q_compress(const std::string& raw)
{
    uLongf bound = compressBound((uLong)raw.size());
    std::string out(4 + bound, '\0');
    out[0] = (char)((raw.size() >> 24) & 0xff);
    out[1] = (char)((raw.size() >> 16) & 0xff);
    out[2] = (char)((raw.size() >> 8) & 0xff);
    out[3] = (char)(raw.size() & 0xff);
    compress2(reinterpret_cast<Bytef*>(&out[4]), &bound, reinterpret_cast<const Bytef*>(raw.data()), (uLong)raw.size(), 9);
    out.resize(4 + bound);
    return out;
}

/* applyPrematchManifest (LC:369-416) up to CoreSetNetplayCheats / CoreSetNetplaySaveData. */
bool stage_manifest(const std::vector<uint8_t>& m, std::string& why, size_t& cheats_count, size_t save_bytes[3])
{
    size_t offset = 8;
    if (m.size() < 20 || std::memcmp(m.data(), "RMGKPMAN", 8) != 0) {
        why = "not a manifest";
        return false;
    }
    const uint32_t version = get_u32(&m[offset]);
    const uint32_t mask = get_u32(&m[offset + 4]);
    const uint32_t cheat_len = get_u32(&m[offset + 8]);
    offset += 12;
    if (version != 1 || (mask & ~3u) != 0 || offset + cheat_len > m.size()) {
        why = "unknown manifest version or sections";
        return false;
    }
    std::vector<CoreCheat> cheats;
    if (!CoreDeserializeNetplayCheats(std::string(m.begin() + offset, m.begin() + offset + cheat_len), cheats)) {
        why = "the cheat list does not read";
        return false;
    }
    offset += cheat_len;
    CoreNetplaySaveData save;
    if (mask & 2u) {
        if (offset + 4 > m.size()) {
            why = "the save section is cut off";
            return false;
        }
        const uint32_t save_len = get_u32(&m[offset]);
        offset += 4;
        std::string raw;
        if (offset + save_len > m.size() || !q_uncompress(&m[offset], save_len, raw) ||
            !CoreDeserializeNetplaySaveData(raw, save)) {
            why = "the save data does not read";
            return false;
        }
        offset += save_len;
    }
    if (offset != m.size()) {
        why = "bytes after the manifest";
        return false;
    }
    if (!CoreSetNetplayCheats(cheats)) {
        why = "the cheats were refused";
        return false;
    }
    CoreSetNetplaySaveData(save);
    cheats_count = cheats.size();
    for (int i = 0; i < 3; ++i)
        save_bytes[i] = save.Kinds[i].size();
    return true;
}

/* During the match (on the lobby thread, after every poll). */
void match_housekeeping()
{
    znx_p2p_lobby_server_up(znx_lobby_connected(g_lb));
    if (!g_match_connected_sent && znx_p2p_visible_frames() > 0) {
        znx_lobby_match_started(g_lb);   /* MATCH_CONNECTED once the game runs (RLD:4472) */
        g_match_connected_sent = true;
    }
    if (!g_match_finished_sent && znx_p2p_lobby_finish_requested()) {
        znx_lobby_match_finished(g_lb);   /* RLD:4503 */
        g_match_finished_sent = true;
        logf("lobby: MATCH_FINISHED sent for match %llu", (unsigned long long)g_match.match_id);
    }
    if (g_match_finished_sent && !g_room_back_reported) {
        /* Over for us, and the server has put the room back to "waiting": the match is over for everyone
         * (those who finished after us get no MATCH_PEER_LEFT for the ones who finished first). */
        znx_lobby_room_detail room;
        if (znx_lobby_current_room(g_lb, &room) == 0 && !std::strcmp(room.state, "waiting")) {
            g_room_back_reported = true;
            for (int i = 0; i < g_match.num_peers && i < 3; ++i)
                znx_p2p_lobby_peer_left(g_match.peers[i].slot, g_match.peers[i].username);
        }
    }
}

void lobby_thread()
{
    while (g_thread_run.load()) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_lb != nullptr) {
                poll_locked();
                if (g_phase == ZNX_LP_IN_GAME)
                    match_housekeeping();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void match_log(const char* line, void*)
{
    if (g_log != nullptr)
        g_log(line);
}
} // namespace

extern "C" void znx_lobby_play_username(const char* name, char* out, size_t out_size)
{
    std::string n;
    for (const char* p = name != nullptr ? name : ""; *p && n.size() < 20; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (std::isalnum(c) || c == '_' || c == '.' || c == '-')
            n.push_back((char)c);
        else if (c == ' ' && !n.empty() && n.back() != '_')
            n.push_back('_');
    }
    while (!n.empty() && n.back() == '_' && n.size() > 2)
        n.pop_back();
    if (n.size() < 2)
        n = "Switch";
    std::snprintf(out, out_size, "%s", n.c_str());
}

extern "C" int znx_lobby_play_open(const struct znx_lobby_play_config* c, char* error, size_t error_size)
{
    znx_lobby_play_close();   /* joins the lobby thread: not under the lock it takes */
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    char name[32];
    znx_lobby_play_username(c->username, name, sizeof(name));
    g_username = name;
    g_server = c->server_url != nullptr && c->server_url[0] ? c->server_url : ZNX_LOBBY_URL_RELEASE;
    g_md5 = c->rom_md5 != nullptr ? c->rom_md5 : "";
    g_rom_name = c->rom_name != nullptr ? c->rom_name : "";
    g_bind = c->bind_address != nullptr ? c->bind_address : "";
    g_local_ip = c->local_ip != nullptr ? c->local_ip : "";
    g_connection = c->connection != nullptr ? c->connection : "";
    g_stun = c->stun != nullptr ? c->stun : "";
    g_log = c->log;

    znx_lobby_config config;
    std::memset(&config, 0, sizeof(config));
    std::string stun_host;
    if (c->stun != nullptr) {
        /* "" / "off": no STUN at all; "host:port" */
        if (std::strcmp(c->stun, "off") != 0 && c->stun[0]) {
            const char* colon = std::strrchr(c->stun, ':');
            stun_host = colon != nullptr ? std::string(c->stun, colon) : std::string(c->stun);
            config.stun_port = colon != nullptr ? std::atoi(colon + 1) : 3478;
        }
        config.stun_host = stun_host.c_str();
    }
    config.use_server_turn = c->no_turn ? 0 : 1;
    config.bind_address = g_bind.empty() ? nullptr : g_bind.c_str();
    config.local_ip = g_local_ip.empty() ? nullptr : g_local_ip.c_str();
    config.connection = g_connection.empty() ? nullptr : g_connection.c_str();
    config.save_data = 1;   /* the save section of the manifest is applied (znx_lobby_play_prepare_core) */
    config.log = lobby_log;
    if (g_log != nullptr)
        znx_lobby_set_ice_log(g_log, 1);   /* libjuice's candidates and state changes into the same log */
    g_lb = znx_lobby_create(&config);
    g_chat.clear();
    g_seated.clear();
    g_ice_reported.clear();
    g_problem = ZNX_LPP_NONE;
    g_leaving = false;
    logf("lobby: opening %s as \"%s\" for %s (%s), STUN %s, ICE bound to %s", g_server.c_str(), g_username.c_str(),
         g_rom_name.c_str(), g_md5.c_str(), c->stun == nullptr ? "from the server" : stun_host.empty() ? "off" : c->stun,
         g_bind.empty() ? "every interface" : g_bind.c_str());
    if (znx_lobby_connect(g_lb, g_server.c_str(), g_username.c_str(), g_md5.c_str(), ZNX_LOBBY_CLIENT_VERSION) != 0) {
        znx_lobby_event ev;
        std::string why = "the address is not a ws:// URL";
        while (znx_lobby_poll(g_lb, &ev))
            if (ev.type == ZNX_LOBBY_EV_DISCONNECTED)
                why = ev.text;
        std::snprintf(error, error_size, "%s: %s", g_server.c_str(), why.c_str());
        znx_lobby_destroy(g_lb);
        g_lb = nullptr;
        g_phase = ZNX_LP_CLOSED;
        return 1;
    }
    znx_lobby_bind_rmgk(g_lb);
    set_phase(ZNX_LP_CONNECTING);
    g_thread_run = true;
    g_thread = std::thread(lobby_thread);
    return 0;
}

extern "C" void znx_lobby_play_close(void)
{
    if (g_thread.joinable()) {
        g_thread_run = false;
        g_thread.join();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_lb == nullptr)
        return;
    znx_lobby_bind_rmgk(nullptr);
    if (znx_lobby_connected(g_lb)) {
        znx_lobby_room_detail room;
        if (znx_lobby_current_room(g_lb, &room) == 0)
            znx_lobby_leave_room(g_lb);
    }
    znx_lobby_destroy(g_lb);
    g_lb = nullptr;
    set_phase(ZNX_LP_CLOSED);
    logf("lobby: closed");
}

extern "C" int znx_lobby_play_is_open(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_lb != nullptr;
}

extern "C" void znx_lobby_play_poll(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    poll_locked();
}

extern "C" void znx_lobby_play_status(struct znx_lobby_play_status* s)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memset(s, 0, sizeof(*s));
    s->phase = g_phase;
    copy(s->server, sizeof(s->server), g_server);
    copy(s->username, sizeof(s->username), g_username);
    if (g_lb != nullptr) {
        copy(s->region, sizeof(s->region), znx_lobby_self_region(g_lb));
        s->users_online = znx_lobby_user_count(g_lb);
    }
    s->seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_phase_since).count();
    if (g_phase == ZNX_LP_STARTING && g_lb != nullptr) {
        znx_lobby_room_detail room;
        if (znx_lobby_current_room(g_lb, &room) == 0)
            for (const znx_lobby_seat& seat : room.seats)
                if (seat.user_id != 0 && seat.user_id != znx_lobby_self_id(g_lb)) {
                    s->start_peers++;
                    if (seat.ice == 1)
                        s->start_connected++;
                }
        s->start_step = g_prematch ? 1 : 0;
        const int limit = g_prematch ? 10 : 15;   /* LC:2575, RLD:5776 */
        const int spent = (int)std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_start_since).count();
        s->start_seconds_left = std::max(0, limit - spent);
    }
}

extern "C" int znx_lobby_play_take_problem(char* title, size_t title_size, char* text, size_t text_size)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const int kind = g_problem;
    if (kind != ZNX_LPP_NONE) {
        std::snprintf(title, title_size, "%s", g_problem_title.c_str());
        std::snprintf(text, text_size, "%s", g_problem_text.c_str());
    }
    g_problem = ZNX_LPP_NONE;
    return kind;
}

extern "C" int znx_lobby_play_rooms(struct znx_lobby_play_room* out, int max, int* all_rooms)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    int n = 0, all = 0;
    if (g_lb != nullptr) {
        const char* self_region = znx_lobby_self_region(g_lb);
        std::map<uint64_t, std::string> regions;
        for (int i = 0; i < znx_lobby_user_count(g_lb); ++i) {
            znx_lobby_user_info u;
            if (znx_lobby_user(g_lb, i, &u) == 0)
                regions[u.id] = u.region;
        }
        for (int i = 0; i < znx_lobby_room_count(g_lb); ++i) {
            znx_lobby_room_info r;
            if (znx_lobby_room(g_lb, i, &r) != 0)
                continue;
            all++;
            if (!same_md5(r.rom_md5, g_md5) || n >= max)
                continue;
            znx_lobby_play_room& o = out[n++];
            std::memset(&o, 0, sizeof(o));
            o.id = r.id;
            copy(o.name, sizeof(o.name), r.name);
            copy(o.host, sizeof(o.host), r.host_name);
            o.players = r.players;
            o.max_players = r.max_players;
            o.in_game = !std::strcmp(r.state, "in_game");
            o.has_password = r.has_password;
            auto it = regions.find(r.host_id);
            o.ping_estimate = it != regions.end() ? region_estimate(self_region, it->second.c_str()) : 0;
        }
    }
    if (all_rooms != nullptr)
        *all_rooms = all;
    return n;
}

extern "C" int znx_lobby_play_room_view(struct znx_lobby_play_room_view* v)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memset(v, 0, sizeof(*v));
    znx_lobby_room_detail room;
    if (g_lb == nullptr || znx_lobby_current_room(g_lb, &room) != 0)
        return -1;
    const uint64_t self = znx_lobby_self_id(g_lb);
    v->id = room.id;
    copy(v->name, sizeof(v->name), room.name);
    copy(v->rom_name, sizeof(v->rom_name), room.rom_name);
    v->same_rom = same_md5(room.rom_md5, g_md5);
    v->is_host = room.host_id == self;
    v->in_game = !std::strcmp(room.state, "in_game");
    v->max_players = room.max_players;
    v->local_delay = room.local_delay;
    v->delay_auto = room.local_delay_auto;
    v->prediction = room.prediction;
    std::string failed, measuring;
    for (int i = 0; i < 4; ++i) {
        const znx_lobby_seat& s = room.seats[i];
        znx_lobby_play_seat& o = v->seats[i];
        o.user_id = s.user_id;
        if (s.user_id == 0)
            continue;
        v->seated++;
        copy(o.name, sizeof(o.name), s.username);
        o.delay = s.frame_delay;
        o.ice = s.ice;
        o.ping = s.ping_ms;
        o.self = s.user_id == self;
        o.host = s.user_id == room.host_id;
        if (o.host)
            copy(v->host_name, sizeof(v->host_name), s.username);
        if (!o.self && s.ice == -1 && failed.empty())
            failed = s.username;
        if (!o.self && s.ping_ms < 0 && measuring.empty())
            measuring = s.username;
    }
    v->can_start = znx_lobby_can_start(g_lb);
    std::string note;
    if (v->in_game)
        note = "A match is being played in this room";
    else if (!v->is_host)
        note = (v->host_name[0] ? std::string(v->host_name) : std::string("The host")) + " starts the match";
    else if (v->can_start)
        note = "Everyone is connected";
    else if (v->seated < 2)
        note = "Waiting for another player to join";
    else if (!failed.empty())
        note = "No direct connection to " + failed;
    else if (!measuring.empty())
        note = "Connecting to " + measuring;
    else
        note = "Measuring the ping between the other players";
    copy(v->start_note, sizeof(v->start_note), note);
    return 0;
}

extern "C" int znx_lobby_play_chat(struct znx_lobby_play_chat_line* out, int max)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const int n = std::min<int>(max, (int)g_chat.size());
    for (int i = 0; i < n; ++i)
        out[i] = g_chat[g_chat.size() - n + i];
    return n;
}

extern "C" int znx_lobby_play_create_room(const char* name, int max_players)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_lb == nullptr || g_phase != ZNX_LP_ROOMS)
        return -1;
    /* CreateRoomDialog's defaults: delay 2, prediction 7; the ROM's GoodName and MD5, region "" */
    if (znx_lobby_create_room(g_lb, name, g_rom_name.c_str(), g_md5.c_str(), "", max_players, 2, 7, nullptr) != 0)
        return -1;
    set_phase(ZNX_LP_ENTERING);
    return 0;
}

extern "C" int znx_lobby_play_join_room(uint64_t room_id)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_lb == nullptr || g_phase != ZNX_LP_ROOMS || znx_lobby_join_room(g_lb, room_id, nullptr) != 0)
        return -1;
    set_phase(ZNX_LP_ENTERING);
    return 0;
}

extern "C" int znx_lobby_play_leave_room(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_lb == nullptr)
        return -1;
    if (g_phase == ZNX_LP_STARTING || g_phase == ZNX_LP_READY)
        znx_lobby_match_finished(g_lb);   /* abandons the match being set up (RLD:5757-5758) */
    if (znx_lobby_leave_room(g_lb) != 0)
        return -1;
    g_leaving = true;
    g_seated.clear();
    set_phase(ZNX_LP_ROOMS);
    return 0;
}

extern "C" int znx_lobby_play_send_chat(const char* text)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_lb != nullptr ? znx_lobby_send_chat(g_lb, "room", text) : -1;
}

extern "C" int znx_lobby_play_set_delay(int delay)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_lb != nullptr ? znx_lobby_set_delay(g_lb, delay) : -1;
}

extern "C" int znx_lobby_play_start(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_lb == nullptr || !znx_lobby_can_start(g_lb))
        return -1;
    logf("lobby: ROOM_START");
    return znx_lobby_start_match(g_lb);
}

extern "C" int znx_lobby_play_test_manifest(int cheat_value, int with_save)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_lb == nullptr)
        return -1;
    std::vector<CoreCheat> cheats;
    if (cheat_value >= 0) {
        CoreCheat cheat;
        cheat.Name = "znx test byte";
        cheat.Author = "znx-net";
        cheat.Note = "8-bit write at 0x80700000, an RDRAM byte the game never uses";
        CoreCheatCode code;
        code.Address = 0x80700000u;
        code.Value = cheat_value & 0xff;
        cheat.CheatCodes.push_back(code);
        cheats.push_back(cheat);
    }
    std::string cheat_bytes;
    if (!CoreSerializeNetplayCheats(cheats, cheat_bytes))
        return -1;
    std::string save_section;
    if (with_save) {
        CoreNetplaySaveData save;
        save.Kinds[(int)CoreSaveDataKind::Sram].resize(0x8000);
        for (size_t i = 0; i < 0x8000; ++i)
            save.Kinds[(int)CoreSaveDataKind::Sram][i] = (uint8_t)(i * 7 + 3);
        std::string raw;
        if (!CoreSerializeNetplaySaveData(save, raw))
            return -1;
        save_section = q_compress(raw);
    }
    /* buildPrematchManifest (LC:334-367) */
    std::string m("RMGKPMAN", 8);
    put_u32(m, 1);
    put_u32(m, 1u | (with_save ? 2u : 0u));
    put_u32(m, (uint32_t)cheat_bytes.size());
    m += cheat_bytes;
    if (with_save) {
        put_u32(m, (uint32_t)save_section.size());
        m += save_section;
    }
    logf("lobby: test manifest: %zu bytes (%zu cheat%s, %s save)", m.size(), cheats.size(), cheats.size() == 1 ? "" : "s",
         with_save ? "an SRAM" : "no");
    return znx_lobby_set_prematch_manifest(g_lb, reinterpret_cast<const uint8_t*>(m.data()), m.size());
}

extern "C" int znx_lobby_play_input_players(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_match.total_seats > 0 ? g_match.total_seats : 2;
}

extern "C" int znx_lobby_play_local_slot(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_match.local_slot;
}

extern "C" int znx_lobby_play_local_delay(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_match.local_delay;
}

extern "C" int znx_lobby_play_prepare_core(void* core)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!m64p::Core.Hook(core)) {
        logf("lobby: cannot hook the core: %s", m64p::Core.GetLastError().c_str());
        return 1;
    }
    std::string why;
    size_t cheats = 0, save[3] = { 0, 0, 0 };
    if (!stage_manifest(g_manifest, why, cheats, save)) {
        logf("lobby: the pre-match manifest (%zu bytes) cannot be applied: %s", g_manifest.size(), why.c_str());
        return 1;
    }
    /* CoreStartEmulation: netplay cheats after the plugins are attached, then the synced save data
     * (Emulation.cpp:1021-1031, 1104) */
    if (!CoreApplyNetplayCheats()) {
        logf("lobby: the manifest's cheats were refused by the core");
        return 1;
    }
    if (!CoreApplyNetplaySaveData(true)) {
        logf("lobby: the manifest's save data was refused by the core");
        return 1;
    }
    logf("lobby: manifest applied (%zu bytes, hash %016llx): %zu cheat%s, save EEPROM %zu / SRAM %zu / FlashRAM %zu bytes%s",
         g_manifest.size(), (unsigned long long)g_match.manifest_hash, cheats, cheats == 1 ? "" : "s", save[0], save[1],
         save[2], save[0] + save[1] + save[2] == 0 ? " (blank)" : "");
    return 0;
}

extern "C" int znx_lobby_play_run_session(void* core, const struct znx_p2p_session_hooks* hooks)
{
    znx_p2p_lobby_match match;
    std::memset(&match, 0, sizeof(match));
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_lb == nullptr || g_phase != ZNX_LP_READY) {
            logf("lobby: no match to run");
            return 1;
        }
        match.local_slot = g_match.local_slot;
        match.seats = g_match.seats;
        match.delay = g_match.local_delay;
        match.prediction = g_match.prediction;
        for (int i = 0; i < g_match.num_peers && i < 3; ++i) {
            match.remote_slot[match.num_remotes] = g_match.peers[i].slot;
            match.remote_user[match.num_remotes] = g_match.peers[i].user_id;
            match.num_remotes++;
        }
        match.control = nullptr;   /* the lobby thread polls and reports (match_housekeeping) */
        match.log = match_log;
        znx_lobby_bind_rmgk(g_lb);
        set_phase(ZNX_LP_IN_GAME);
    }
    const int result = znx_p2p_run_lobby_session(core, hooks, &match);
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        /* the netplay cheats go with this game (the core still has them: it shuts down after this) */
        if (m64p::Core.Hook(core)) {
            CoreClearCheats();
            m64p::Core.Unhook();
        }
        CoreClearNetplaySaveData();
        if (g_lb != nullptr && !g_match_finished_sent) {
            znx_lobby_match_finished(g_lb);
            g_match_finished_sent = true;
        }
        char why[256];
        const int end = znx_p2p_last_end(why, sizeof(why));
        switch (end) {
        case ZNX_P2P_END_PEER_DROP:
            g_end_title = "The match ended";
            break;
        case ZNX_P2P_END_TIMEOUT:
            g_end_title = "The connection was lost";
            break;
        case ZNX_P2P_END_ERROR:
            g_end_title = "The match could not start";
            break;
        default:
            g_end_title = "";
            break;
        }
        g_end_text = why;
        if (result != 0 && g_end_title.empty())
            g_end_title = "The match could not start";
    }
    return result;
}

extern "C" int znx_lobby_play_after_game(char* title, size_t title_size, char* text, size_t text_size)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    poll_locked();
    if (g_lb == nullptr) {
        std::snprintf(title, title_size, "The lobby is closed");
        std::snprintf(text, text_size, "Open it again to play another match.");
        return 0;
    }
    if (g_lost_in_game || !znx_lobby_connected(g_lb)) {
        set_phase(ZNX_LP_FAILED);
        std::snprintf(title, title_size, "The lobby connection dropped during the match");
        std::snprintf(text, text_size, "%s%sOpen the lobby again to reconnect.", g_end_text.c_str(),
                      g_end_text.empty() ? "" : " ");
        g_problem = ZNX_LPP_NONE;
        return 0;
    }
    znx_lobby_room_detail room;
    const bool in_room = znx_lobby_current_room(g_lb, &room) == 0;
    set_phase(in_room ? ZNX_LP_ROOM : ZNX_LP_ROOMS);
    if (!g_end_text.empty())
        system_line(g_end_text);
    if (!g_end_title.empty()) {
        std::snprintf(title, title_size, "%s", g_end_title.c_str());
        std::snprintf(text, text_size, "%s", g_end_text.empty() ? "The reason is in latest.log." : g_end_text.c_str());
        return 0;
    }
    return 1;
}
