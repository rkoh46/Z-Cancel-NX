// SPDX-License-Identifier: GPL-3.0-or-later
/* See znx_p2p.h. File:line references are to ref/RMG-K (v0.9.16, 2f5454a). */

#include "znx_p2p.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "core/p2p_core.h"
#include "m64p/Api.hpp"
#include "rmgk_gekko.hpp"

#include "../znx_net.h"

namespace
{
using Clock = std::chrono::steady_clock;

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

/* KailleraP2PDialog.cpp:59-79 */
const char* const kModePrefix = "RMGK:MODE:";
const char* const kDelayPrefix = "RMGK:RBDELAY:";
const char* const kPredictionPrefix = "RMGK:RBPRED:";
const char* const kPacingPrefix = "RMGK:RBPACE:";
const char* const kTraversal = "N02TRAV1";
constexpr int kDelayModeDefault = 0;   /* automatic from the ping */
constexpr int kDelayModeCustom = 3;
constexpr int kDefaultDelay = 2;
constexpr int kDefaultPrediction = 7;
constexpr int kPacingMode = 1;         /* Rollback_PacingMode's default (Settings.cpp:384); RMG-K ignores it now */
constexpr long long kDirectJoinTimeoutMs = 7000;
constexpr long long kDelayInitialUpdateMs = 5000;
constexpr int kDelayInitialSamples = 5;
constexpr long long kDelayUpdateIntervalMs = 10000;
constexpr long long kDelaySampleWindowMs = 20000;

/* ---- configuration and state (g_mutex guards everything below it that both threads touch) ---- */

struct Config {
    bool host = false;
    int port = ZNX_P2P_DEFAULT_PORT;
    std::string game, username, app = ZNX_P2P_APP, target;
    int delay = 0, prediction = 0;
    bool use_codes = true, enlist = false;
    std::string nat_host = ZNX_P2P_NAT_HOST, list_host = ZNX_P2P_LIST_HOST;
    int nat_port = ZNX_P2P_NAT_PORT, list_port = ZNX_P2P_LIST_PORT;
    void (*save_identity)(const char*, const char*, void*) = nullptr;
    void (*log)(const char*, void*) = nullptr;
    void* user = nullptr;
};

Config g_cfg;
std::recursive_mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_open{false};

/* lobby */
int g_phase = ZNX_P2P_CLOSED;
std::string g_message;
std::string g_peer_name, g_peer_app, g_host_game, g_chat;
bool g_peer_connected = false;
bool g_peer_ready = false, g_local_ready = false;
bool g_got_mode = false, g_mode_rollback = false;
bool g_app_mismatch = false;
bool g_ping_timeout = false;        /* n02 said "Ping timeout. Peer dropped." before the peer-left callback */
int g_ping = -1;
int g_host_delay = -1, g_host_delay_mode = kDelayModeDefault, g_host_prediction = 0;   /* join: the host's */
bool g_announce = false;            /* host: send the RMGK: settings at the next step */
int g_last_sent_delay = -1, g_last_sent_mode = -1;
std::atomic<int> g_game_player{0};  /* p2p_game_callback */
int g_port = 0;                     /* our n02 port (p2p_core_get_port, cached: see the lock rule below) */
/* Requests from the program's thread, carried out by the lobby thread. Lock rule: n02 takes its own
 * mutex in every call and calls back into this file with it held, and the callbacks take g_mutex. So
 * no thread may call n02 while holding g_mutex unless n02 is only ever entered from that thread or
 * already held: every n02 call outside a callback happens on the lobby thread (or, during a match,
 * the transport and p2p_drop_game on the emulation thread, whose callbacks never take g_mutex). */
int g_req_ready = -1;
bool g_req_delay = false;

/* automatic delay (KailleraP2PDialog.cpp:1931-2070) */
struct PingSample { long long t; int ping; };
std::vector<PingSample> g_ping_samples;
long long g_delay_last_update = 0;
int g_auto_delay = kDefaultDelay;

/* traversal, host (KailleraP2PDialog.cpp:2662-3190, 3943-4110) */
std::string g_code, g_token, g_live_token;
int g_reg_attempts = 0;
long long g_next_reg = 0, g_next_keep = 0;
bool g_suspended = false;           /* a peer is connected: no live host session, not listed */
int g_code_state = 0;
std::string g_public_address;
bool g_whatismyip_pending = false;
std::string g_punch_peer_ip;
int g_punch_peer_port = 0;
long long g_punch_deadline = 0, g_next_punch = 0;
bool g_enlisted = false;
int g_timer_step = 0;

/* traversal / direct, join */
bool g_join_by_code = false, g_join_active = false, g_got_host = false, g_fallback_tried = false;
std::string g_join_code, g_join_ip, g_join_token, g_fallback;
int g_join_port = 0, g_punch_attempts = 0;
long long g_next_join = 0, g_join_deadline = 0, g_next_connect = 0;

/* session */
std::atomic<bool> g_in_game{false};
std::atomic<bool> g_peer_dropped{false};
std::atomic<bool> g_peer_left{false};
std::atomic<bool> g_local_drop{false};
std::atomic<int> g_end{ZNX_P2P_END_NONE};
std::string g_end_text;
const znx_p2p_session_hooks* g_hooks = nullptr;
znx_p2p_session_hooks g_hooks_copy;
std::atomic<int> g_visible{0};
std::atomic<int> g_rollbacks{0}, g_deepest{0}, g_live_ping{-1}, g_live_delay{0};
std::atomic<float> g_live_ahead{0};
std::atomic<double> g_speed{1.0};
int g_depth = 0;

/* a lobby match (znx_p2p_run_lobby_session): no n02; the lobby's control callback reports drops */
std::atomic<bool> g_lobby_mode{false};
znx_p2p_lobby_match g_lobby{};
std::atomic<bool> g_lobby_finish{false};
std::atomic<bool> g_lobby_server{true};
std::atomic<unsigned> g_lobby_left{0};   /* bit s: seat s left the match */
std::string g_lobby_left_name;           /* g_mutex */

void note(const char* format, ...)
{
    char line[600];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_cfg.log != nullptr)
        g_cfg.log(line, g_cfg.user);
    else if (g_lobby_mode.load() && g_lobby.log != nullptr)
        g_lobby.log(line, g_lobby.user);
}

void set_phase(int phase, const std::string& message)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_phase = phase;
    g_message = message;
}

void fail(const std::string& message)
{
    note("p2p: %s", message.c_str());
    set_phase(ZNX_P2P_FAILED, message);
}

/* ---- the n02 calls the dialog makes ---- */

void send_chat(const std::string& text)
{
    std::vector<char> buffer(text.begin(), text.end());
    buffer.push_back(0);
    p2p_send_chat(buffer.data());
}

void send_text(const std::string& host, int port, const std::string& text, bool with_nul)
{
    /* p2p_send_ssrv_packet: a datagram that starts with 0 then the text; traversal messages
     * without a trailing NUL (travSendToServer), super-server commands with it (ssrvSend). */
    std::vector<char> buffer(text.begin(), text.end());
    buffer.push_back(0);
    std::vector<char> hostname(host.begin(), host.end());
    hostname.push_back(0);
    p2p_send_ssrv_packet(buffer.data(), (int)text.size() + (with_nul ? 1 : 0), hostname.data(), port);
}

void trav_send(const std::string& message)
{
    /* the type only: the tokens are the code's credentials */
    const size_t bar = message.find('|', std::strlen(kTraversal) + 1);
    note("p2p: traversal -> %s %s", message.substr(0, bar).c_str(),
        message.rfind(std::string(kTraversal) + "|JOIN|", 0) == 0 ? message.substr(bar + 1).c_str() : "");
    send_text(g_cfg.nat_host, g_cfg.nat_port, message, false);
}

void ssrv_send(const std::string& message)
{
    note("p2p: list server -> %s", message.c_str());
    send_text(g_cfg.list_host, g_cfg.list_port, message, true);
}

