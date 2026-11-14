// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-net: a headless RMG-K P2P rollback session, against another copy of this program or the
 * Switch program, with the lobby and session code the Switch runs (net/p2p/znx_p2p.cpp).
 *
 *   znx-net --rom ROM --host PORT              --inputs FILE --player 1
 *   znx-net --rom ROM --join IP:PORT|CODE      --inputs FILE --player 2
 *   znx-net --fetch-list HOST:PORT             (prints the public list, then exits)
 *   znx-net --rom ROM --lobby ws://H:P/ws --create-room NAME [--start-players N] --inputs FILE --player 1
 *   znx-net --rom ROM --lobby ws://H:P/ws --join-room [NAME]                    --inputs FILE --player 2
 *       RMG-K's rollback lobby instead of P2P: net/lobby_play.cpp (the code behind the Switch's lobby
 *       screens) connects, opens or joins a room of this ROM, the host starts once everyone is connected,
 *       and the match runs through RMG-K's start_lobby_session over the room's ICE mesh (libjuice).
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
 * net/p2p/znx_p2p.cpp plays the part of RMG-K's Qt dialog (host or join, connect codes, the public
 * list, announce or take the rollback settings, ready, start, DROP) and runs the session; this file
 * plays CoreStartEmulation (bench/bench_core.c sets the core up the way a session does) and the
 * controller (an input plugin fed from a script), and prints the check lines.
 *
 * Output (stdout):
 *   check <frame> <whole> <rest>   RDRAM checksums after <frame> frames (the bench's "check" line),
 *                                  every --check-every frames and after the last one; printed
 *                                  only once that frame is final (no later input can roll it back)
 *   code <CODE>                    host with --nat: the connect code, once the server gave it
 *   lobby: ...                     the peer, ping and agreed delay before the match
 *   summary lines at the end: frames, wall time, frames per second, rollbacks
 * Diagnostics go to stderr. */

#include <arpa/inet.h>
#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ZNX_STATIC_LIBS
/* The Linux self-test of the Switch build (builder/build-switch.sh --selftest-net): the core and the
 * plugins are linked in and found through switch/dynlib_static.c's tables, as on the console. */
extern "C" void* znx_static_lib(const char* name);
extern "C" void* osal_dynlib_getproc(void* lib, const char* name);
#else
#include <dlfcn.h>
#include <mach-o/dyld.h>
#endif

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

#include "m64p/Api.hpp"
#include "rmgk_gekko.hpp"

#include "lobby_play.h"
#include "p2p/znx_p2p.h"
#include "znx_net.h"

namespace
{
using Clock = std::chrono::steady_clock;

/* ---- options ---- */

struct Options {
    const char* rom = nullptr;
    bool host = false;
    int port = 0;                 /* host: port to listen on */
    std::string join;             /* joiner: ip:port or a connect code */
    const char* inputs = nullptr;
    int player = 0;               /* which player's lines of the script this side plays */
    int delay = 2;                /* host: 0 = automatic from the ping, as RMG-K's default */
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
    int precompile_ms = 12;      /* the recompiler's warm list: znx_options.precompile_ms */
    bool no_warm = false;
    const char* warm = nullptr;
    bool bind_any = false;        /* listen on every interface (a LAN test against the Switch) */
    bool enlist = false;
    std::string nat, list;        /* host:port of the traversal and list servers (tests: a local mock) */
    const char* name = nullptr;
    std::string app = ZNX_P2P_APP;
    int lobby_timeout_s = 60;
    const char* code_file = nullptr;   /* host: keeps the connect code and token between runs */
    int ready_after_s = 0;        /* press Ready this long after the peer arrived (lets the automatic delay settle) */
    /* --lobby: RMG-K's rollback lobby (net/lobby_play.h) */
    std::string lobby;            /* ws://host:port/ws */
    std::string create_room;      /* host: open a room with this name */
    bool join_room = false;       /* join a room of this ROM (the first one, or the one named) */
    std::string join_room_name;
    std::string room_password;    /* --room-password: for opening or joining a locked room */
    int max_players = 2;
    int start_players = 2;        /* host: start when this many are seated and RMG-K's rule allows it */
    std::string ice_bind;         /* the ICE agents' address; default 127.0.0.1 (every interface with --bind-any) */
    std::string stun = "off";     /* "off" (default: nothing leaves the machine), "server", or host:port */
    int manifest_cheat = -1;      /* host: a test cheat writing this byte at 0x80700000 */
    bool manifest_save = false;   /* host: a test SRAM save in the manifest */
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

int g_session_delay = 2;
std::string g_goodname;
std::string g_log_dir;

void* g_core = nullptr;
const uint8_t* g_rdram = nullptr;
bool g_first_frame = true;

int g_max_saved = -1;                    /* newest GekkoNet frame whose post-frame state was saved */
int g_final = -1;                        /* newest frame known final */
std::map<int, std::pair<uint64_t, uint64_t>> g_pending_checks;   /* frame -> checksums, not yet final */
int g_checks_printed = 0;
std::atomic<bool> g_done{false};         /* every frame below --frames is final */
Clock::time_point g_done_time;
std::atomic<bool> g_stop_failed{false};
int g_visible = 0;

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
    std::fprintf(stderr, "znx-net[%s]: %s\n", g_opt.lobby.empty() ? (g_opt.host ? "host" : "join")
                                                : (!g_opt.create_room.empty() ? "room-host" : "room-join"), line);
}

void p2p_log(const char* line, void*)
{
    note("%s", line);
}

/* ---- the libraries: dlopen on the Mac, the linked-in tables in the self-test ---- */

void* lib_sym(void* lib, const char* name)
{
#ifdef ZNX_STATIC_LIBS
    return osal_dynlib_getproc(lib, name);
#else
    return dlsym(lib, name);
#endif
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

/* ---- the session hooks ---- */

int begin_visible(void*)
{
    if (g_first_frame) {
        g_first_frame = false;
        /* RDRAM exists once the emulation runs */
        auto get_pointer = (ptr_DebugMemGetPointer)lib_sym(g_core, "DebugMemGetPointer");
        g_rdram = get_pointer != nullptr ? (const uint8_t*)get_pointer(M64P_DBG_PTR_RDRAM) : nullptr;
        if (g_rdram == nullptr) {
            note("no RDRAM pointer");
            return 0;
        }
        /* as znx-bench: video and audio output on (null plugins), no core limiter, no host events */
        m64p::Core.DoCommand(M64CMD_FRAME_OUTPUT_SET, M64FRAME_OUTPUT_VIDEO | M64FRAME_OUTPUT_AUDIO, nullptr);
    }
    /* RMG-K paces visible frames in rmgk_gekko::pace_before_present, which the video plugin's
     * rendering callback calls right before it presents. The null video plugin presents nothing,
     * so the same pacer runs here, once per visible frame. */
    if (g_opt.pace)
        rmgk_gekko::pace_before_present();
    return 1;
}

int end_visible(void*)
{
    const int visible = ++g_visible;
    if (visible % 1200 == 0) {
        struct znx_p2p_live live;
        znx_p2p_live(&live);
        const double s = std::chrono::duration<double>(Clock::now() - g_session_begin).count();
        note("progress: %d frames in %.1f s, final up to %d, rollbacks %d (deepest %d), ping %d ms, frames ahead %.2f",
            visible, s, g_final + 1, g_rollbacks, g_max_depth, live.ping, live.frames_ahead);
    }
    if (g_done.load()) {
        /* Final here. Keep running (and sending our inputs) until the peer is final too: it says
         * so with n02's DROP. */
        if (znx_p2p_peer_dropped() || Clock::now() - g_done_time > std::chrono::seconds(10)) {
            if (!znx_p2p_peer_dropped())
                note("no DROP from the peer within 10 s of finishing; stopping");
            znx_p2p_stop_game();
        }
    } else if (znx_p2p_peer_dropped()) {
        note("the peer left the game before every frame was final");
        g_stop_failed = true;
        znx_p2p_stop_game();
    }
    return 1;
}

void state_saved(int frame, void*)
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

void state_loaded(int, void*)
{
    if (g_depth > 0)
        g_depth_hist[std::min(g_depth, 15)]++;
    g_rollbacks++;
    g_depth = 0;
}

void frame_resimulated(void*)
{
    g_resim_frames++;
    g_depth++;
    g_max_depth = std::max(g_max_depth, g_depth);
}

void end_frame(int confirmed, void*)
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
        znx_p2p_send_drop();
    }
}

/* ---- the session (bench_core calls this where it would run its own frame loop) ---- */

int run_session()
{
    struct znx_p2p_session_hooks hooks;
    std::memset(&hooks, 0, sizeof(hooks));
    hooks.begin_visible = begin_visible;
    hooks.end_visible = end_visible;
    hooks.state_saved = state_saved;
    hooks.state_loaded = state_loaded;
    hooks.frame_resimulated = frame_resimulated;
    hooks.end_frame = end_frame;
    hooks.stop_on_peer_drop = 0;
    hooks.verbose_log = g_opt.gekko_log;
    hooks.log_dir = g_log_dir.c_str();

    g_session_begin = Clock::now();
    const int result = g_opt.lobby.empty() ? znx_p2p_run_session(g_core, &hooks)
                                           : znx_lobby_play_run_session(g_core, &hooks);
    const double wall = std::chrono::duration<double>(Clock::now() - g_session_begin).count();
    char end_text[256];
    const int end = znx_p2p_last_end(end_text, sizeof(end_text));

    const int visible = g_visible;
    std::printf("frames %d visible, final up to %d, %d check lines\n", visible, g_final + 1, g_checks_printed);
    std::printf("wall %.3f s, %.2f frames per second\n", wall, wall > 0 ? visible / wall : 0.0);
    std::printf("rollbacks %d, re-simulated frames %d, deepest %d frames\n", g_rollbacks, g_resim_frames, g_max_depth);
    std::printf("rollback depths:");
    for (int i = 1; i < 16; i++)
        if (g_depth_hist[i] != 0)
            std::printf(" %d:%d", i, g_depth_hist[i]);
    std::printf("\n");
    uint64_t sent, dropped, delayed;
    if (g_opt.lobby.empty()) {
        znx_netsim_stats(&sent, &dropped, &delayed);
        std::printf("netsim: %llu datagrams sent, %llu dropped, %llu delayed\n",
            (unsigned long long)sent, (unsigned long long)dropped, (unsigned long long)delayed);
    } else {
        znx_lobby_rmgk_netsim_stats(&sent, &dropped);
        std::printf("netsim: %llu GekkoNet datagrams sent over ICE, %llu dropped\n", (unsigned long long)sent,
            (unsigned long long)dropped);
    }
    note("session end %d: %s", end, end_text);

    if (result != 0)
        note("the session failed");
    if (g_stop_failed.load() || !g_done.load()) {
        note("the session ended before every frame up to %d was final", g_opt.frames);
        return 1;
    }
    return result;
}

/* ---- the ROM's GoodName, the game name n02 announces (RMG-K uses the ROM browser's) ---- */

std::string g_rom_md5;

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
    g_rom_md5 = md5;
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

/* ---- the lobby: net/p2p/znx_p2p.cpp, driven as the Switch's online screens drive it ---- */

bool split_host_port(const std::string& text, std::string& host, int& port)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos)
        return false;
    host = text.substr(0, colon);
    port = std::atoi(text.c_str() + colon + 1);
    return !host.empty() && port > 0 && port <= 65535;
}

