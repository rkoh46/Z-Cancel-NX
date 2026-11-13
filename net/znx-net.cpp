// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-net: a headless RMG-K P2P rollback session between two copies of this program.
 *
 *   znx-net --rom ROM --host PORT          --inputs FILE --player 1
 *   znx-net --rom ROM --join 127.0.0.1:PORT --inputs FILE --player 2
 *
 * Everything that is on the wire is RMG-K's own code, compiled unchanged from ref/RMG-K (see
 * docs/PLAN.md, "RMG-K P2P rollback, in detail"):
 *   - the n02 P2P client (n02/core/p2p_core.cpp and friends): the lobby handshake (LOGN, PING,
 *     CHAT with RMG-K's RMGK:MODE/RBDELAY/RBPRED/RBPACE messages, PREADY, LOAD) and, during the
 *     game, the tunnel that carries GekkoNet's datagrams on the same UDP socket;
 *   - RMG-K's session driver (RMG-Core/rmgk_gekko.cpp: start_p2p_session, execute, the frame
 *     callbacks, input latching, deferred saves, time sync and the presentation pacer) and its
 *     core wrappers (RMG-Core/RollbackNetcode.cpp);
 *   - GekkoNet (GekkoLib), built without asio.
 * This file plays the part of RMG-K's Qt dialog (KailleraP2PDialog: host or join, announce or
 * take the rollback settings, ready, start), of CoreStartEmulation (bench/bench_core.c sets the
 * core up the way a session does) and of the controller (an input plugin fed from a script).
 *
 * Output (stdout):
 *   check <frame> <whole> <rest>   RDRAM checksums after <frame> frames (the bench's "check" line),
 *                                  every --check-every frames and after the last one; printed
 *                                  only once that frame is final (no later input can roll it back)
 *   summary lines at the end: frames, wall time, frames per second, rollbacks
 * Diagnostics go to stderr. */

#include <arpa/inet.h>
#include <dlfcn.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "bench_core.h"
#include "md5.h"
}
#include "m64p_debugger.h"
#include "m64p_frontend.h"

#include "core/p2p_core.h"
#include "m64p/Api.hpp"
#include "rmgk_gekko.hpp"

#include "znx_net.h"

namespace
{
using Clock = std::chrono::steady_clock;

/* ---- options ---- */

struct Options {
    const char* rom = nullptr;
    bool host = false;
    int port = 0;                 /* host: port to listen on; joiner: the host's port */
    std::string join_ip;
    const char* inputs = nullptr;
    int player = 0;               /* which player's lines of the script this side plays */
    int delay = 2;
    int prediction = 7;
    int frames = 17000;
    int check_every = 60;
    long check_skip_lo = 0, check_skip_hi = 0;
    int sim_latency = 0, sim_jitter = 0;
    double sim_loss = 0;
    unsigned sim_seed = 0;
    bool pace = true;
    bool verbose = false;
    bool gekko_log = false;
    const char* name = nullptr;
    std::string app = "RMG-K v0.9.16";   /* RMG-K's "RMG-K " + CORE_VERSION (git tag) */
    int lobby_timeout_s = 60;
};

Options g_opt;

/* ---- the input script ---- */

std::vector<std::pair<int, uint32_t>> g_script;   /* this side's player only, sorted by frame */

bool load_script(const char* path, int player)
{
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        std::fprintf(stderr, "znx-net: cannot open %s\n", path);
        return false;
    }
    char line[256];
    int lineno = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        lineno++;
        if (char* hash = std::strchr(line, '#'))
            *hash = 0;
        if (line[std::strspn(line, " \t\r\n")] == 0)
            continue;
        int frame, p;
        unsigned value;
        if (std::sscanf(line, "%d %d %x", &frame, &p, &value) != 3 || frame < 0 || p < 1 || p > 4) {
            std::fprintf(stderr, "znx-net: %s:%d: expected \"frame player buttons_hex\"\n", path, lineno);
            std::fclose(f);
            return false;
        }
        if (p == player)
            g_script.emplace_back(frame, value);
    }
    std::fclose(f);
    /* same order as the bench: by frame, lines of one frame in file order (the last one wins) */
    std::stable_sort(g_script.begin(), g_script.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
}