void punch(const std::string& ip, int port, const std::string& token)
{
    /* travPunchEndpoint: ten PUNCH datagrams to open the NAT mappings */
    if (ip.empty() || port <= 0)
        return;
    const std::string message = std::string(kTraversal) + "|PUNCH|" + token;
    for (int i = 0; i < 10; i++)
        send_text(ip, port, message, false);
}

std::string enlist_app()
{
    /* buildEnlistAppName: APP, plus " {CC:code}" while hosting by code (p2p_appcode.h) */
    std::string app = g_cfg.app;
    if (g_cfg.use_codes && g_code_state != 2 && !g_code.empty())
        app += " {CC:" + g_code + "}";
    return app;
}

void enlist()
{
    /* enlistGame: ENLISP (with the port) when hosting by code or on another port than 27886 */
    const int port = p2p_core_get_port();
    if ((g_cfg.use_codes && g_code_state != 2) || port != ZNX_P2P_DEFAULT_PORT)
        ssrv_send("ENLISP " + g_cfg.game + "|" + enlist_app() + "|" + g_cfg.username + "|" + std::to_string(port));
    else
        ssrv_send("ENLIST " + g_cfg.game + "|" + enlist_app() + "|" + g_cfg.username);
    g_enlisted = true;
}

void unenlist()
{
    if (g_enlisted)
        ssrv_send("UNENLIST");
    g_enlisted = false;
}

void save_identity()
{
    if (g_cfg.save_identity != nullptr)
        g_cfg.save_identity(g_code.c_str(), g_token.c_str(), g_cfg.user);
}

int effective_delay_locked()
{
    if (g_cfg.host)
        return std::clamp(g_cfg.delay > 0 ? g_cfg.delay : g_auto_delay, 0, 9);
    return g_host_delay >= 0 ? std::clamp(g_host_delay, 0, 9) : kDefaultDelay;
}

int effective_prediction_locked()
{
    const int window = g_cfg.host ? g_cfg.prediction : g_host_prediction;
    return window > 0 && window <= 10 ? window : kDefaultPrediction;
}

void send_settings(bool force)
{
    /* sendGameLayer + sendRollbackDelaySettings + sendRollbackPredictionSettings +
     * sendRollbackPacingSettings (KailleraP2PDialog.cpp:2508, 2090, 2116, 1676) */
    int mode, delay, prediction;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        mode = g_cfg.delay > 0 ? kDelayModeCustom : kDelayModeDefault;
        delay = effective_delay_locked();
        prediction = g_cfg.prediction > 0 && g_cfg.prediction <= 10 ? g_cfg.prediction : 0;
        if (!force && mode == g_last_sent_mode && delay == g_last_sent_delay)
            return;
        g_last_sent_mode = mode;
        g_last_sent_delay = delay;
    }
    if (force)
        send_chat("RMGK:MODE:RB");
    send_chat(std::string(kDelayPrefix) + std::to_string(mode) + ":" + std::to_string(delay));
    if (force) {
        send_chat(std::string(kPredictionPrefix) + std::to_string(prediction));
        send_chat(std::string(kPacingPrefix) + std::to_string(kPacingMode));
    }
}

/* maybeUpdateAutomaticRollbackDelay: the 75th percentile of the last 20 s of pings, first after five
 * samples or five seconds, then every ten seconds. Returns true if the delay changed. */
bool update_auto_delay_locked(long long now)
{
    while (!g_ping_samples.empty() && now - g_ping_samples.front().t > kDelaySampleWindowMs)
        g_ping_samples.erase(g_ping_samples.begin());
    if (g_ping_samples.empty())
        return false;
    if (g_delay_last_update == 0) {
        if ((int)g_ping_samples.size() < kDelayInitialSamples && now - g_ping_samples.front().t < kDelayInitialUpdateMs)
            return false;
    } else if (now - g_delay_last_update < kDelayUpdateIntervalMs) {
        return false;
    }
    std::vector<int> pings;
    for (const auto& s : g_ping_samples)
        pings.push_back(s.ping);
    std::sort(pings.begin(), pings.end());
    const int previous = g_auto_delay;
    g_auto_delay = znx_p2p_auto_delay(pings[(pings.size() - 1) * 3 / 4]);
    g_delay_last_update = now;
    return previous != g_auto_delay;
}

bool parse_ipv4_port(const char* s, std::string& ip, int& port)
{
    /* tryExtractIPv4AndPort */
    ip.clear();
    port = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            continue;
        unsigned octets[4];
        const char* cur = p;
        bool ok = true;
        for (int o = 0; o < 4 && ok; o++) {
            unsigned value = 0;
            int digits = 0;
            while (*cur >= '0' && *cur <= '9' && digits < 3) {
                value = value * 10 + (unsigned)(*cur - '0');
                cur++;
                digits++;
            }
            if (digits == 0 || value > 255) ok = false;
            octets[o] = value;
            if (ok && o < 3) {
                if (*cur != '.') ok = false;
                else cur++;
            }
        }
        if (!ok)
            continue;
        char text[32];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", octets[0], octets[1], octets[2], octets[3]);
        ip = text;
        while (*cur == ' ' || *cur == '\t') cur++;
        if (*cur == ':') cur++;
        else if (*cur != 0 && (*cur < '0' || *cur > '9')) return true;
        unsigned value = 0;
        int digits = 0;
        while (*cur >= '0' && *cur <= '9' && digits < 5) {
            value = value * 10 + (unsigned)(*cur - '0');
            cur++;
            digits++;
        }
        if (digits > 0 && value > 0 && value <= 65535)
            port = (int)value;
        return true;
    }
    return false;
}

std::string friendly_traversal_error(const std::string& reason, const std::string& code)
{
    /* friendlyNatTraversalErrorMessage, in this program's words */
    const std::string who = code.empty() ? "That code" : "The code " + code;
    if (reason == "UNKNOWNCODE") return who + " was not found. Check it with the host.";
    if (reason == "OFFLINE") return who + " exists, but its host is not hosting right now.";
    if (reason == "BUSY") return "Someone else is connecting to that host. Wait a moment and try again.";
    if (reason == "RATELIMIT") return "Too many requests went to the connect-code server. Wait a moment and try again.";
    if (reason == "FULL") return "The connect-code server is full. Try again later, or join by IP address.";
    if (reason == "BADREQ") return "The connect-code server did not understand the request.";
    return "The connect-code server answered " + reason + ".";
}

/* ---- traversal and list server replies (p2p_ssrv_packet_recv_callback, lobby thread) ---- */