std::string g_saved_code, g_saved_token;

void save_identity(const char* code, const char* token, void*)
{
    g_saved_code = code;
    g_saved_token = token;
    if (g_opt.code_file != nullptr) {
        FILE* f = std::fopen(g_opt.code_file, "w");
        if (f != nullptr) {
            std::fprintf(f, "%s\n%s\n", code, token);
            std::fclose(f);
        }
    }
}

bool run_lobby()
{
    std::string nat_host, list_host;
    int nat_port = 0, list_port = 0;
    if (!g_opt.nat.empty() && !split_host_port(g_opt.nat, nat_host, nat_port)) {
        note("--nat wants HOST:PORT");
        return false;
    }
    if (!g_opt.list.empty() && !split_host_port(g_opt.list, list_host, list_port)) {
        note("--list wants HOST:PORT");
        return false;
    }
    char code_check[32];
    if (!g_opt.host && znx_p2p_normalize_code(g_opt.join.c_str(), code_check, sizeof(code_check)) && nat_host.empty()) {
        note("joining by code needs --nat HOST:PORT");
        return false;
    }
    if (g_opt.code_file != nullptr) {
        std::ifstream f(g_opt.code_file);
        std::getline(f, g_saved_code);
        std::getline(f, g_saved_token);
    }
    struct znx_p2p_config config;
    std::memset(&config, 0, sizeof(config));
    config.host = g_opt.host;
    config.port = g_opt.port;
    config.game = g_goodname.c_str();
    config.username = g_opt.name != nullptr ? g_opt.name : (g_opt.host ? "znx-host" : "znx-join");
    config.app = g_opt.app.c_str();
    config.join_target = g_opt.join.c_str();
    config.delay = g_opt.delay;
    config.prediction = g_opt.prediction;
    /* connect codes and the list only against a server named on the command line */
    config.use_codes = g_opt.host && !nat_host.empty();
    config.enlist = g_opt.enlist && !list_host.empty();
    config.nat_host = nat_host.c_str();
    config.nat_port = nat_port;
    config.list_host = list_host.c_str();
    config.list_port = list_port;
    config.code = g_saved_code.c_str();
    config.token = g_saved_token.c_str();
    config.save_identity = save_identity;
    config.log = p2p_log;
    char error[256];
    if (znx_p2p_open(&config, error, sizeof(error)) != 0) {
        note("%s", error);
        return false;
    }

    const auto begin = Clock::now();
    std::string printed_code;
    bool ready = false, lobby_line = false;
    Clock::time_point lobby_since{};
    for (;;) {
        struct znx_p2p_status s;
        znx_p2p_status(&s);
        if (s.code[0] && printed_code != s.code) {
            printed_code = s.code;
            std::printf("code %s\n", s.code);
        }
        if (s.phase == ZNX_P2P_FAILED) {
            note("the lobby failed: %s", s.message);
            return false;
        }
        if (s.phase == ZNX_P2P_STARTING || s.phase == ZNX_P2P_IN_GAME)
            break;
        if (Clock::now() - begin > std::chrono::seconds(g_opt.lobby_timeout_s)) {
            note("lobby timed out");
            return false;
        }
        if (s.phase == ZNX_P2P_LOBBY) {
            if (lobby_since == Clock::time_point{})
                lobby_since = Clock::now();
            if (!g_opt.host && s.game_mismatch) {
                note("the host plays \"%s\", this ROM is \"%s\"", s.host_game, g_goodname.c_str());
                return false;
            }
            /* the player presses Ready: the host once a peer is there and its ping is known, the
             * joiner once it knows the host's game and has been told the rollback layer */
            if (!ready && s.can_ready && s.ping >= 0 &&
                Clock::now() - lobby_since >= std::chrono::seconds(g_opt.ready_after_s)) {
                if (!lobby_line) {
                    std::printf("lobby: peer \"%s\"%s, ping %d ms, delay %d%s, prediction %d\n", s.peer_name,
                        s.app_mismatch ? " (another app version)" : "", s.ping, s.delay, s.delay_auto ? " (auto)" : "",
                        s.prediction);
                    lobby_line = true;
                }
                znx_p2p_set_ready(1);
                ready = true;
                note("ready");
            }
        }
        usleep(2000);
    }
    struct znx_p2p_status s;
    znx_p2p_status(&s);
    g_session_delay = s.delay;
    note("game starts: player %d, peer \"%s\", delay %d, prediction %d", s.player, s.peer_name, s.delay, s.prediction);
    if (g_opt.inputs != nullptr && g_opt.player != s.player)
        note("playing the script's player %d from controller slot %d", g_opt.player, s.player);
    return true;
}