uint32_t script_value(int frame)
{
    auto it = std::upper_bound(g_script.begin(), g_script.end(), frame,
        [](int f, const auto& e) { return f < e.first; });
    return it == g_script.begin() ? 0 : std::prev(it)->second;
}

/* ---- session state ---- */

std::mutex g_lobby_mutex;
bool g_peer_joined = false, g_hosted_game_known = false, g_got_mode = false, g_mode_rollback = false;
bool g_got_delay = false, g_got_prediction = false;
int g_host_delay = -1, g_host_prediction = 0;
std::string g_hosted_game, g_peer_name, g_peer_app;
std::atomic<int> g_game_player{0};       /* set by p2p_game_callback: 1 host, 2 joiner */
std::atomic<bool> g_local_drop{false};   /* our own p2p_drop_game is calling back */
std::atomic<bool> g_peer_dropped{false};

int g_session_delay = 2, g_session_prediction = 7, g_local_player = 1;
std::string g_goodname;
std::string g_log_dir;

void* g_core = nullptr;
const uint8_t* g_rdram = nullptr;
bool g_first_frame = true;

/* frames */
std::atomic<int> g_visible{0};           /* visible frames completed */
int g_max_saved = -1;                    /* newest GekkoNet frame whose post-frame state was saved */
int g_final = -1;                        /* newest frame known final */
std::map<int, std::pair<uint64_t, uint64_t>> g_pending_checks;   /* frame -> checksums, not yet final */
int g_checks_printed = 0;
std::atomic<bool> g_done{false};         /* every frame below --frames is final */
Clock::time_point g_done_time;
std::atomic<bool> g_stop_failed{false};

/* rollback statistics, from the wrapped core calls */
int g_rollbacks = 0, g_resim_frames = 0, g_depth = 0, g_max_depth = 0, g_depth_hist[16];

Clock::time_point g_session_begin;

void note(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fprintf(stderr, "znx-net[%s]: %s\n", g_opt.host ? "host" : "join", line);
}

/* ---- the controller ---- */

uint32_t input_source(int control)
{
    if (control != 0)
        return 0;
    /* GekkoNet files the input added during frame F under F + delay (input.cpp AddLocalInput);
     * rmgk_gekko samples it right after storing F in current_frame(). The script says what the
     * game should see on each frame, so look delay frames ahead. */
    const int frame = rmgk_gekko::current_frame();
    return frame < 0 ? 0 : script_value(frame + g_session_delay);
}

/* ---- the frame hooks (rmgk_gekko's debug hooks: start and end of every visible frame) ---- */

bool begin_frame_hook(void*)
{
    if (g_first_frame) {
        g_first_frame = false;
        /* RDRAM exists once the emulation runs */
        auto get_pointer = (ptr_DebugMemGetPointer)dlsym(g_core, "DebugMemGetPointer");
        g_rdram = get_pointer != nullptr ? (const uint8_t*)get_pointer(M64P_DBG_PTR_RDRAM) : nullptr;
        if (g_rdram == nullptr) {
            note("no RDRAM pointer");
            return false;
        }
        /* as znx-bench: video and audio output on (null plugins), no core limiter, no host events */
        m64p::Core.DoCommand(M64CMD_FRAME_OUTPUT_SET, M64FRAME_OUTPUT_VIDEO | M64FRAME_OUTPUT_AUDIO, nullptr);
    }
    /* RMG-K paces visible frames in rmgk_gekko::pace_before_present, which the video plugin's
     * rendering callback calls right before it presents. The null video plugin presents nothing,
     * so the same pacer runs here, once per visible frame. */
    if (g_opt.pace)
        rmgk_gekko::pace_before_present();
    return true;
}