void handle_traversal(const std::string& text, const sockaddr_in* from)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (parts.size() < 8) {
        const size_t bar = text.find('|', start);
        parts.push_back(text.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos)
            break;
        start = bar + 1;
    }
    if (parts.size() < 2)
        return;
    const std::string& type = parts[1];
    (void)from;
    if (type == "PUNCH")
        return;
    note("p2p: traversal <- %s", type.c_str());
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_cfg.host && type == "CLAIMOK" && parts.size() >= 4) {
        char code[32];
        if (!znx_p2p_normalize_code(parts[2].c_str(), code, sizeof(code)))
            std::snprintf(code, sizeof(code), "%s", parts[2].c_str());
        g_code = code;
        g_token = parts[3];
        g_live_token.clear();
        g_reg_attempts = 0;
        save_identity();
        trav_send(std::string(kTraversal) + "|CLAIMACK|" + g_token);
        trav_send(std::string(kTraversal) + "|HOSTOPEN|" + g_token);
        g_next_reg = now_ms() + 2000;
    } else if (g_cfg.host && type == "HOSTOK" && parts.size() >= 6) {
        char code[32];
        if (!znx_p2p_normalize_code(parts[2].c_str(), code, sizeof(code)))
            std::snprintf(code, sizeof(code), "%s", parts[2].c_str());
        g_code = code;
        g_live_token = parts[3];
        g_reg_attempts = 0;
        g_next_reg = 0;
        g_next_keep = now_ms() + 10000;
        g_code_state = 1;
        save_identity();
        note("p2p: hosting by code %s", g_code.c_str());
        if (g_cfg.enlist && !g_suspended)
            enlist();
    } else if (!g_cfg.host && type == "HOST" && parts.size() >= 5 && g_join_active) {
        g_join_token = parts[2];
        g_join_ip = parts[3];
        g_join_port = std::atoi(parts[4].c_str());
        g_got_host = true;
        if (g_fallback.empty()) {
            g_fallback = g_join_ip;
            g_fallback_tried = false;
        }
        g_next_join = 0;
        g_punch_attempts++;
        punch(g_join_ip, g_join_port, g_join_token);
        std::vector<char> ip(g_join_ip.begin(), g_join_ip.end());
        ip.push_back(0);
        p2p_core_connect(ip.data(), g_join_port);
        g_next_connect = now_ms() + 1000;
    } else if (g_cfg.host && type == "PEER" && parts.size() >= 5) {
        if (!g_live_token.empty() && parts[2] != g_live_token)
            return;
        g_punch_peer_ip = parts[3];
        g_punch_peer_port = std::atoi(parts[4].c_str());
        g_punch_deadline = now_ms() + 15000;
        g_next_punch = now_ms() + 1000;
        punch(g_punch_peer_ip, g_punch_peer_port, parts[2]);
    } else if (type == "ERR" && parts.size() >= 3) {
        const std::string& reason = parts[2];
        if (!g_cfg.host && g_join_active) {
            g_join_active = false;
            g_phase = ZNX_P2P_FAILED;
            g_message = friendly_traversal_error(reason, g_join_code);
        } else if (g_cfg.host && reason == "NOAUTH") {
            /* travClearIdentity: the saved code is no longer ours; get a new one */
            g_code.clear();
            g_token.clear();
            g_live_token.clear();
            save_identity();
            g_reg_attempts = 0;
            g_next_reg = 0;
            g_next_keep = 0;
            g_code_state = 0;
        } else if (g_cfg.host && reason == "NOSESSION") {
            g_live_token.clear();
            g_next_reg = 0;
            g_next_keep = 0;
        } else {
            note("p2p: traversal error %s", reason.c_str());
        }
    }
}

void handle_ssrv(const char* data, int len, const sockaddr_in* from)
{
    if (len <= 0)
        return;
    std::string text(data, (size_t)len);
    const size_t nul = text.find('\0');
    if (nul != std::string::npos)
        text.resize(nul);
    if (text.rfind(std::string(kTraversal) + "|", 0) == 0) {
        handle_traversal(text, from);
        return;
    }
    if (text.rfind("PINGRQ", 0) == 0) {
        /* the list server measures the ping: answer at once */
        if (from != nullptr) {
            char reply[] = "xxxxxxxxxx";
            p2p_send_ssrv_packet(reply, 11, (void*)from);
        }
        return;
    }
    if (text.rfind("MSG", 0) == 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_whatismyip_pending) {
        std::string ip;
        int port = 0;
        if (parse_ipv4_port(text.c_str(), ip, port)) {
            if (port <= 0)
                port = p2p_core_get_port();
            g_public_address = ip + ":" + std::to_string(port);
            note("p2p: the list server sees this console at %s", g_public_address.c_str());
        }
        g_whatismyip_pending = false;
    }
}

/* ---- the lobby thread ---- */

void host_timer(long long now)
{
    /* onTravTimer, host part */
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_cfg.use_codes && !g_suspended) {
        if (g_token.empty() || g_code.empty()) {
            if (g_next_reg == 0 || now >= g_next_reg) {
                if (g_reg_attempts >= 4) {
                    if (g_code_state != 2) {
                        /* RMG-K: "Failed to get a connect code ... Hosting by IP instead" */
                        note("p2p: no connect code from %s after 4 tries; hosting by IP", g_cfg.nat_host.c_str());
                        g_code_state = 2;
                        g_whatismyip_pending = true;
                        ssrv_send("WHATISMYIP");
                    }
                    g_next_reg = now + 30000;   /* RMG-K stops here; trying again now and then costs nothing */
                    g_reg_attempts = 3;
                } else {
                    trav_send(std::string(kTraversal) + "|CLAIM|AUTO");
                    g_reg_attempts++;
                    g_next_reg = now + 2000;
                }
            }
        } else if (g_live_token.empty()) {
            if (g_next_reg == 0 || now >= g_next_reg) {
                trav_send(std::string(kTraversal) + "|HOSTOPEN|" + g_token);
                g_next_reg = now + 2000;
                if (++g_reg_attempts >= 4 && g_code_state == 0) {
                    /* a saved code but no answer: show the IP address meanwhile, keep asking */
                    g_code_state = 2;
                    g_whatismyip_pending = true;
                    ssrv_send("WHATISMYIP");
                }
            }
        } else if (g_next_keep == 0 || now >= g_next_keep) {
            trav_send(std::string(kTraversal) + "|HOSTKEEP|" + g_live_token);
            g_next_keep = now + 10000;
        }
        if (!p2p_is_connected() && !g_punch_peer_ip.empty() && g_punch_peer_port > 0 && now < g_punch_deadline &&
            now >= g_next_punch) {
            punch(g_punch_peer_ip, g_punch_peer_port, g_live_token);
            g_next_punch = now + 1000;
        }
    }
    if (g_timer_step % 30 == 0 && !p2p_is_connected() && g_cfg.enlist && !g_peer_connected)
        enlist();
}

void try_fallback_connect()
{
    /* travTryFallbackConnect: the host's address on 27886 */
    if (g_fallback_tried || g_fallback.empty())
        return;
    g_fallback_tried = true;
    std::string ip;
    int port = 0;
    if (!parse_ipv4_port(g_fallback.c_str(), ip, port))
        return;
    if (port <= 0)
        port = ZNX_P2P_DEFAULT_PORT;
    note("p2p: trying the host directly at %s:%d", ip.c_str(), port);
    std::vector<char> address(ip.begin(), ip.end());
    address.push_back(0);
    p2p_core_connect(address.data(), port);
}

void join_timer(long long now)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_join_active)
        return;
    if (p2p_is_connected()) {
        g_join_active = false;
        return;
    }
    if (g_join_deadline != 0 && now >= g_join_deadline) {
        g_join_active = false;
        if (g_join_by_code) {
            g_phase = ZNX_P2P_FAILED;
            g_message = g_got_host
                ? "The host of " + g_join_code + " did not answer. It may need to forward UDP port " +
                  std::to_string(ZNX_P2P_DEFAULT_PORT) + ", or try joining by IP address."
                : "The connect-code server did not answer. Check the internet connection, or join by IP address.";
            try_fallback_connect();
        } else {
            g_phase = ZNX_P2P_FAILED;
            g_message = "Nobody answered at " + g_cfg.target + ". Check the address and that the host is waiting.";
        }
        return;
    }
    if (!g_join_by_code) {
        if (now >= g_next_connect) {
            /* RMG-K sends the login once; sending it again each second costs nothing (the host
             * ignores a repeat) and survives a lost first datagram. */
            std::vector<char> ip(g_join_ip.begin(), g_join_ip.end());
            ip.push_back(0);
            p2p_core_connect(ip.data(), g_join_port);
            g_next_connect = now + 1000;
        }
        return;
    }
    if (!g_got_host) {
        if (g_next_join == 0 || now >= g_next_join) {
            trav_send(std::string(kTraversal) + "|JOIN|" + g_join_code + "|" + std::to_string((unsigned)now));
            g_next_join = now + 3000;
        }
    } else if (g_next_connect == 0 || now >= g_next_connect) {
        g_punch_attempts++;
        punch(g_join_ip, g_join_port, g_join_token);
        std::vector<char> ip(g_join_ip.begin(), g_join_ip.end());
        ip.push_back(0);
        p2p_core_connect(ip.data(), g_join_port);
        g_next_connect = now + 1000;
        if (g_punch_attempts >= 3)
            try_fallback_connect();
    }
}