/* ---- the rollback lobby: net/lobby_play.cpp, driven as the Switch's lobby screens drive it ---- */

void lobby_log(const char* line)
{
    note("%s", line);
}

bool run_lobby_room()
{
    struct znx_lobby_play_config config;
    std::memset(&config, 0, sizeof(config));
    const bool host = !g_opt.create_room.empty();
    config.server_url = g_opt.lobby.c_str();
    config.username = g_opt.name != nullptr ? g_opt.name : (host ? "znx-host" : "znx-join");
    config.rom_md5 = g_rom_md5.c_str();
    config.rom_name = g_goodname.c_str();
    /* default: no STUN, no TURN, ICE on 127.0.0.1 only: nothing leaves this machine */
    config.stun = g_opt.stun == "server" ? nullptr : g_opt.stun.c_str();
    config.no_turn = 1;
    if (!g_opt.ice_bind.empty())
        config.bind_address = g_opt.ice_bind.c_str();
    else if (!g_opt.bind_any)
        config.bind_address = "127.0.0.1";
    config.log = lobby_log;
    char error[256];
    if (znx_lobby_play_open(&config, error, sizeof(error)) != 0) {
        note("%s", error);
        return false;
    }
    const auto begin = Clock::now();
    bool asked = false, started = false, delay_set = false, room_line = false;
    for (;;) {
        znx_lobby_play_poll();
        struct znx_lobby_play_status s;
        znx_lobby_play_status(&s);
        char title[160], text[512];
        const int problem = znx_lobby_play_take_problem(title, sizeof(title), text, sizeof(text));
        if (problem != ZNX_LPP_NONE) {
            note("problem: %s: %s", title, text);
            return false;
        }
        if (s.phase == ZNX_LP_FAILED || s.phase == ZNX_LP_CLOSED)
            return false;
        if (s.phase == ZNX_LP_READY)
            break;
        if (Clock::now() - begin > std::chrono::seconds(g_opt.lobby_timeout_s)) {
            note("lobby timed out in phase %d", s.phase);
            return false;
        }
        if (s.phase == ZNX_LP_ROOMS && !asked) {
            if (host) {
                asked = znx_lobby_play_create_room(g_opt.create_room.c_str(), g_opt.max_players, g_opt.room_password.c_str()) == 0;
            } else {
                struct znx_lobby_play_room rooms[16];
                int all = 0;
                const int n = znx_lobby_play_rooms(rooms, 16, &all);
                for (int i = 0; i < n && !asked; ++i)
                    if (!rooms[i].in_game && rooms[i].players < rooms[i].max_players &&
                        (g_opt.join_room_name.empty() || g_opt.join_room_name == rooms[i].name)) {
                        note("joining room \"%s\" of %s (%d of %d players, %d rooms in all)", rooms[i].name, rooms[i].host,
                             rooms[i].players, rooms[i].max_players, all);
                        asked = znx_lobby_play_join_room(rooms[i].id, g_opt.room_password.c_str()) == 0;
                    }
            }
        }
        if (s.phase == ZNX_LP_ROOM) {
            struct znx_lobby_play_room_view v;
            if (znx_lobby_play_room_view(&v) == 0) {
                if (!delay_set) {
                    znx_lobby_play_set_delay(g_opt.delay > 0 ? g_opt.delay : -1);
                    delay_set = true;
                }
                if (host && !started && v.seated >= g_opt.start_players && v.can_start) {
                    if (!room_line) {
                        for (const auto& seat : v.seats)
                            if (seat.user_id != 0)
                                std::printf("lobby: seat \"%s\"%s%s, delay %d, ICE %d, ping %d ms\n", seat.name,
                                    seat.self ? " (us)" : "", seat.host ? " (host)" : "", seat.delay, seat.ice, seat.ping);
                        room_line = true;
                    }
                    if (g_opt.manifest_cheat >= 0 || g_opt.manifest_save)
                        znx_lobby_play_test_manifest(g_opt.manifest_cheat, g_opt.manifest_save);
                    started = znx_lobby_play_start() == 0;
                }
            }
        }
        usleep(2000);
    }
    g_session_delay = znx_lobby_play_local_delay();
    note("game starts: seat %d of %d, delay %d", znx_lobby_play_local_slot(), znx_lobby_play_input_players(),
         g_session_delay);
    if (g_opt.player == 0)
        g_opt.player = znx_lobby_play_local_slot();
    if (g_opt.inputs != nullptr && !load_script(g_opt.inputs, g_opt.player))
        return false;
    return true;
}