bool end_frame_hook(void*)
{
    const int visible = ++g_visible;
    if (visible % 1200 == 0) {
        RollbackLagStats lag;
        rmgk_gekko::lag_stats(lag);
        const double s = std::chrono::duration<double>(Clock::now() - g_session_begin).count();
        int ping = -1;
        float ahead = 0;
        if (!lag.peers.empty()) {
            ping = lag.peers[0].pingMs;
            ahead = lag.peers[0].framesAhead;
        }
        note("progress: %d frames in %.1f s, final up to %d, rollbacks %d (deepest %d), ping %d ms, frames ahead %.2f",
            visible, s, g_final + 1, g_rollbacks, g_max_depth, ping, ahead);
    }
    return true;
}

/* ---- the control thread: what RMG-K's dialog timer does while a rollback game runs ---- */

std::atomic<bool> g_control_running{false};

void control_thread()
{
    int last_visible = -1;
    Clock::time_point last_progress = Clock::now();
    while (g_control_running.load()) {
        /* KailleraP2PDialog::onStepTimer: n02 CHAT/DROP/EXIT/PING while GekkoNet owns the game */
        p2p_rollback_process_control();
        const auto now = Clock::now();
        const int visible = g_visible.load();
        if (visible != last_visible) {
            last_visible = visible;
            last_progress = now;
        }
        if (g_done.load()) {
            /* Final here. Keep running (and sending our inputs) until the peer is final too:
             * it says so with n02's DROP. */
            if (g_peer_dropped.load() || now - g_done_time > std::chrono::seconds(10)) {
                if (!g_peer_dropped.load())
                    note("no DROP from the peer within 10 s of finishing; stopping");
                rmgk_gekko::request_stop();
            }
        }
        else if (now - last_progress > std::chrono::seconds(30)) {
            note("no frame for 30 s (stalled at %d visible frames); stopping", visible);
            g_stop_failed = true;
            rmgk_gekko::request_stop();
        }
        else if (g_peer_dropped.load()) {
            note("the peer left the game before every frame was final");
            g_stop_failed = true;
            rmgk_gekko::request_stop();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

/* ---- the session (bench_core calls this where it would run its own frame loop) ---- */

int run_session()
{
    if (!m64p::Core.Hook(g_core)) {
        note("cannot hook RMG-K's core wrapper: %s", m64p::Core.GetLastError().c_str());
        return 1;
    }

    char peer_ip[128] = {};
    int peer_port = 0;
    if (!p2p_core_get_peer_endpoint(peer_ip, sizeof(peer_ip), &peer_port)) {
        note("no peer endpoint");
        return 1;
    }
    /* CoreStartEmulation, the GEKKO| branch (Emulation.cpp:1135-1170) */
    if (!rmgk_gekko::set_deterministic(true)) {
        note("set_deterministic failed");
        return 1;
    }
    if (!rmgk_gekko::start_p2p_session("rmgk-gekko", 2, (int)sizeof(uint32_t), g_local_player,
            (unsigned short)p2p_core_get_port(), peer_ip, (unsigned short)peer_port,
            g_session_delay, g_session_prediction)) {
        note("start_p2p_session failed");
        return 1;
    }
    rmgk_gekko::set_debug_hooks(nullptr, begin_frame_hook, end_frame_hook, nullptr);
    note("session: player %d, delay %d, prediction window %d, peer %s:%d, local port %d",
        g_local_player, g_session_delay, g_session_prediction, peer_ip, peer_port, p2p_core_get_port());

    g_session_begin = Clock::now();
    g_control_running = true;
    std::thread control(control_thread);
    const bool ok = rmgk_gekko::execute();
    const double wall = std::chrono::duration<double>(Clock::now() - g_session_begin).count();
    g_control_running = false;
    control.join();

    RollbackLagStats lag;
    rmgk_gekko::lag_stats(lag);
    rmgk_gekko::close_session();

    const int visible = g_visible.load();
    std::printf("frames %d visible, final up to %d, %d check lines\n", visible, g_final + 1, g_checks_printed);
    std::printf("wall %.3f s, %.2f frames per second\n", wall, wall > 0 ? visible / wall : 0.0);
    std::printf("rollbacks %d, re-simulated frames %d, deepest %d frames\n", g_rollbacks, g_resim_frames, g_max_depth);
    std::printf("rollback depths:");
    for (int i = 1; i < 16; i++)
        if (g_depth_hist[i] != 0)
            std::printf(" %d:%d", i, g_depth_hist[i]);
    std::printf("\n");
    std::printf("rmgk lag_stats: rollbacks %d, re-simulated frames %d, deepest %d, waited for input %.1f ms (longest %.1f ms), "
                "slow frames %d, hitches %d\n",
        lag.match.rollbacks, lag.match.rollbackFrames, lag.match.maxRollbackFrames,
        lag.match.waitUs / 1000.0, lag.match.longestWaitUs / 1000.0, lag.match.slowFrames, lag.match.hitches);
    for (const auto& peer : lag.peers)
        std::printf("rmgk peer slot %d: ping %d ms (avg %.1f, jitter %.1f), frames ahead %.2f\n",
            peer.slot, peer.pingMs, peer.avgPingMs, peer.jitterMs, peer.framesAhead);
    uint64_t sent, dropped, delayed;
    znx_netsim_stats(&sent, &dropped, &delayed);
    std::printf("netsim: %llu datagrams sent, %llu dropped, %llu delayed\n",
        (unsigned long long)sent, (unsigned long long)dropped, (unsigned long long)delayed);

    if (!ok)
        note("execute failed");
    if (g_stop_failed.load() || !g_done.load()) {
        note("the session ended before every frame up to %d was final", g_opt.frames);
        return 1;
    }
    return ok ? 0 : 1;
}

/* ---- the ROM's GoodName, the game name n02 announces (RMG-K uses the ROM browser's) ---- */

std::string rom_goodname(const char* rom_path, const std::string& data_dir)
{
    FILE* f = std::fopen(rom_path, "rb");
    if (f == nullptr)
        return "";
    md5_state_t state;
    md5_init(&state);
    std::vector<unsigned char> buffer(1 << 20);
    size_t n;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0)
        md5_append(&state, buffer.data(), (unsigned)n);
    std::fclose(f);
    md5_byte_t digest[16];
    md5_finish(&state, digest);
    char md5[33];
    for (int i = 0; i < 16; i++)
        std::snprintf(md5 + i * 2, 3, "%02X", digest[i]);
    std::ifstream ini(data_dir + "/mupen64plus.ini");
    std::string line, section = std::string("[") + md5 + "]";
    bool in = false;
    while (std::getline(ini, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line[0] == '[')
            in = line == section;
        else if (in && line.rfind("GoodName=", 0) == 0)
            return line.substr(9);
    }
    return std::string("ROM ") + md5;
}

/* ---- the lobby: what KailleraP2PDialog does in rollback mode ---- */

void send_chat(const std::string& text)
{
    std::vector<char> buffer(text.begin(), text.end());
    buffer.push_back(0);
    p2p_send_chat(buffer.data());
}

bool parse_int_after(const char* message, const char* prefix, int* value, int* second)
{
    if (std::strncmp(message, prefix, std::strlen(prefix)) != 0)
        return false;
    const char* rest = message + std::strlen(prefix);
    *value = std::atoi(rest);
    if (second != nullptr) {
        const char* colon = std::strchr(rest, ':');
        *second = colon != nullptr ? std::atoi(colon + 1) : -1;
    }
    return true;
}

bool run_lobby()
{
    char app[128] = {}, game[128] = {}, name[32] = {};
    std::snprintf(app, sizeof(app), "%s", g_opt.app.c_str());
    std::snprintf(name, sizeof(name), "%s", g_opt.name != nullptr ? g_opt.name : (g_opt.host ? "znx-host" : "znx-join"));
    if (g_opt.host)
        std::snprintf(game, sizeof(game), "%s", g_goodname.c_str());
    /* KailleraNetplayDialog.cpp:4319 (host) and :4483 (join): one UDP socket for lobby and game */
    if (!p2p_core_initialize(g_opt.host, g_opt.host ? g_opt.port : 0, app, game, name)) {
        note("cannot open the n02 socket");
        return false;
    }
    note("n02 socket on port %d (%s \"%s\", %s)", p2p_core_get_port(), g_opt.host ? "hosting" : "joining",
        g_opt.host ? game : "", app);

    const auto begin = Clock::now();
    auto last_connect = begin - std::chrono::seconds(1), last_ping = begin;
    bool announced = false, ready = false;
    while (g_game_player.load() == 0) {
        const auto now = Clock::now();
        if (now - begin > std::chrono::seconds(g_opt.lobby_timeout_s)) {
            note("lobby timed out");
            return false;
        }
        if (!g_opt.host && !p2p_is_connected() && now - last_connect > std::chrono::milliseconds(500)) {
            char ip[64];
            std::snprintf(ip, sizeof(ip), "%s", g_opt.join_ip.c_str());
            p2p_core_connect(ip, g_opt.port);   /* sends LOGN_REQ */
            last_connect = now;
        }
        p2p_step();
        bool joined, rollback, got_mode, known;
        {
            std::lock_guard<std::mutex> lock(g_lobby_mutex);
            joined = g_peer_joined;
            rollback = g_mode_rollback;
            got_mode = g_got_mode;
            known = g_hosted_game_known;
        }
        if (joined && p2p_is_connected() && now - last_ping > std::chrono::seconds(1)) {
            p2p_ping();   /* the dialog pings once a second in the lobby (onTravTimer) */
            last_ping = now;
        }
        if (g_opt.host && joined && !announced) {
            /* onPeerJoined / setGameLayer: sendGameLayer, sendRollbackDelaySettings,
             * sendRollbackPredictionSettings, sendRollbackPacingSettings */
            send_chat("RMGK:MODE:RB");
            send_chat("RMGK:RBDELAY:3:" + std::to_string(g_opt.delay));   /* mode 3 = custom */
            send_chat("RMGK:RBPRED:" + std::to_string(g_opt.prediction));
            send_chat("RMGK:RBPACE:0");
            announced = true;
        }
        if (!ready && joined && p2p_is_connected()) {
            /* the player presses Ready: the host at once, the joiner once it knows the host's
             * game and has been told the rollback layer (onReady needs m_gameName) */
            if (g_opt.host || (known && got_mode && rollback)) {
                p2p_set_ready(true);
                ready = true;
                note("ready");
            }
        }
        usleep(1000);
    }
    (void)announced;
    return true;
}

void usage()
{
    std::fprintf(stderr,
        "usage: znx-net --rom FILE (--host PORT | --join IP:PORT) [--inputs FILE --player N]\n"
        "               [--delay D] [--prediction P] [--frames N] [--check-every K] [--check-skip LO HI]\n"
        "               [--sim-latency MS] [--sim-jitter MS] [--sim-loss PCT] [--sim-seed S]\n"
        "               [--no-pace] [--name NAME] [--app APP] [--gekko-log] [--verbose]\n"
        "  An RMG-K P2P rollback session (n02 lobby + GekkoNet over the n02 socket), headless.\n"
        "  --host PORT          host on this UDP port (player 1); --join IP:PORT joins it (player 2)\n"
        "  --inputs FILE        the controller script (bench/inputs format); this side plays --player N\n"
        "  --delay D            input delay in frames, default 2 (the joiner takes the host's, as RMG-K does)\n"
        "  --prediction P       GekkoNet prediction window, default 7 (likewise)\n"
        "  --frames N           frames to finish (every frame below N final on both sides), default 17000\n"
        "  --check-every K      \"check\" lines every K frames (and after the last), default 60\n"
        "  --sim-latency MS     add MS to every datagram this side sends (so the round trip gains 2*MS)\n"
        "  --sim-jitter MS      +- MS on GekkoNet's datagrams (reordering)\n"
        "  --sim-loss PCT       drop PCT %% of GekkoNet's datagrams\n"
        "  --no-pace            run as fast as possible instead of RMG-K's 60 Hz presentation pacer\n"
        "  --gekko-log          write RMG-K's verbose GekkoNet log to build/<arch>/profile/net-<side>/Logs\n"
        "  The socket is bound to 127.0.0.1 only; nothing is sent anywhere but the given peer.\n");
    std::exit(2);
}

void* load(const char* dir, const char* name, int flags)
{
    std::string path = std::string(dir) + "/" + name;
    void* lib = dlopen(path.c_str(), flags);
    if (lib == nullptr)
        std::fprintf(stderr, "znx-net: %s\n", dlerror());
    return lib;
}

void print_line(int is_error, const char* line)
{
    std::fprintf(is_error ? stderr : stdout, "%s\n", line);
}

} // namespace

/* ---- callbacks from the glue ---- */

void znx_net_state_saved(int frame)
{
    if (frame > g_max_saved)
        g_max_saved = frame;
    const int after = frame + 1;   /* GekkoNet frame F completed = F + 1 frames, the bench's count */
    if (after > g_opt.frames || g_rdram == nullptr)
        return;
    if (after % g_opt.check_every != 0 && after != g_opt.frames)
        return;
    uint64_t whole, rest;
    znx_check_rdram(g_rdram, g_opt.check_skip_lo, g_opt.check_skip_hi, &whole, &rest);
    g_pending_checks[frame] = { whole, rest };
}

void znx_net_state_loaded(int frame)
{
    (void)frame;
    if (g_depth > 0) {
        g_depth_hist[std::min(g_depth, 15)]++;
    }
    g_rollbacks++;
    g_depth = 0;
}

void znx_net_frame_resimulated(void)
{
    g_resim_frames++;
    g_depth++;
    g_max_depth = std::max(g_max_depth, g_depth);
}

void znx_net_end_frame(int confirmed)
{
    if (g_depth > 0) {
        g_depth_hist[std::min(g_depth, 15)]++;
        g_depth = 0;
    }
    const int final_frame = std::min(confirmed, g_max_saved);
    if (final_frame > g_final)
        g_final = final_frame;
    while (!g_pending_checks.empty() && g_pending_checks.begin()->first <= g_final) {
        const auto& entry = *g_pending_checks.begin();
        std::printf("check %d %016llx %016llx\n", entry.first + 1,
            (unsigned long long)entry.second.first, (unsigned long long)entry.second.second);
        g_checks_printed++;
        g_pending_checks.erase(g_pending_checks.begin());
    }
    if (!g_done.load() && g_final >= g_opt.frames - 1) {
        g_done_time = Clock::now();
        g_done = true;
        note("every frame up to %d is final; telling the peer (n02 DROP)", g_opt.frames);
        g_local_drop = true;
        p2p_drop_game();
        g_local_drop = false;
    }
}

int znx_net_visible_frames(void) { return g_visible.load(); }
const char* znx_net_log_dir(void) { return g_log_dir.c_str(); }
bool znx_net_verbose_log(void) { return g_opt.gekko_log; }

void znx_net_chat(const char* nick, const char* message)
{
    (void)nick;
    int a, b;
    std::lock_guard<std::mutex> lock(g_lobby_mutex);
    if (std::strncmp(message, "RMGK:MODE:", 10) == 0) {
        g_got_mode = true;
        g_mode_rollback = std::strcmp(message + 10, "RB") == 0;
    }
    else if (parse_int_after(message, "RMGK:RBDELAY:", &a, &b)) {
        g_got_delay = true;
        g_host_delay = b >= 0 ? b : 2;
    }
    else if (parse_int_after(message, "RMGK:RBPRED:", &a, nullptr)) {
        g_got_prediction = true;
        g_host_prediction = a;
    }
    else if (std::strncmp(message, "RMGK:", 5) == 0) {
        /* RMGK:RBPACE: the pacing model; RMG-K ignores it now (rmgk_gekko.cpp reset_gekko_log) */
    }
    else {
        note("chat <%s> %s", nick, message);
    }
}

void znx_net_game_started(const char* game, int player)
{
    int expected = 0;
    g_game_player.compare_exchange_strong(expected, player);
    (void)game;
}

void znx_net_game_ended(void)
{
    if (!g_local_drop.load() && g_game_player.load() != 0)
        g_peer_dropped = true;
}

void znx_net_peer_joined(void)
{
    std::lock_guard<std::mutex> lock(g_lobby_mutex);
    g_peer_joined = true;
}

void znx_net_peer_left(void)
{
    g_peer_dropped = true;
}

void znx_net_peer_info(const char* name, const char* app)
{
    std::lock_guard<std::mutex> lock(g_lobby_mutex);
    g_peer_name = name;
    g_peer_app = app;
}

void znx_net_hosted_game(const char* game)
{
    std::lock_guard<std::mutex> lock(g_lobby_mutex);
    g_hosted_game = game;
    g_hosted_game_known = true;
}

void znx_net_debug(const char* message)
{
    if (g_opt.verbose || std::strstr(message, "alert") != nullptr || std::strstr(message, "drop") != nullptr ||
        std::strstr(message, "Connection") != nullptr || std::strstr(message, "Peer") != nullptr)
        note("n02: %s", message);
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (!std::strcmp(a, "--rom")) g_opt.rom = next();
        else if (!std::strcmp(a, "--host")) { g_opt.host = true; g_opt.port = std::atoi(next()); }
        else if (!std::strcmp(a, "--join")) {
            std::string target = next();
            const size_t colon = target.rfind(':');
            if (colon == std::string::npos) usage();
            g_opt.join_ip = target.substr(0, colon);
            g_opt.port = std::atoi(target.c_str() + colon + 1);
        }
        else if (!std::strcmp(a, "--inputs")) g_opt.inputs = next();
        else if (!std::strcmp(a, "--player")) g_opt.player = std::atoi(next());
        else if (!std::strcmp(a, "--delay")) g_opt.delay = std::atoi(next());
        else if (!std::strcmp(a, "--prediction")) g_opt.prediction = std::atoi(next());
        else if (!std::strcmp(a, "--frames")) g_opt.frames = std::atoi(next());
        else if (!std::strcmp(a, "--check-every")) g_opt.check_every = std::atoi(next());
        else if (!std::strcmp(a, "--check-skip")) {
            g_opt.check_skip_lo = std::strtol(next(), nullptr, 16) & 0x7fffff;
            g_opt.check_skip_hi = std::strtol(next(), nullptr, 16) & 0x7fffff;
        }
        else if (!std::strcmp(a, "--sim-latency")) g_opt.sim_latency = std::atoi(next());
        else if (!std::strcmp(a, "--sim-jitter")) g_opt.sim_jitter = std::atoi(next());
        else if (!std::strcmp(a, "--sim-loss")) g_opt.sim_loss = std::atof(next());
        else if (!std::strcmp(a, "--sim-seed")) g_opt.sim_seed = (unsigned)std::strtoul(next(), nullptr, 0);
        else if (!std::strcmp(a, "--no-pace")) g_opt.pace = false;
        else if (!std::strcmp(a, "--name")) g_opt.name = next();
        else if (!std::strcmp(a, "--app")) g_opt.app = next();
        else if (!std::strcmp(a, "--gekko-log")) g_opt.gekko_log = true;
        else if (!std::strcmp(a, "--verbose")) g_opt.verbose = true;
        else usage();
    }
    if (g_opt.rom == nullptr || g_opt.port <= 0 || g_opt.port > 65535 || g_opt.frames <= 0 || g_opt.check_every <= 0)
        usage();
    if (!g_opt.host && g_opt.join_ip != "127.0.0.1" && g_opt.join_ip != "localhost") {
        std::fprintf(stderr, "znx-net: this build only joins 127.0.0.1\n");
        return 2;
    }
    if (g_opt.inputs != nullptr && (g_opt.player < 1 || g_opt.player > 2))
        usage();
    setvbuf(stdout, nullptr, _IOLBF, 0);

    char exe[4096], dir[4096];
    uint32_t exe_size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &exe_size) != 0 || realpath(exe, dir) == nullptr) {
        std::fprintf(stderr, "znx-net: cannot find my own path\n");
        return 1;
    }
    std::strcpy(dir, dirname(dir));
    const std::string data_dir = std::string(dir) + "/data";
    const std::string user_dir = std::string(dir) + "/profile/net-" + (g_opt.host ? "host" : "join");
    mkdir((std::string(dir) + "/profile").c_str(), 0755);
    mkdir(user_dir.c_str(), 0755);
    g_log_dir = user_dir;

    if (g_opt.inputs != nullptr && !load_script(g_opt.inputs, g_opt.player))
        return 1;
    g_goodname = rom_goodname(g_opt.rom, data_dir);
    if (g_goodname.empty()) {
        std::fprintf(stderr, "znx-net: cannot read %s\n", g_opt.rom);
        return 1;
    }

    znx_netsim_config sim = {};
    sim.latency_ms = g_opt.sim_latency;
    sim.jitter_ms = g_opt.sim_jitter;
    sim.loss_pct = g_opt.sim_loss;
    sim.seed = g_opt.sim_seed != 0 ? g_opt.sim_seed : (g_opt.host ? 1u : 2u);
    sim.loopback_only = 1;
    znx_netsim_configure(&sim);

    if (!run_lobby()) {
        p2p_core_cleanup();
        znx_netsim_shutdown();
        return 1;
    }
    g_local_player = g_game_player.load();
    {
        std::lock_guard<std::mutex> lock(g_lobby_mutex);
        if (g_opt.host) {
            g_session_delay = std::clamp(g_opt.delay, 0, 9);
            g_session_prediction = g_opt.prediction;
        } else {
            /* effectiveRollbackFrameDelay / effectiveRollbackPredictionWindow: the host's */
            g_session_delay = g_got_delay ? g_host_delay : std::clamp(g_opt.delay, 0, 9);
            g_session_prediction = g_got_prediction && g_host_prediction > 0 ? g_host_prediction
                : (g_opt.prediction > 0 ? g_opt.prediction : 7);
            if (g_hosted_game != g_goodname)
                note("the host plays \"%s\", this ROM is \"%s\"", g_hosted_game.c_str(), g_goodname.c_str());
        }
        note("game starts: player %d, peer \"%s\" (%s)", g_local_player, g_peer_name.c_str(), g_peer_app.c_str());
    }
    if (g_opt.inputs != nullptr && g_opt.player != g_local_player)
        note("playing the script's player %d from controller slot %d", g_opt.player, g_local_player);

    struct znx_options o;
    znx_options_defaults(&o);
    o.rom_path = g_opt.rom;
    o.frames = 0;            /* the session decides when to stop */
    o.hash_every = 0;
    o.input_players = 2;     /* what start_p2p_session sets too */
    o.verbose = g_opt.verbose ? 1 : 0;

    struct znx_host host;
    std::memset(&host, 0, sizeof(host));
    host.name = "znx-net";
    host.core = load(dir, "libmupen64plus.dylib", RTLD_NOW | RTLD_GLOBAL);
    host.video = load(dir, "znx-video-null.dylib", RTLD_NOW);
    host.rsp = load(dir, "mupen64plus-rsp-hle.dylib", RTLD_NOW);
    host.input = load(dir, "znx-input-net.dylib", RTLD_NOW);
    if (host.core == nullptr || host.video == nullptr || host.rsp == nullptr || host.input == nullptr)
        return 1;
    auto set_source = (void (*)(uint32_t (*)(int)))dlsym(host.input, "znx_input_set_source");
    if (set_source == nullptr)
        return 1;
    set_source(input_source);
    g_core = host.core;
    host.sym = dlsym;
    host.print = print_line;
    host.data_dir = data_dir.c_str();
    host.user_dir = user_dir.c_str();
    host.session_run = run_session;

    const int result = znx_bench_run(&host, &o);

    p2p_core_cleanup();
    znx_netsim_shutdown();
    m64p::Core.Unhook();
    dlclose(host.input);
    dlclose(host.rsp);
    dlclose(host.video);
    dlclose(host.core);
    return result;
}