void lobby_thread()
{
    long long next_timer = now_ms() + 1000, next_ping = 0;
    bool was_connected = false;
    while (g_running.load()) {
        const long long now = now_ms();
        if (g_game_player.load() == 0) {
            /* KailleraP2PDialog::onStepTimer: n02's lobby state machine */
            p2p_step();
            bool connected = p2p_is_connected();
            bool announce = false, update = false, joined_peer;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                joined_peer = g_peer_connected;
                if (g_announce && connected) {
                    announce = true;
                    g_announce = false;
                }
            }
            if (connected != was_connected) {
                was_connected = connected;
                if (connected)
                    next_ping = 0;
            }
            if (announce && g_cfg.host)
                send_settings(true);
            int ready;
            bool delay_changed;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                ready = g_req_ready;
                delay_changed = g_req_delay;
                g_req_ready = -1;
                g_req_delay = false;
            }
            if (ready >= 0) {
                if (ready && g_cfg.host)
                    send_settings(true);   /* onReady: the host announces its settings again */
                p2p_set_ready(ready != 0);
            }
            if (delay_changed && connected)
                send_settings(false);
            if (connected && joined_peer && now >= next_ping) {
                p2p_ping();   /* onTravTimer: once a second while a peer is in the lobby */
                next_ping = now + 1000;
            }
            if (now >= next_timer) {
                next_timer = now + 1000;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_mutex);
                    g_timer_step++;
                    if (g_cfg.host && g_cfg.delay == 0 && g_peer_connected)
                        update = update_auto_delay_locked(now);
                }
                if (update)
                    send_settings(false);
                if (g_cfg.host)
                    host_timer(now);
                else
                    join_timer(now);
            } else if (!g_cfg.host) {
                /* joining: the first JOIN / login goes out at once, not a second later */
                bool first;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_mutex);
                    first = g_join_active && ((g_join_by_code && g_next_join == 0) || (!g_join_by_code && g_next_connect == 0));
                }
                if (first)
                    join_timer(now);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            /* onStepTimer while a rollback game is active: CHAT, DROP, EXIT and PING only;
             * GekkoNet reads its own datagrams from the same socket */
            p2p_rollback_process_control();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

std::string lower_ascii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool parse_int_after(const char* message, const char* prefix, int* a, int* b)
{
    const size_t n = std::strlen(prefix);
    if (std::strncmp(message, prefix, n) != 0)
        return false;
    const char* rest = message + n;
    *a = std::atoi(rest);
    if (b != nullptr) {
        const char* colon = std::strchr(rest, ':');
        *b = colon != nullptr ? std::atoi(colon + 1) : -1;
    }
    return true;
}

/* ---- the match: the debug hooks rmgk_gekko calls around every visible frame ---- */

bool begin_visible_hook(void*)
{
    if (g_hooks != nullptr && g_hooks->begin_visible != nullptr && !g_hooks->begin_visible(g_hooks->user))
        return false;
    return true;
}

bool end_visible_hook(void*)
{
    const int visible = ++g_visible;
    if (visible % 30 == 0) {
        RollbackLagStats lag;
        rmgk_gekko::lag_stats(lag);
        if (!lag.peers.empty()) {
            g_live_ping = lag.peers[0].pingMs;
            g_live_ahead = lag.peers[0].framesAhead;
        }
    }
    if (g_hooks != nullptr && g_hooks->end_visible != nullptr && !g_hooks->end_visible(g_hooks->user))
        return false;
    return true;
}

void session_control_loop(std::atomic<bool>& running)
{
    /* The dialog's part while the game runs, on its own thread: stop the session when the peer
     * leaves (DROP or EXIT, from the lobby thread's p2p_rollback_process_control), when GekkoNet
     * loses the peer, or when nothing moves for 20 s. */
    int last_visible = -1;
    Clock::time_point last_progress = Clock::now();
    bool seen_connected = false;
    int lost_count = 0;
    const bool lobby = g_lobby_mode.load();
    unsigned remote_seats = 0;
    for (int i = 0; lobby && i < g_lobby.num_remotes; ++i)
        remote_seats |= 1u << g_lobby.remote_slot[i];
    while (running.load()) {
        if (lobby && g_lobby.control != nullptr)
            g_lobby.control(g_lobby.user);   /* the lobby's WebSocket, ICE signalling, MATCH_PEER_LEFT */
        const auto now = Clock::now();
        const int visible = g_visible.load();
        if (visible != last_visible) {
            last_visible = visible;
            last_progress = now;
        }
        int end = ZNX_P2P_END_NONE;
        std::string text;
        if (lobby) {
            /* RollbackLobbyDialog::onMatchWatchTick: close the game when every other player is gone, or
             * when the lobby is lost and the game stands still (10 s). No late join here, so the host
             * closes too. */
            const unsigned left = g_lobby_left.load();
            const bool everyone_left = remote_seats != 0 && (left & remote_seats) == remote_seats;
            if (everyone_left && g_lobby_finish.load()) {
                /* over for us (MATCH_FINISHED sent) and for everyone else: nothing more to wait for */
                end = ZNX_P2P_END_LOCAL;
                text = "The match is over.";
            } else if (g_hooks != nullptr && g_hooks->stop_on_peer_drop && everyone_left) {
                end = ZNX_P2P_END_PEER_DROP;
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                text = g_lobby.num_remotes == 1 && !g_lobby_left_name.empty()
                    ? g_lobby_left_name + " left the match." : "Every other player left the match.";
            } else if (!g_lobby_server.load() && now - last_progress > std::chrono::seconds(10)) {
                end = ZNX_P2P_END_TIMEOUT;
                text = "The connection to the lobby was lost and the game stopped moving.";
            }
        }
        if (end != ZNX_P2P_END_NONE) {
        } else if (!lobby && g_peer_left.load()) {
            end = ZNX_P2P_END_PEER_LEFT;
            text = "The other player left.";
        } else if (!lobby && g_peer_dropped.load() && (g_hooks == nullptr || g_hooks->stop_on_peer_drop)) {
            end = ZNX_P2P_END_PEER_DROP;
            text = "The other player ended the match.";
        } else if (now - last_progress > std::chrono::seconds(20)) {
            end = ZNX_P2P_END_TIMEOUT;
            text = visible == 0 ? "The other player's game did not start within 20 seconds."
                                : "The connection stopped: no frame for 20 seconds.";
        } else {
            RollbackLagStats lag;
            rmgk_gekko::lag_stats(lag);
            if (lag.valid && !lag.peers.empty()) {
                bool any_connected = false;
                for (const RollbackLagPeer& peer : lag.peers)
                    any_connected = any_connected || peer.connected;
                if (any_connected) {
                    seen_connected = true;
                    lost_count = 0;
                } else if (seen_connected && ++lost_count > (lobby ? 50 : 25)) {
                    /* GekkoNet gives up on a peer after 5 s of silence (30 s in a lobby match); its inputs
                     * would be zeros from then on */
                    end = ZNX_P2P_END_TIMEOUT;
                    text = lobby ? "The connection to the other players was lost."
                                 : "The connection to the other player was lost.";
                }
            }
        }
        if (end != ZNX_P2P_END_NONE) {
            int expected = ZNX_P2P_END_NONE;
            if (g_end.compare_exchange_strong(expected, end)) {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                g_end_text = text;
            }
            note("%s: %s", lobby ? "lobby" : "p2p", text.c_str());
            rmgk_gekko::request_stop();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(lobby ? 10 : 20));
    }
}

} // namespace

/* ---- what n02 and rmgk_gekko call back into (net/n02_glue.cpp, net/rmgk_glue.cpp) ---- */

void znx_net_chat(const char* nick, const char* message)
{
    int a, b;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    std::string upper = message;
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (upper.rfind(kModePrefix, 0) == 0) {
        if (!g_cfg.host) {
            g_got_mode = true;
            g_mode_rollback = upper.substr(std::strlen(kModePrefix)) == "RB";
        }
    } else if (parse_int_after(upper.c_str(), kDelayPrefix, &a, &b)) {
        if (!g_cfg.host) {
            g_host_delay_mode = a >= 0 && a <= 3 ? a : kDelayModeDefault;
            g_host_delay = b >= 0 ? std::clamp(b, 0, 9) : kDefaultDelay;
        }
    } else if (parse_int_after(upper.c_str(), kPredictionPrefix, &a, nullptr)) {
        if (!g_cfg.host)
            g_host_prediction = a >= 0 && a <= 10 ? a : 0;
    } else if (upper.rfind("RMGK:", 0) == 0) {
        /* RBPACE: the pacing model, ignored by RMG-K's session now */
    } else if (lower_ascii(message).find("emulator/version difference alert") != std::string::npos) {
        g_app_mismatch = true;
    } else if (std::strcmp(nick, g_cfg.username.c_str()) != 0 && std::strncmp(message, "both players are ready", 22) != 0) {
        g_chat = message;
        note("p2p: chat <%s> %s", nick, message);
    }
}