int prepare_core(void* core)
{
    return znx_lobby_play_prepare_core(core);
}

int fetch_list(const std::string& where)
{
    std::string host;
    int port = 0;
    if (!split_host_port(where, host, port)) {
        std::fprintf(stderr, "znx-net: --fetch-list wants HOST:PORT\n");
        return 2;
    }
    std::vector<znx_p2p_listing> list(64);
    char error[256];
    const int n = znx_p2p_fetch_list(host.c_str(), port, list.data(), (int)list.size(), error, sizeof(error));
    if (n < 0) {
        std::fprintf(stderr, "znx-net: %s\n", error);
        return 1;
    }
    for (int i = 0; i < n; i++)
        std::printf("listing game=\"%s\" app=\"%s\" user=\"%s\" address=%s code=%s\n", list[i].game, list[i].app,
            list[i].user, list[i].address, list[i].code);
    std::printf("listings %d\n", n);
    return 0;
}

void usage()
{
    std::fprintf(stderr,
        "usage: znx-net --rom FILE (--host PORT | --join IP:PORT|CODE | --lobby URL (--create-room NAME | --join-room [NAME]))\n"
        "               [--inputs FILE --player N]\n"
        "               [--delay D] [--prediction P] [--frames N] [--check-every K] [--check-skip LO HI]\n"
        "               [--sim-latency MS] [--sim-jitter MS] [--sim-loss PCT] [--sim-seed S]\n"
        "               [--bind-any] [--nat HOST:PORT] [--list HOST:PORT] [--enlist] [--code-file F]\n"
        "               [--no-pace] [--name NAME] [--app APP] [--lobby-timeout S] [--ready-after S] [--gekko-log] [--verbose]\n"
        "       znx-net --fetch-list HOST:PORT\n"
        "  An RMG-K P2P rollback session (n02 lobby + GekkoNet over the n02 socket), headless.\n"
        "  --host PORT          host on this UDP port (player 1); --join IP:PORT or a connect code joins (player 2)\n"
        "  --inputs FILE        the controller script (bench/inputs format); this side plays --player N\n"
        "  --delay D            host: input delay in frames, default 2; 0 = from the ping as RMG-K's default\n"
        "                       (the joiner takes the host's, as RMG-K does)\n"
        "  --prediction P       GekkoNet prediction window, default 7 (likewise)\n"
        "  --frames N           frames to finish (every frame below N final on both sides), default 17000\n"
        "  --check-every K      \"check\" lines every K frames (and after the last), default 60\n"
        "  --sim-latency MS     add MS to every datagram this side sends (so the round trip gains 2*MS)\n"
        "  --sim-jitter MS      +- MS on GekkoNet's datagrams (reordering)\n"
        "  --sim-loss PCT       drop PCT %% of GekkoNet's datagrams\n"
        "  --bind-any           listen on every interface, and join any address (default: 127.0.0.1 only)\n"
        "  --nat HOST:PORT      host: get a connect code from this traversal server (RMG-K's is\n"
        "                       nat.smash64.net:6364); joiner: look codes up there\n"
        "  --list HOST:PORT     with --enlist, the public list server (RMG-K's: p2plist.smash64.net:27887)\n"
        "  --ready-after S      press Ready S seconds after the peer arrived (the automatic delay needs ~5 s of pings)\n"
        "  --code-file F        host: keep the connect code between runs, as RMG-K keeps it in its settings\n"
        "  --no-pace            run as fast as possible instead of RMG-K's 60 Hz presentation pacer\n"
        "  --precompile-ms N    compile the ROM's warm list ahead in the time the pacer leaves over (default 12; 0 = off);\n"
        "                       --warm FILE: that list file; --no-warm: no list (see znx-bench)\n"
        "  --gekko-log          write RMG-K's verbose GekkoNet log to build/<arch>/profile/net-<side>/Logs\n"
        "  --fetch-list H:P     GET http://H:P/plist.txt and print the games with a connect code\n"
        "  --lobby URL          RMG-K's rollback lobby (ws://HOST:PORT/ws) instead of P2P; then:\n"
        "  --create-room NAME   open a room of this ROM (this side starts the match), or\n"
        "  --join-room [NAME]   join the first room of this ROM (or the one with that name)\n"
        "  --room-password PW   the password for the room opened or joined (default: none)\n"
        "  --max-players N      the room's seats (2-4), --start-players N start once N are seated (default 2)\n"
        "  --ice-bind ADDR      the ICE agents' address (default 127.0.0.1; every interface with --bind-any)\n"
        "  --stun off|server|H:P   STUN for ICE (default off: nothing leaves the machine)\n"
        "  --manifest-cheat V, --manifest-save   host: a test cheat (byte V at 0x80700000) / SRAM save in the manifest\n"
        "  In a lobby match --player defaults to this side's seat; --delay 0 means Auto (RMG-K's default).\n"
        "  Nothing is sent anywhere but the given peer and the servers named on the command line.\n");
    std::exit(2);
}