void znx_net_game_started(const char* game, int player)
{
    int expected = 0;
    if (g_game_player.compare_exchange_strong(expected, player)) {
        note("p2p: the match starts: \"%s\", player %d", game, player);
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_phase = ZNX_P2P_STARTING;
        g_message.clear();
    }
}

void znx_net_game_ended(void)
{
    if (!g_local_drop.load() && g_game_player.load() != 0) {
        g_peer_dropped = true;
        note("p2p: the other player ended the match (n02 DROP)");
    }
}

void znx_net_peer_joined(void)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_peer_connected = true;
    g_peer_ready = false;
    g_ping = -1;
    g_ping_samples.clear();
    g_delay_last_update = 0;
    g_auto_delay = kDefaultDelay;
    g_last_sent_delay = g_last_sent_mode = -1;
    g_phase = ZNX_P2P_LOBBY;
    g_message.clear();
    g_join_active = false;
    if (g_cfg.host) {
        g_announce = true;            /* onPeerJoined: sendGameLayer + the three settings */
        g_suspended = true;           /* no live host session or listing while a peer is here */
        g_punch_peer_ip.clear();
        if (!g_live_token.empty()) {
            trav_send(std::string(kTraversal) + "|HOSTCLOSE|" + g_live_token);
            g_live_token.clear();
        }
        unenlist();
    }
    note("p2p: peer connected");
}

void znx_net_peer_left(void)
{
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        name = g_peer_name;
        g_peer_connected = false;
        g_peer_ready = false;
        g_local_ready = false;
        g_ping = -1;
        g_peer_name.clear();
        g_peer_app.clear();
        g_chat.clear();
        g_app_mismatch = false;
        const std::string who = name.empty() ? std::string(g_cfg.host ? "The other player" : "The host") : name;
        const std::string what = g_ping_timeout ? who + " stopped answering for 10 seconds." : who + " left.";
        g_ping_timeout = false;
        if (g_cfg.host && g_game_player.load() == 0) {
            /* onPeerLeft: back to waiting, the code opened again, listed again */
            g_phase = ZNX_P2P_WAITING;
            g_message = what + " Waiting for another player.";
            g_suspended = false;
            g_next_reg = 0;
            g_next_keep = 0;
            g_reg_attempts = 0;
            if (g_cfg.enlist)
                enlist();
        } else if (g_game_player.load() == 0) {
            g_phase = ZNX_P2P_FAILED;
            g_message = what + " Join again to play another match.";
        }
    }
    if (g_game_player.load() != 0)
        g_peer_left = true;
    note("p2p: peer left");
}

void znx_net_peer_info(const char* name, const char* app)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_peer_name = name;
    if (g_cfg.host) {
        g_peer_app = app;
        g_app_mismatch = g_peer_app != g_cfg.app;
    }
    note("p2p: peer \"%s\" (%s)", name, app);
}

void znx_net_hosted_game(const char* game)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_host_game = game;
}

void znx_net_ping(int ping)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_ping = ping;
    if (ping >= 0 && g_cfg.host && g_game_player.load() == 0)
        g_ping_samples.push_back({ now_ms(), ping });
}

void znx_net_ssrv(const char* data, int len, const void* from)
{
    handle_ssrv(data, len, (const sockaddr_in*)from);
}

void znx_net_ready_line(const char* text);

void znx_net_debug(const char* message)
{
    znx_net_ready_line(message);
    if (std::strncmp(message, "Ping timeout", 12) == 0) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_ping_timeout = true;
    }
    const std::string lower = lower_ascii(message);
    if (lower.find("error") != std::string::npos || lower.find("timeout") != std::string::npos ||
        lower.find("drop") != std::string::npos || lower.find("alert") != std::string::npos ||
        lower.find("connection") != std::string::npos || lower.find("peer") != std::string::npos ||
        lower.find("ready") != std::string::npos)
        note("n02: %s", message);
}

void znx_net_state_saved(int frame)
{
    if (g_hooks != nullptr && g_hooks->state_saved != nullptr)
        g_hooks->state_saved(frame, g_hooks->user);
}

void znx_net_state_loaded(int frame)
{
    g_rollbacks++;
    g_depth = 0;
    if (g_hooks != nullptr && g_hooks->state_loaded != nullptr)
        g_hooks->state_loaded(frame, g_hooks->user);
}

void znx_net_frame_resimulated(void)
{
    if (++g_depth > g_deepest.load())
        g_deepest = g_depth;
    if (g_hooks != nullptr && g_hooks->frame_resimulated != nullptr)
        g_hooks->frame_resimulated(g_hooks->user);
}

void znx_net_end_frame(int confirmed)
{
    g_depth = 0;
    if (g_hooks != nullptr && g_hooks->end_frame != nullptr)
        g_hooks->end_frame(confirmed, g_hooks->user);
}

void znx_net_timesync_scale(double scale)
{
    g_speed = scale;
    if (g_hooks != nullptr && g_hooks->timesync_scale != nullptr)
        g_hooks->timesync_scale(scale, g_hooks->user);
}

int znx_net_visible_frames(void) { return g_visible.load(); }
const char* znx_net_log_dir(void) { return g_hooks != nullptr && g_hooks->log_dir != nullptr ? g_hooks->log_dir : "."; }
bool znx_net_verbose_log(void) { return g_hooks != nullptr && g_hooks->verbose_log; }

/* ---- the C API ---- */

extern "C" int znx_p2p_open(const struct znx_p2p_config* config, char* error, size_t error_size)
{
    auto fail_open = [&](const std::string& text) {
        std::snprintf(error, error_size, "%s", text.c_str());
        return 1;
    };
    if (g_open.load())
        znx_p2p_close();
    Config cfg;
    cfg.host = config->host != 0;
    cfg.port = config->port > 0 && config->port <= 65535 ? config->port : ZNX_P2P_DEFAULT_PORT;
    cfg.game = config->game != nullptr ? config->game : "";
    cfg.username = config->username != nullptr && config->username[0] ? config->username : "Switch";
    if (cfg.username.size() > 31)
        cfg.username.resize(31);
    if (config->app != nullptr)
        cfg.app = config->app;
    cfg.target = config->join_target != nullptr ? config->join_target : "";
    cfg.delay = std::clamp(config->delay, 0, 9);
    cfg.prediction = std::clamp(config->prediction, 0, 10);
    cfg.use_codes = config->use_codes != 0;
    cfg.enlist = config->enlist != 0;
    if (config->nat_host != nullptr && config->nat_host[0]) cfg.nat_host = config->nat_host;
    if (config->nat_port > 0) cfg.nat_port = config->nat_port;
    if (config->list_host != nullptr && config->list_host[0]) cfg.list_host = config->list_host;
    if (config->list_port > 0) cfg.list_port = config->list_port;
    cfg.save_identity = config->save_identity;
    cfg.log = config->log;
    cfg.user = config->user;

    /* parse the join target before opening anything */
    bool by_code = false;
    std::string join_ip, join_code;
    int join_port = ZNX_P2P_DEFAULT_PORT;
    if (!cfg.host) {
        char code[32];
        if (znx_p2p_normalize_code(cfg.target.c_str(), code, sizeof(code))) {
            by_code = true;
            join_code = code;
        } else {
            std::string target = cfg.target;
            target.erase(0, target.find_first_not_of(" \t"));
            target.erase(target.find_last_not_of(" \t") + 1);
            const size_t colon = target.rfind(':');
            if (colon != std::string::npos) {
                join_ip = target.substr(0, colon);
                join_port = std::atoi(target.c_str() + colon + 1);
                if (join_port <= 0 || join_port > 65535)
                    join_port = ZNX_P2P_DEFAULT_PORT;
            } else {
                join_ip = target;
            }
            if (join_ip.empty())
                return fail_open("There is no address to join.");
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_cfg = cfg;
        g_phase = cfg.host ? ZNX_P2P_WAITING : ZNX_P2P_CONNECTING;
        g_message.clear();
        g_peer_name.clear(); g_peer_app.clear(); g_host_game.clear(); g_chat.clear();
        g_peer_connected = g_peer_ready = g_local_ready = false;
        g_got_mode = g_mode_rollback = g_app_mismatch = false;
        g_ping = -1;
        g_host_delay = -1; g_host_delay_mode = kDelayModeDefault; g_host_prediction = 0;
        g_announce = false;
        g_last_sent_delay = g_last_sent_mode = -1;
        g_ping_samples.clear();
        g_delay_last_update = 0;
        g_auto_delay = kDefaultDelay;
        g_code = config->code != nullptr ? config->code : "";
        g_token = config->token != nullptr ? config->token : "";
        if (g_code.empty() || g_token.empty()) {
            g_code.clear();
            g_token.clear();
        }
        g_live_token.clear();
        g_reg_attempts = 0;
        g_next_reg = g_next_keep = 0;
        g_suspended = false;
        g_code_state = cfg.use_codes ? 0 : 2;
        g_public_address.clear();
        g_whatismyip_pending = false;
        g_punch_peer_ip.clear();
        g_punch_peer_port = 0;
        g_punch_deadline = g_next_punch = 0;
        g_enlisted = false;
        g_timer_step = 0;
        g_join_by_code = by_code;
        g_join_active = !cfg.host;
        g_got_host = g_fallback_tried = false;
        g_join_code = join_code;
        g_join_ip = join_ip;
        g_join_port = join_port;
        g_join_token.clear();
        g_fallback.clear();
        g_punch_attempts = 0;
        g_next_join = 0;
        g_next_connect = 0;
        g_join_deadline = now_ms() + (by_code ? 15000 : kDirectJoinTimeoutMs);
    }
    g_game_player = 0;
    g_in_game = false;
    g_peer_dropped = g_peer_left = g_local_drop = false;
    g_end = ZNX_P2P_END_NONE;

    std::vector<char> app(cfg.app.begin(), cfg.app.end()), game(cfg.game.begin(), cfg.game.end()),
        name(cfg.username.begin(), cfg.username.end());
    app.push_back(0); game.push_back(0); name.push_back(0);
    /* KailleraNetplayDialog.cpp:4319 (host, on the Kaillera port) and :4483 (join, port 0) */
    if (!p2p_core_initialize(cfg.host, cfg.host ? cfg.port : 0, app.data(), cfg.host ? game.data() : (char*)"", name.data())) {
        p2p_core_cleanup();
        return fail_open(cfg.host ? "UDP port " + std::to_string(cfg.port) + " cannot be opened. Is another program using it?"
                                  : std::string("The network cannot be used: no UDP socket."));
    }
    g_port = p2p_core_get_port();
    note("p2p: n02 socket on port %d, %s \"%s\" as \"%s\" (%s)", p2p_core_get_port(), cfg.host ? "hosting" : "joining",
        cfg.host ? cfg.game.c_str() : cfg.target.c_str(), cfg.username.c_str(), cfg.app.c_str());
    if (cfg.host && cfg.use_codes) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        /* the dialog's constructor: HOSTOPEN with a saved code, else CLAIM AUTO */
        if (!g_token.empty())
            trav_send(std::string(kTraversal) + "|HOSTOPEN|" + g_token);
        else {
            trav_send(std::string(kTraversal) + "|CLAIM|AUTO");
            g_reg_attempts++;
        }
        g_next_reg = now_ms() + 2000;
    } else if (cfg.host && cfg.enlist) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_whatismyip_pending = true;
        ssrv_send("WHATISMYIP");
        enlist();
    }
    g_open = true;
    g_running = true;
    g_thread = std::thread(lobby_thread);
    return 0;
}

extern "C" void znx_p2p_close(void)
{
    if (!g_open.load())
        return;
    g_running = false;
    if (g_thread.joinable())
        g_thread.join();
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        /* cleanupSessionForClose */
        unenlist();
        if (g_cfg.host && !g_live_token.empty())
            trav_send(std::string(kTraversal) + "|HOSTCLOSE|" + g_live_token);
        g_live_token.clear();
        g_phase = ZNX_P2P_CLOSED;
    }
    if (g_game_player.load() != 0 && !g_peer_left.load()) {
        g_local_drop = true;
        p2p_drop_game();
        g_local_drop = false;
    }
    p2p_disconnect();
    p2p_core_cleanup();
    g_game_player = 0;
    g_open = false;
    note("p2p: closed");
}

extern "C" int znx_p2p_is_open(void) { return g_open.load() ? 1 : 0; }

extern "C" void znx_p2p_status(struct znx_p2p_status* s)
{
    std::memset(s, 0, sizeof(*s));
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    s->phase = g_phase;
    if (g_phase == ZNX_P2P_STARTING && g_in_game.load())
        s->phase = ZNX_P2P_IN_GAME;
    s->host = g_cfg.host;
    s->port = g_open.load() ? g_port : 0;
    std::snprintf(s->code, sizeof(s->code), "%s", g_code_state == 1 ? g_code.c_str() : "");
    s->code_state = g_code_state;
    std::snprintf(s->public_address, sizeof(s->public_address), "%s", g_public_address.c_str());
    std::snprintf(s->peer_name, sizeof(s->peer_name), "%s", g_peer_name.c_str());
    std::snprintf(s->peer_app, sizeof(s->peer_app), "%s", g_peer_app.c_str());
    std::snprintf(s->host_game, sizeof(s->host_game), "%s", g_host_game.c_str());
    s->game_mismatch = !g_cfg.host && !g_host_game.empty() && g_host_game != g_cfg.game;
    s->not_rollback = !g_cfg.host && g_got_mode && !g_mode_rollback;
    s->app_mismatch = g_app_mismatch;
    s->ping = g_ping;
    s->delay = effective_delay_locked();
    s->delay_auto = g_cfg.host ? g_cfg.delay == 0 : g_host_delay_mode == kDelayModeDefault;
    s->prediction = effective_prediction_locked();
    s->local_ready = g_local_ready;
    s->peer_ready = g_peer_ready;
    s->can_ready = g_peer_connected && (g_cfg.host || (!g_host_game.empty() && g_got_mode && g_mode_rollback &&
                                                        g_host_game == g_cfg.game));
    s->enlisted = g_enlisted;
    s->player = g_game_player.load();
    std::snprintf(s->message, sizeof(s->message), "%s", g_message.c_str());
}

extern "C" void znx_p2p_set_ready(int ready)
{
    bool ok;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        ok = g_peer_connected && g_game_player.load() == 0 &&
             (g_cfg.host || (!ready) || (!g_host_game.empty() && g_got_mode && g_mode_rollback && g_host_game == g_cfg.game));
        if (ok) {
            g_local_ready = ready != 0;
            g_req_ready = ready != 0;
        }
    }
}

extern "C" void znx_p2p_set_delay(int delay)
{
    bool changed;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_cfg.host)
            return;
        changed = g_cfg.delay != std::clamp(delay, 0, 9);
        g_cfg.delay = std::clamp(delay, 0, 9);
        if (changed)
            g_req_delay = true;
    }
}

/* n02_glue.cpp's PREADY reports arrive as debug lines; the peer's ready state is read from them as the
 * dialog does (onDebug: "<name> is ready" / "is not ready"). */
void znx_net_ready_line(const char* text)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const std::string line = text;
    auto ends = [&](const char* suffix) {
        const size_t n = std::strlen(suffix);
        return line.size() >= n && line.compare(line.size() - n, n, suffix) == 0;
    };
    if (line == "You are marked as ready" || line == "You are marked as not ready")
        return;
    if (ends(" is not ready"))
        g_peer_ready = false;
    else if (ends(" is ready"))
        g_peer_ready = true;
}