#ifdef ZNX_STATIC_LIBS
#define RTLD_NOW 0
#define RTLD_GLOBAL 0
#endif

void* load(const char* dir, const char* name, int flags)
{
#ifdef ZNX_STATIC_LIBS
    (void)dir;
    (void)flags;
    static const char* const names[][2] = { { "libmupen64plus.dylib", "core" }, { "znx-video-null.dylib", "video_null" },
        { "mupen64plus-rsp-hle.dylib", "rsp_hle" }, { "znx-input-net.dylib", "input_net" } };
    for (const auto& n : names)
        if (std::strcmp(n[0], name) == 0)
            return znx_static_lib(n[1]);
    return nullptr;
#else
    std::string path = std::string(dir) + "/" + name;
    void* lib = dlopen(path.c_str(), flags);
    if (lib == nullptr)
        std::fprintf(stderr, "znx-net: %s\n", dlerror());
    return lib;
#endif
}

void unload(void* lib)
{
#ifndef ZNX_STATIC_LIBS
    dlclose(lib);
#else
    (void)lib;
#endif
}

void print_line(int is_error, const char* line)
{
    std::fprintf(is_error ? stderr : stdout, "%s\n", line);
}

bool is_loopback(const std::string& target)
{
    char code[32];
    if (znx_p2p_normalize_code(target.c_str(), code, sizeof(code)))
        return true;   /* found through the --nat server; the address comes from there */
    const std::string ip = target.substr(0, target.rfind(':'));
    return ip == "127.0.0.1" || ip == "localhost";
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (!std::strcmp(a, "--rom")) g_opt.rom = next();
        else if (!std::strcmp(a, "--host")) { g_opt.host = true; g_opt.port = std::atoi(next()); }
        else if (!std::strcmp(a, "--join")) g_opt.join = next();
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
        else if (!std::strcmp(a, "--bind-any")) g_opt.bind_any = true;
        else if (!std::strcmp(a, "--nat")) g_opt.nat = next();
        else if (!std::strcmp(a, "--list")) g_opt.list = next();
        else if (!std::strcmp(a, "--enlist")) g_opt.enlist = true;
        else if (!std::strcmp(a, "--code-file")) g_opt.code_file = next();
        else if (!std::strcmp(a, "--no-pace")) g_opt.pace = false;
        else if (!std::strcmp(a, "--name")) g_opt.name = next();
        else if (!std::strcmp(a, "--app")) g_opt.app = next();
        else if (!std::strcmp(a, "--lobby-timeout")) g_opt.lobby_timeout_s = std::atoi(next());
        else if (!std::strcmp(a, "--ready-after")) g_opt.ready_after_s = std::atoi(next());
        else if (!std::strcmp(a, "--gekko-log")) g_opt.gekko_log = true;
        else if (!std::strcmp(a, "--precompile-ms") && i + 1 < argc) g_opt.precompile_ms = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--no-warm")) g_opt.no_warm = true;
        else if (!std::strcmp(a, "--warm") && i + 1 < argc) g_opt.warm = argv[++i];
        else if (!std::strcmp(a, "--verbose")) g_opt.verbose = true;
        else if (!std::strcmp(a, "--fetch-list")) return fetch_list(next());
        else if (!std::strcmp(a, "--lobby")) g_opt.lobby = next();
        else if (!std::strcmp(a, "--create-room")) g_opt.create_room = next();
        else if (!std::strcmp(a, "--room-password")) g_opt.room_password = next();
        else if (!std::strcmp(a, "--join-room")) {
            g_opt.join_room = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                g_opt.join_room_name = argv[++i];
        }
        else if (!std::strcmp(a, "--max-players")) g_opt.max_players = std::atoi(next());
        else if (!std::strcmp(a, "--start-players")) g_opt.start_players = std::atoi(next());
        else if (!std::strcmp(a, "--ice-bind")) g_opt.ice_bind = next();
        else if (!std::strcmp(a, "--stun")) g_opt.stun = next();
        else if (!std::strcmp(a, "--manifest-cheat")) g_opt.manifest_cheat = std::atoi(next()) & 0xff;
        else if (!std::strcmp(a, "--manifest-save")) g_opt.manifest_save = true;
        else usage();
    }
    const bool lobby = !g_opt.lobby.empty();
    if (lobby) {
        if (g_opt.create_room.empty() == !g_opt.join_room || g_opt.rom == nullptr || g_opt.frames <= 0 ||
            g_opt.check_every <= 0)
            usage();
        g_opt.host = !g_opt.create_room.empty();
        const std::string where = g_opt.lobby.substr(0, g_opt.lobby.find('/', 5));
        if (!g_opt.bind_any && where.find("127.0.0.1") == std::string::npos && where.find("localhost") == std::string::npos) {
            std::fprintf(stderr, "znx-net: without --bind-any the lobby must be on 127.0.0.1\n");
            return 2;
        }
    } else {
        if (g_opt.rom == nullptr || (g_opt.host ? (g_opt.port <= 0 || g_opt.port > 65535) : g_opt.join.empty()) ||
            g_opt.frames <= 0 || g_opt.check_every <= 0)
            usage();
        if (!g_opt.host && !g_opt.bind_any && !is_loopback(g_opt.join)) {
            std::fprintf(stderr, "znx-net: without --bind-any this only joins 127.0.0.1\n");
            return 2;
        }
    }
    if (g_opt.inputs != nullptr && (lobby ? (g_opt.player < 0 || g_opt.player > 4) : (g_opt.player < 1 || g_opt.player > 2)))
        usage();
    setvbuf(stdout, nullptr, _IOLBF, 0);

    char exe[4096], dir[4096];