namespace
{
/* What both kinds of match share: the counters, the hooks, rmgk_gekko::execute() with the control
 * thread beside it, and the summary. start() starts GekkoNet's session (start_p2p_session or
 * start_lobby_session) and returns false with the reason in *why. */
template <typename Start>
int run_rmgk_session(void* core, const struct znx_p2p_session_hooks* hooks, int delay, Start start,
                     RollbackLagStats& lag)
{
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_end_text.clear();
    }
    g_end = ZNX_P2P_END_NONE;
    g_peer_dropped = false;
    g_visible = 0;
    g_rollbacks = 0;
    g_deepest = 0;
    g_live_ping = -1;
    g_live_delay = delay;
    g_live_ahead = 0;
    g_speed = 1.0;
    if (hooks != nullptr) {
        g_hooks_copy = *hooks;
        g_hooks = &g_hooks_copy;
    } else {
        g_hooks = nullptr;
    }
    auto error_end = [&](const std::string& text) {
        note("%s: %s", g_lobby_mode.load() ? "lobby" : "p2p", text.c_str());
        g_end = ZNX_P2P_END_ERROR;
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_end_text = text;
        return 1;
    };
    if (!m64p::Core.Hook(core))
        return error_end("cannot hook the core: " + m64p::Core.GetLastError());
    std::string why;
    if (!start(why)) {
        m64p::Core.Unhook();
        return error_end(why);
    }
    rmgk_gekko::set_debug_hooks(nullptr, begin_visible_hook, end_visible_hook, nullptr);

    g_in_game = true;
    std::atomic<bool> control_running{true};
    std::thread control([&] { session_control_loop(control_running); });
    const bool ok = rmgk_gekko::execute();
    control_running = false;
    control.join();
    g_in_game = false;

    rmgk_gekko::lag_stats(lag);
    rmgk_gekko::set_debug_hooks(nullptr, nullptr, nullptr, nullptr);
    rmgk_gekko::close_session();
    m64p::Core.Unhook();
    note("%s: session over after %d frames: rollbacks %d (%d frames re-run, deepest %d), waited for input %.1f ms, "
         "ping %d ms", g_lobby_mode.load() ? "lobby" : "p2p", g_visible.load(), lag.match.rollbacks,
         lag.match.rollbackFrames, lag.match.maxRollbackFrames, lag.match.waitUs / 1000.0,
         lag.peers.empty() ? -1 : lag.peers[0].pingMs);
    return ok ? 0 : 2;
}
} // namespace

extern "C" int znx_p2p_run_session(void* core, const struct znx_p2p_session_hooks* hooks)
{
    const int player = g_game_player.load();
    int delay, prediction;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        delay = effective_delay_locked();
        prediction = effective_prediction_locked();
    }
    g_lobby_mode = false;
    RollbackLagStats lag;
    const int result = run_rmgk_session(core, hooks, delay, [&](std::string& why) {
        if (player == 0) {
            why = "the match was not started in the lobby";
            return false;
        }
        char peer_ip[128] = {};
        int peer_port = 0;
        if (!p2p_core_get_peer_endpoint(peer_ip, sizeof(peer_ip), &peer_port) || p2p_core_get_port() <= 0) {
            why = "the other player's address is unknown";
            return false;
        }
        /* CoreStartEmulation, the GEKKO| branch (Emulation.cpp:1135-1170) */
        if (!rmgk_gekko::set_deterministic(true)) {
            why = "set_deterministic failed";
            return false;
        }
        if (!rmgk_gekko::start_p2p_session("rmgk-gekko", 2, (int)sizeof(uint32_t), player,
                (unsigned short)p2p_core_get_port(), peer_ip, (unsigned short)peer_port, delay, prediction)) {
            why = "GekkoNet's session did not start";
            return false;
        }
        note("p2p: session: player %d, delay %d, prediction window %d, peer %s:%d, local port %d", player, delay,
            prediction, peer_ip, peer_port, p2p_core_get_port());
        return true;
    }, lag);
    if (result == 1) {
        g_game_player = 0;
        g_hooks = nullptr;
        return 1;
    }

    int end = g_end.load();
    if (end == ZNX_P2P_END_NONE) {
        /* stopped from outside without znx_p2p_leave_game (the program is closing) */
        end = g_peer_dropped.load() ? ZNX_P2P_END_PEER_DROP : ZNX_P2P_END_LOCAL;
        g_end = end;
    }
    /* back to the lobby (the dialog's onGameEnded), unless the peer is gone */
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_local_ready = false;
        g_peer_ready = false;
        if (end == ZNX_P2P_END_PEER_LEFT || end == ZNX_P2P_END_TIMEOUT) {
            g_phase = ZNX_P2P_FAILED;
            g_message = g_end_text;
        } else {
            g_phase = g_peer_connected ? ZNX_P2P_LOBBY : ZNX_P2P_FAILED;
            g_message = g_end_text;
        }
    }
    if (end == ZNX_P2P_END_TIMEOUT && !g_peer_dropped.load()) {
        /* tell the peer, in case it still hears us */
        g_local_drop = true;
        p2p_drop_game();
        g_local_drop = false;
    }
    g_game_player = 0;
    g_hooks = nullptr;
    return result == 0 || end != ZNX_P2P_END_ERROR ? 0 : 1;
}

extern "C" int znx_p2p_lobby_total_seats(const struct znx_p2p_lobby_match* m)
{
    /* Emulation.cpp:1188-1202: the highest occupied seat, or the seats a late-join match emulates */
    int total = std::max(m->local_slot, m->seats);
    for (int i = 0; i < m->num_remotes && i < 3; ++i)
        total = std::max(total, m->remote_slot[i]);
    return total;
}

extern "C" int znx_p2p_lobby_address(const struct znx_p2p_lobby_match* m, char* out, size_t out_size)
{
    /* EmulationThread::SetLobbyNetplay with RollbackLobbyDialog's "<slot>,ice,<userId>" peers and the
     * "seats=N" option (RollbackLobbyDialog.cpp:5920-5943) */
    std::string a = "LOBBY|" + std::to_string(m->delay) + "|" + std::to_string(m->prediction);
    for (int i = 0; i < m->num_remotes && i < 3; ++i)
        a += "|" + std::to_string(m->remote_slot[i]) + ",ice," + std::to_string((unsigned long long)m->remote_user[i]);
    if (m->seats > 0)
        a += "|seats=" + std::to_string(m->seats);
    std::snprintf(out, out_size, "%s", a.c_str());
    return (int)a.size();
}

extern "C" int znx_p2p_run_lobby_session(void* core, const struct znx_p2p_session_hooks* hooks,
                                         const struct znx_p2p_lobby_match* match)
{
    g_lobby = *match;
    g_lobby_mode = true;
    g_lobby_finish = false;
    g_lobby_server = true;
    g_lobby_left = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_lobby_left_name.clear();
    }
    char address[256];
    znx_p2p_lobby_address(match, address, sizeof(address));
    const int players = znx_p2p_lobby_total_seats(match);
    RollbackLagStats lag;
    const int result = run_rmgk_session(core, hooks, match->delay, [&](std::string& why) {
        /* CoreStartEmulation, the LOBBY| branch (Emulation.cpp:1168-1215) */
        std::vector<LobbyRemotePeer> remotes;
        for (int i = 0; i < match->num_remotes && i < 3; ++i) {
            LobbyRemotePeer peer;
            peer.slot = match->remote_slot[i];
            peer.userId = match->remote_user[i];
            remotes.push_back(peer);
        }
        if (match->delay < 0 || match->prediction < 1 || (remotes.empty() && match->seats == 0)) {
            why = "invalid lobby session parameters";
            return false;
        }
        if (!rmgk_gekko::set_deterministic(true)) {
            why = "set_deterministic failed";
            return false;
        }
        note("lobby: session %s: slot %d of %d seats", address, match->local_slot, players);
        if (!rmgk_gekko::start_lobby_session("rmgk-gekko", players, (int)sizeof(uint32_t), match->local_slot, 0,
                remotes.data(), (int)remotes.size(), match->delay, match->prediction)) {
            why = "GekkoNet's session did not start (a player's ICE path closed before the game started?)";
            return false;
        }
        return true;
    }, lag);
    int end = g_end.load();
    if (result != 1 && end == ZNX_P2P_END_NONE) {
        end = ZNX_P2P_END_LOCAL;   /* stopped from outside (the program is closing) */
        g_end = end;
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_end_text.empty())
            g_end_text = "You left the match.";
    }
    g_hooks = nullptr;
    return result == 1 ? 1 : 0;
}

extern "C" void znx_p2p_lobby_peer_left(int slot, const char* name)
{
    if (!g_lobby_mode.load() || slot < 1 || slot > 4)
        return;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_lobby_left_name = name != nullptr ? name : "";
    }
    g_lobby_left.fetch_or(1u << slot);
    g_peer_dropped = true;
    note("lobby: the player in seat %d (%s) left the match", slot, name != nullptr ? name : "?");
    if (g_hooks != nullptr && g_hooks->stop_on_peer_drop)
        rmgk_gekko::request_disconnect_player(slot);   /* RollbackLobbyDialog::onMatchPeerLeft */
}

extern "C" void znx_p2p_lobby_server_up(int up)
{
    const bool was = g_lobby_server.exchange(up != 0);
    if (was != (up != 0))
        note("lobby: %s", up ? "the lobby server is back" : "lost the lobby server; the match plays on");
}

extern "C" int znx_p2p_lobby_finish_requested(void) { return g_lobby_finish.load() ? 1 : 0; }
extern "C" int znx_p2p_lobby_mode(void) { return g_lobby_mode.load() ? 1 : 0; }

extern "C" void znx_p2p_leave_game(void)
{
    int expected = ZNX_P2P_END_NONE;
    if (g_end.compare_exchange_strong(expected, ZNX_P2P_END_LOCAL)) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            g_end_text = "You left the match.";
        }
        if (g_lobby_mode.load()) {
            /* RollbackLobbyDialog::notifyEmulationFinished -> MATCH_FINISHED, sent by the control callback */
            g_lobby_finish = true;
        } else if (!g_peer_left.load() && !g_peer_dropped.load()) {
            /* KailleraP2PDialog::onDrop -> p2p_drop_game: n02 DROP */
            g_local_drop = true;
            p2p_drop_game();
            g_local_drop = false;
        }
        note(g_lobby_mode.load() ? "lobby: leaving the match (MATCH_FINISHED)" : "p2p: leaving the match (n02 DROP)");
    }
    rmgk_gekko::request_stop();
}

extern "C" void znx_p2p_send_drop(void)
{
    int expected = ZNX_P2P_END_NONE;
    g_end.compare_exchange_strong(expected, ZNX_P2P_END_LOCAL);
    if (g_lobby_mode.load()) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (g_end_text.empty())
                g_end_text = "The match is over.";
        }
        g_lobby_finish = true;
        return;
    }
    g_local_drop = true;
    p2p_drop_game();
    g_local_drop = false;
}

extern "C" void znx_p2p_stop_game(void)
{
    rmgk_gekko::request_stop();
}

extern "C" int znx_p2p_last_end(char* text, size_t text_size)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (text != nullptr && text_size > 0)
        std::snprintf(text, text_size, "%s", g_end_text.c_str());
    return g_end.load();
}

extern "C" int znx_p2p_peer_dropped(void) { return g_peer_dropped.load() ? 1 : 0; }

extern "C" void znx_p2p_live(struct znx_p2p_live* live)
{
    live->ping = g_live_ping.load();
    live->delay = g_live_delay.load();
    live->rollbacks = g_rollbacks.load();
    live->deepest = g_deepest.load();
    live->frames = g_visible.load();
    live->frames_ahead = g_live_ahead.load();
    live->speed = g_speed.load();
}

extern "C" int znx_p2p_visible_frames(void) { return g_visible.load(); }

/* ---- helpers ---- */

extern "C" int znx_p2p_normalize_code(const char* input, char* out, size_t out_size)
{
    /* normalizeTraversalCode (KailleraP2PDialog.cpp:861-899) */
    std::string s;
    for (const char* p = input != nullptr ? input : ""; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        s.push_back(c);
    }
    if (s.empty())
        return 0;
    for (char c : s)
        if (c == '.' || c == ':' || c == '/')
            return 0;
    if (s.size() < 4)
        return 0;
    size_t prefix = 0;
    while (prefix < s.size() && prefix < 4 && s[prefix] >= 'A' && s[prefix] <= 'Z')
        prefix++;
    if (prefix < 3)
        return 0;
    if (prefix < s.size() && s[prefix] >= 'A' && s[prefix] <= 'Z')
        return 0;
    std::string digits = s.substr(prefix);
    if (!digits.empty() && (digits[0] == '@' || digits[0] == '#' || digits[0] == '-' || digits[0] == '_'))
        digits.erase(0, 1);
    if (digits.empty() || digits.size() > 3)
        return 0;
    for (char c : digits)
        if (c < '0' || c > '9')
            return 0;
    std::snprintf(out, out_size, "%s@%d", s.substr(0, prefix).c_str(), std::atoi(digits.c_str()));
    return 1;
}

extern "C" int znx_p2p_auto_delay(int ping)
{
    /* automaticRollbackFrameDelayForPing (KailleraP2PDialog.cpp:287-310) */
    if (ping < 0) return kDefaultDelay;
    if (ping <= 50) return 1;
    if (ping <= 100) return 2;
    if (ping <= 150) return 3;
    if (ping <= 220) return 4;
    return 5;
}

extern "C" int znx_p2p_parse_list(const char* body, struct znx_p2p_listing* out, int max)
{
    /* KailleraWaitingGamesDialog::parseResponse and KailleraNetplayDialog's waiting-games parser:
     * after the HTTP header, lines of "game|app {CC:code}|user|ip:port|..." or one flat pipe stream,
     * four fields per game; only games with a connect code are kept. */
    if (body == nullptr)
        return 0;
    std::string text = body;
    const size_t header = text.find("\r\n\r\n");
    if (header != std::string::npos)
        text = text.substr(header + 4);
    int count = 0;
    size_t line_start = 0;
    while (line_start <= text.size() && count < max) {
        size_t line_end = text.find('\n', line_start);
        if (line_end == std::string::npos)
            line_end = text.size();
        std::string line = text.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        std::vector<std::string> fields;
        size_t s = 0;
        for (;;) {
            const size_t bar = line.find('|', s);
            std::string field = line.substr(s, bar == std::string::npos ? std::string::npos : bar - s);
            field.erase(0, field.find_first_not_of(" \t\r"));
            const size_t last = field.find_last_not_of(" \t\r");
            field.erase(last == std::string::npos ? 0 : last + 1);
            fields.push_back(field);
            if (bar == std::string::npos)
                break;
            s = bar + 1;
        }
        for (size_t i = 0; i + 3 < fields.size() && count < max; i += 4) {
            std::string app = fields[i + 1], code;
            const size_t open = app.find(" {CC:");
            if (open != std::string::npos) {
                const size_t close = app.find('}', open + 5);
                if (close != std::string::npos) {
                    code = app.substr(open + 5, close - open - 5);
                    app.resize(open);
                }
            }
            if (code.empty())
                continue;
            znx_p2p_listing& l = out[count++];
            std::snprintf(l.game, sizeof(l.game), "%s", fields[i].c_str());
            std::snprintf(l.app, sizeof(l.app), "%s", app.c_str());
            std::snprintf(l.user, sizeof(l.user), "%s", fields[i + 2].c_str());
            std::snprintf(l.address, sizeof(l.address), "%s", fields[i + 3].c_str());
            std::snprintf(l.code, sizeof(l.code), "%s", code.c_str());
        }
        if (line_end >= text.size())
            break;
    }
    return count;
}