#ifdef ZNX_STATIC_LIBS
    const ssize_t exe_length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exe_length <= 0 || (exe[exe_length] = 0, realpath(exe, dir)) == nullptr) {
#else
    uint32_t exe_size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &exe_size) != 0 || realpath(exe, dir) == nullptr) {
#endif
        std::fprintf(stderr, "znx-net: cannot find my own path\n");
        return 1;
    }
    std::strcpy(dir, dirname(dir));
    const std::string data_dir = std::string(dir) + "/data";
    const std::string user_dir = std::string(dir) + "/profile/net-" + (lobby ? "room-" : "") + (g_opt.host ? "host" : "join");
    mkdir((std::string(dir) + "/profile").c_str(), 0755);
    mkdir(user_dir.c_str(), 0755);
    g_log_dir = user_dir;

    if (!lobby && g_opt.inputs != nullptr && !load_script(g_opt.inputs, g_opt.player))
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
    sim.loopback_only = g_opt.bind_any ? 0 : 1;
    if (lobby) {
        /* the same latency, jitter and loss on GekkoNet's datagrams, added where the session hands them to
         * the ICE mesh (net/lobby/lobby_ice_rmgk.cpp) */
        znx_lobby_rmgk_netsim(sim.latency_ms, sim.jitter_ms, sim.loss_pct, sim.seed);
        if (!run_lobby_room()) {
            znx_lobby_play_close();
            znx_lobby_rmgk_netsim_shutdown();
            return 1;
        }
    } else {
        znx_netsim_configure(&sim);
        if (!run_lobby()) {
            znx_p2p_close();
            znx_netsim_shutdown();
            return 1;
        }
    }

    struct znx_options o;
    znx_options_defaults(&o);
    o.rom_path = g_opt.rom;
    o.frames = 0;            /* the session decides when to stop */
    o.hash_every = 0;
    o.input_players = lobby ? znx_lobby_play_input_players() : 2;   /* what start_*_session sets too */
    o.verbose = g_opt.verbose ? 1 : 0;
    o.precompile_ms = g_opt.precompile_ms;
    o.warm_off = g_opt.no_warm ? 1 : 0;
    o.warm_path = g_opt.warm;

    struct znx_host host;
    std::memset(&host, 0, sizeof(host));
    host.name = "znx-net";
    host.core = load(dir, "libmupen64plus.dylib", RTLD_NOW | RTLD_GLOBAL);
    host.video = load(dir, "znx-video-null.dylib", RTLD_NOW);
    host.rsp = load(dir, "mupen64plus-rsp-hle.dylib", RTLD_NOW);
    host.input = load(dir, "znx-input-net.dylib", RTLD_NOW);
    if (host.core == nullptr || host.video == nullptr || host.rsp == nullptr || host.input == nullptr)
        return 1;
    auto set_source = (void (*)(uint32_t (*)(int)))lib_sym(host.input, "znx_input_set_source");
    if (set_source == nullptr)
        return 1;
    set_source(input_source);
    g_core = host.core;
    host.sym = lib_sym;
    host.print = print_line;
    host.data_dir = data_dir.c_str();
    host.user_dir = user_dir.c_str();
    const std::string warm_dir = data_dir + "/warm";   /* the shipped warm lists (build-mac.sh copies assets/warm) */
    host.warm_dir = warm_dir.c_str();
    host.session_run = run_session;
    if (lobby)
        host.session_prepare = prepare_core;

    int result = znx_bench_run(&host, &o);

    if (lobby) {
        char title[160], text[512];
        if (!znx_lobby_play_after_game(title, sizeof(title), text, sizeof(text)))
            note("after the match: %s: %s", title, text);
        else
            note("after the match: back in the room");
        znx_lobby_play_leave_room();
        for (int i = 0; i < 50; ++i) {   /* let ROOM_LEAVE go out */
            znx_lobby_play_poll();
            usleep(2000);
        }
        znx_lobby_play_close();
        znx_lobby_rmgk_netsim_shutdown();
    } else {
        znx_p2p_close();
        znx_netsim_shutdown();
    }
    unload(host.input);
    unload(host.rsp);
    unload(host.video);
    unload(host.core);
    return result;
}
