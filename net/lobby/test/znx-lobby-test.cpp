// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-lobby-test: two lobby clients in one process against the local mock server
 * (net/lobby/test/mock_lobby.py), everything on 127.0.0.1:
 *   connect both, A creates a room, B finds it in the room list and joins, room chat, the ICE
 *   mesh comes up through the server's ICE_SIGNAL relay (host candidates on 127.0.0.1, no STUN,
 *   no TURN), pings over ICE, A starts the match, MATCH_BEGIN, pre-match sync (a manifest large
 *   enough to be fragmented), datagrams both ways, then a GekkoNet rollback session between the
 *   two over the lobby's GekkoNet adapter with a small deterministic stand-in game, whose state
 *   checksums are compared frame by frame ("check" lines), then match end and leaving.
 *
 *   znx-lobby-test [--mock net/lobby/test/mock_lobby.py] [--port 18080] [--frames 1200] [--verbose]
 *   znx-lobby-test --server ws://127.0.0.1:PORT/ws     (a server that is already running)
 * Exit status 0 when every step passed. */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "gekkonet.h"
#include "znx_lobby.h"

namespace
{
bool g_verbose = false;
int g_failures = 0;
pid_t g_mock = -1;

void say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void check(bool ok, const char* what)
{
    say("%s %s", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void logLine(void* user, const char* line)
{
    if (g_verbose)
        std::fprintf(stderr, "[%s] %s\n", static_cast<const char*>(user), line);
}

struct Client
{
    const char* name;
    znx_lobby* lb = nullptr;
    uint64_t id = 0;
    uint64_t room = 0;
    bool connected = false;
    bool roomListHasRoom = false;
    uint64_t seenRoomInList = 0;
    std::string lastChat;
    bool iceUp = false;
    int ping = -1;
    bool matchBegin = false;
    bool matchReady = false;
    std::string matchFailed;
    std::string roomLeft;
    bool left = false;
    int roomStates = 0;
};

void handle(Client& c, const znx_lobby_event& ev)
{
    switch (ev.type)
    {
    case ZNX_LOBBY_EV_CONNECTED: c.connected = true; c.id = ev.user_id; break;
    case ZNX_LOBBY_EV_HELLO_FAILED: say("%s: HELLO failed: %s", c.name, ev.text); break;
    case ZNX_LOBBY_EV_DISCONNECTED: c.connected = false; say("%s: disconnected: %s", c.name, ev.text); break;
    case ZNX_LOBBY_EV_ROOM_LIST:
        for (int i = 0; i < znx_lobby_room_count(c.lb); ++i)
        {
            znx_lobby_room_info r;
            if (znx_lobby_room(c.lb, i, &r) == 0 && r.id != 0)
                c.seenRoomInList = r.id;
        }
        break;
    case ZNX_LOBBY_EV_ROOM_CREATED:
    case ZNX_LOBBY_EV_ROOM_JOINED: c.room = ev.room_id; break;
    case ZNX_LOBBY_EV_ROOM_STATE: ++c.roomStates; break;
    case ZNX_LOBBY_EV_ROOM_LEFT: c.left = true; c.room = 0; c.roomLeft = ev.text; break;
    case ZNX_LOBBY_EV_CHAT: c.lastChat = std::string(ev.channel) + "|" + ev.name + "|" + ev.text; break;
    case ZNX_LOBBY_EV_PEER_ICE:
        if (ev.value == 1) c.iceUp = true;
        if (g_verbose) std::fprintf(stderr, "[%s] peer %llu ICE %d (attempt %d)\n", c.name,
                                    (unsigned long long)ev.user_id, ev.value, ev.value2);
        break;
    case ZNX_LOBBY_EV_PEER_PING: c.ping = ev.value; break;
    case ZNX_LOBBY_EV_MATCH_BEGIN: c.matchBegin = true; break;
    case ZNX_LOBBY_EV_MATCH_READY: c.matchReady = true; break;
    case ZNX_LOBBY_EV_MATCH_FAILED: c.matchFailed = ev.text; say("%s: match failed: %s", c.name, ev.text); break;
    default: break;
    }
}

void pumpOnce(Client& a, Client& b)
{
    znx_lobby_event ev;
    while (znx_lobby_poll(a.lb, &ev)) handle(a, ev);
    while (znx_lobby_poll(b.lb, &ev)) handle(b, ev);
}

bool pumpUntil(Client& a, Client& b, int timeoutMs, const std::function<bool()>& done)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end)
    {
        pumpOnce(a, b);
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}

bool startMock(const char* script, int port)
{
    int fds[2];
    if (pipe(fds) != 0)
        return false;
    g_mock = fork();
    if (g_mock == 0)
    {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        const std::string p = std::to_string(port);
        if (g_verbose)
            execlp("python3", "python3", "-I", script, "--port", p.c_str(), "--verbose", (char*)nullptr);
        else
            execlp("python3", "python3", "-I", script, "--port", p.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    char buf[64] = {};
    size_t got = 0;
    while (got < 5)
    {
        const ssize_t n = read(fds[0], buf + got, sizeof(buf) - 1 - got);
        if (n <= 0)
            break;
        got += size_t(n);
    }
    close(fds[0]);
    return std::strncmp(buf, "READY", 5) == 0;
}

void stopMock()
{
    if (g_mock > 0)
    {
        kill(g_mock, SIGTERM);
        waitpid(g_mock, nullptr, 0);
        g_mock = -1;
    }
}

/* ---- the stand-in game for the GekkoNet session ---- */
struct Game   // no padding: the checksum covers every byte
{
    uint64_t hash = 0x9e3779b97f4a7c15ull;
    uint32_t frame = 0;
    uint32_t p1 = 0, p2 = 0;   // "positions" moved by the inputs
    uint32_t spare = 0;
};
static_assert(sizeof(Game) == 24, "Game must have no padding");

uint32_t checksum(const Game& g)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&g);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(g); ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

uint32_t scriptedInput(int player, int frame)
{
    // Changes every few frames so prediction (repeat the last input) is often wrong.
    uint32_t x = uint32_t(frame / (player == 1 ? 7 : 5)) * 2654435761u + uint32_t(player) * 40503u;
    x ^= x >> 13;
    return x & 0xffffu;
}

struct Side
{
    const char* name;
    znx_lobby* lb;
    GekkoSession* s = nullptr;
    int localHandle = -1;
    int localSlot = 0;
    Game g;
    std::vector<uint32_t> after;   // checksum of the state after frame f (last simulation wins)
    int loads = 0, resims = 0, advances = 0;
    bool desync = false;
    bool synced = false;
};

bool setupSide(Side& side, const znx_lobby_match_info& m, int frames)
{
    void* adapter = znx_lobby_gekko_adapter(side.lb, 3);
    if (!adapter || !gekko_create(&side.s, GekkoGameSession))
        return false;
    GekkoConfig cfg{};
    cfg.num_players = static_cast<unsigned char>(m.num_peers + 1);
    cfg.max_spectators = 0;
    cfg.input_prediction_window = static_cast<unsigned char>(m.prediction);
    cfg.input_size = sizeof(uint32_t);
    cfg.state_size = sizeof(Game);
    cfg.limited_saving = false;
    cfg.desync_detection = true;
    cfg.check_distance = 10;
    gekko_start(side.s, &cfg);
    gekko_net_adapter_set(side.s, static_cast<GekkoNetAdapter*>(adapter));
    gekko_set_runahead(side.s, 0);
    side.localSlot = m.local_slot;
    side.after.assign(size_t(frames) + 64, 0);
    // One actor per occupied seat, in seat order, as add_lobby_actors does (rmgk_gekko.cpp:2441-2537).
    std::vector<std::string> addrs(4);
    for (int seat = 1; seat <= m.total_seats; ++seat)
    {
        if (seat == m.local_slot)
        {
            side.localHandle = gekko_add_actor(side.s, GekkoLocalPlayer, nullptr);
            // Delay 0 instead of the lobby's (1 on loopback): every remote input then arrives after
            // the frame that needs it, so prediction and rollback are always exercised.
            gekko_set_local_delay(side.s, side.localHandle, 0);
            continue;
        }
        for (int i = 0; i < m.num_peers; ++i)
        {
            if (m.peers[i].slot != seat)
                continue;
            char text[32];
            znx_lobby_peer_address(m.peers[i].user_id, text, sizeof(text));
            addrs[size_t(seat - 1)] = text;
            GekkoNetAddress a{};
            a.data = addrs[size_t(seat - 1)].data();
            a.size = unsigned(addrs[size_t(seat - 1)].size());
            if (gekko_add_actor(side.s, GekkoRemotePlayer, &a) < 0)
                return false;
        }
    }
    gekko_set_disconnect_timeout(side.s, 30000);
    return side.localHandle >= 0;
}

void stepSide(Side& side, int frames)
{
    gekko_network_poll(side.s);
    int n = 0;
    GekkoSessionEvent** sev = gekko_session_events(side.s, &n);
    for (int i = 0; i < n; ++i)
    {
        if (sev[i]->type == GekkoSessionStarted)
            side.synced = true;
        else if (sev[i]->type == GekkoDesyncDetected)
        {
            side.desync = true;
            say("%s: GekkoNet desync at frame %d (local %08x remote %08x)", side.name,
                sev[i]->data.desynced.frame, sev[i]->data.desynced.local_checksum,
                sev[i]->data.desynced.remote_checksum);
        }
        else if (sev[i]->type == GekkoPlayerDisconnected)
            say("%s: GekkoNet player %d disconnected", side.name, sev[i]->data.disconnected.handle);
    }
    // Keep the two in step the way RMG-K's pacer does, coarsely: the side ahead waits.
    if (side.synced && gekko_frames_ahead(side.s) >= 2.0f)
        return;
    if (int(side.g.frame) >= frames + 30)
        return;   // ran far enough; keep polling so the other side can confirm
    uint32_t input = scriptedInput(side.localSlot, gekko_current_frame(side.s) + 1);
    gekko_add_local_input(side.s, side.localHandle, &input);
    GekkoGameEvent** ev = gekko_update_session(side.s, &n);
    for (int i = 0; i < n; ++i)
    {
        GekkoGameEvent* e = ev[i];
        switch (e->type)
        {
        case GekkoAdvanceEvent:
        {
            const auto* in = reinterpret_cast<const uint32_t*>(e->data.adv.inputs);
            side.g.frame = uint32_t(e->data.adv.frame);
            side.g.p1 = side.g.p1 * 3 + in[0];
            side.g.p2 = side.g.p2 * 5 + in[1];
            side.g.hash = (side.g.hash ^ (uint64_t(in[0]) << 32 | in[1])) * 0x100000001b3ull + side.g.frame;
            if (e->data.adv.rolling_back)
                ++side.resims;
            else
                ++side.advances;
            if (e->data.adv.frame >= 0 && size_t(e->data.adv.frame) < side.after.size())
                side.after[size_t(e->data.adv.frame)] = checksum(side.g);
            break;
        }
        case GekkoSaveEvent:
            *e->data.save.state_len = sizeof(Game);
            *e->data.save.checksum = checksum(side.g);
            std::memcpy(e->data.save.state, &side.g, sizeof(Game));
            break;
        case GekkoLoadEvent:
            std::memcpy(&side.g, e->data.load.state, sizeof(Game));
            ++side.loads;
            break;
        default: break;
        }
    }
}

bool runGekko(Client& a, Client& b, int frames)
{
    znx_lobby_match_info ma, mb;
    if (znx_lobby_match(a.lb, &ma) != 0 || znx_lobby_match(b.lb, &mb) != 0)
        return false;
    Side sa{"A", a.lb}, sb{"B", b.lb};
    if (!setupSide(sa, ma, frames) || !setupSide(sb, mb, frames))
    {
        say("GekkoNet setup failed");
        return false;
    }
    znx_lobby_match_started(a.lb);
    znx_lobby_match_started(b.lb);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < end)
    {
        stepSide(sa, frames);
        stepSide(sb, frames);
        pumpOnce(a, b);   // the lobby keeps polling during the match (signalling, pings)
        if (gekko_confirmed_frame(sa.s) >= frames && gekko_confirmed_frame(sb.s) >= frames)
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const int confirmedA = gekko_confirmed_frame(sa.s), confirmedB = gekko_confirmed_frame(sb.s);
    say("GekkoNet: A confirmed %d (advances %d, re-simulated %d, loads %d); B confirmed %d (advances %d, "
        "re-simulated %d, loads %d)", confirmedA, sa.advances, sa.resims, sa.loads, confirmedB, sb.advances,
        sb.resims, sb.loads);
    bool same = confirmedA >= frames && confirmedB >= frames;
    int mismatches = 0;
    for (int f = 0; f < frames && same; ++f)
    {
        if (sa.after[size_t(f)] != sb.after[size_t(f)])
        {
            if (++mismatches <= 5)
                say("mismatch at frame %d: %08x vs %08x", f, sa.after[size_t(f)], sb.after[size_t(f)]);
        }
        if ((f + 1) % 60 == 0)
            say("check %d A %08x B %08x", f, sa.after[size_t(f)], sb.after[size_t(f)]);
    }
    check(same && mismatches == 0 && !sa.desync && !sb.desync,
          "GekkoNet session over the lobby transport: identical state every confirmed frame, no desync");
    check(sa.loads + sb.loads > 0, "the session rolled back at least once (prediction was exercised)");
    gekko_destroy(&sa.s);
    gekko_destroy(&sb.s);
    znx_lobby_gekko_adapter_release(a.lb);
    znx_lobby_gekko_adapter_release(b.lb);
    return same && mismatches == 0;
}

std::string bigManifest()
{
    // A manifest with a 6000-byte save section so the host has to fragment it (1000-byte pieces,
    // LobbyClient.cpp:443-471). The bytes are a pattern, not a real compressed save.
    auto le = [](std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xff)); };
    std::string cheats("RMGKCHT1", 8);
    le(cheats, 0);
    std::string m("RMGKPMAN", 8);
    le(m, 1);
    le(m, 3);
    le(m, uint32_t(cheats.size()));
    m += cheats;
    le(m, 6000);
    for (int i = 0; i < 6000; ++i)
        m.push_back(char((i * 7 + 3) & 0xff));
    return m;
}
} // namespace

int main(int argc, char** argv)
{
    const char* mock = "net/lobby/test/mock_lobby.py";
    std::string server;
    int port = 18080;
    int frames = 1200;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--mock") && i + 1 < argc) mock = argv[++i];
        else if (!std::strcmp(argv[i], "--server") && i + 1 < argc) server = argv[++i];
        else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--verbose")) g_verbose = true;
        else
        {
            std::fprintf(stderr, "usage: %s [--mock mock_lobby.py] [--port N] [--server ws://127.0.0.1:N/ws] "
                                 "[--frames N] [--verbose]\n", argv[0]);
            return 2;
        }
    }
    if (server.empty())
    {
        if (!startMock(mock, port))
        {
            std::fprintf(stderr, "could not start the mock server (%s)\n", mock);
            stopMock();
            return 1;
        }
        server = "ws://127.0.0.1:" + std::to_string(port) + "/ws";
    }
    if (server.rfind("ws://127.0.0.1:", 0) != 0)
    {
        std::fprintf(stderr, "refusing a server other than 127.0.0.1\n");
        stopMock();
        return 2;
    }

    // Loopback only: no STUN, no TURN, ICE agents bound to 127.0.0.1.
    znx_lobby_config cfg{};
    cfg.stun_host = "";
    cfg.use_server_turn = 0;
    cfg.bind_address = "127.0.0.1";
    cfg.connection = "lan";
    cfg.log = logLine;
    Client a{"alice"}, b{"bob"};
    cfg.log_user = const_cast<char*>("A");
    a.lb = znx_lobby_create(&cfg);
    cfg.log_user = const_cast<char*>("B");
    b.lb = znx_lobby_create(&cfg);
    const char* md5 = "F7C52568A31AADF26E14DC2B6416B2ED";

    check(znx_lobby_connect(a.lb, server.c_str(), "alice", md5, nullptr) == 0 &&
          znx_lobby_connect(b.lb, server.c_str(), "bob", md5, nullptr) == 0, "connect started");
    check(pumpUntil(a, b, 5000, [&] { return a.connected && b.connected; }),
          "WebSocket upgrade, HELLO -> HELLO_OK for both (user ids assigned)");
    say("alice is user %llu, bob is user %llu", (unsigned long long)a.id, (unsigned long long)b.id);
    pumpUntil(a, b, 300, [&] { return znx_lobby_user_count(a.lb) == 2; });
    check(znx_lobby_user_count(a.lb) == 2 && znx_lobby_user_count(b.lb) == 2, "presence: both see two users");

    znx_lobby_create_room(a.lb, "znx test", "Super Smash Bros. (U)", md5, "USA", 2, 2, 7, nullptr);
    check(pumpUntil(a, b, 3000, [&] { return a.room != 0 && b.seenRoomInList == a.room; }),
          "ROOM_CREATE -> ROOM_CREATED for A, ROOM_LIST shows it to B");
    znx_lobby_join_room(b.lb, b.seenRoomInList, nullptr);
    check(pumpUntil(a, b, 3000, [&] { return b.room == a.room && b.room != 0; }), "B: ROOM_JOIN -> ROOM_JOIN_OK");

    znx_lobby_send_chat(a.lb, "room", "hello from the Switch side");
    check(pumpUntil(a, b, 3000, [&] { return b.lastChat == "room|alice|hello from the Switch side"; }),
          "room chat A -> B (CHAT_SEND -> CHAT_MSG)");

    check(pumpUntil(a, b, 15000, [&] { return a.iceUp && b.iceUp; }),
          "ICE: both agents connected through the server's ICE_SIGNAL relay (127.0.0.1 host candidates)");
    check(pumpUntil(a, b, 8000, [&] { return a.ping >= 0 && b.ping >= 0; }), "ping over the ICE path, both ways");
    say("ping A->B %d ms, B->A %d ms", a.ping, b.ping);
    check(pumpUntil(a, b, 3000, [&] {
              znx_lobby_room_detail d;
              return znx_lobby_current_room(a.lb, &d) == 0 && d.seats[1].frame_delay >= 0 && d.seats[1].ping_ms >= 0;
          }), "ROOM_STATE carries B's published delay; A's seat view has B's ping");
    check(pumpUntil(a, b, 3000, [&] { return znx_lobby_can_start(a.lb) != 0; }),
          "host may start (RMG-K's rule: 2 seated, every path measured)");
    check(!znx_lobby_can_start(b.lb), "non-host may not start");

    const std::string manifest = bigManifest();
    check(znx_lobby_set_prematch_manifest(a.lb, reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()) == 0,
          "host manifest accepted (6 kB, fragmented on the wire)");
    znx_lobby_start_match(a.lb);
    check(pumpUntil(a, b, 3000, [&] { return a.matchBegin && b.matchBegin; }), "ROOM_START -> MATCH_BEGIN for both");
    check(pumpUntil(a, b, 12000, [&] { return (a.matchReady && b.matchReady) || !a.matchFailed.empty() ||
                                              !b.matchFailed.empty(); }) && a.matchReady && b.matchReady,
          "pre-match sync over ICE (manifest fragments, hash, ACKs) -> MATCH_READY for both");

    znx_lobby_match_info ma{}, mb{};
    const bool haveA = znx_lobby_match(a.lb, &ma) == 0, haveB = znx_lobby_match(b.lb, &mb) == 0;
    check(haveA && haveB && ma.match_id == mb.match_id && ma.local_slot == 1 && mb.local_slot == 2 &&
          ma.num_peers == 1 && mb.num_peers == 1 && ma.peers[0].user_id == b.id && mb.peers[0].user_id == a.id &&
          ma.total_seats == 2 && mb.total_seats == 2, "match info: same match, A is P1, B is P2, peers right");
    check(haveA && haveB && ma.manifest_size == manifest.size() && mb.manifest_size == manifest.size() &&
          ma.manifest_hash == mb.manifest_hash &&
          std::memcmp(mb.manifest, manifest.data(), manifest.size()) == 0,
          "B received the host's manifest byte for byte");
    say("match %llu: delay A %d B %d, prediction %d, manifest %zu bytes hash %016llx",
        (unsigned long long)ma.match_id, ma.local_delay, mb.local_delay, ma.prediction, ma.manifest_size,
        (unsigned long long)ma.manifest_hash);

    // Raw datagrams both ways on a GekkoNet epoch channel (0x10), 200 each.
    int gotA = 0, gotB = 0, sent = 0;
    for (int i = 0; i < 200; ++i)
    {
        char msg[64];
        const int n = std::snprintf(msg, sizeof(msg), "A->B %d", i);
        sent += znx_lobby_send(a.lb, b.id, 0x10, msg, size_t(n)) == 0;
        const int m = std::snprintf(msg, sizeof(msg), "B->A %d", i);
        sent += znx_lobby_send(b.lb, a.id, 0x10, msg, size_t(m)) == 0;
    }
    pumpUntil(a, b, 3000, [&] {
        char buf[128];
        uint64_t from = 0;
        int n;
        while ((n = znx_lobby_receive(b.lb, 0x10, &from, buf, sizeof(buf))) >= 0)
            gotB += (from == a.id && n >= 4 && std::memcmp(buf, "A->B", 4) == 0);
        while ((n = znx_lobby_receive(a.lb, 0x10, &from, buf, sizeof(buf))) >= 0)
            gotA += (from == b.id && n >= 4 && std::memcmp(buf, "B->A", 4) == 0);
        return gotA == 200 && gotB == 200;
    });
    say("datagrams: sent %d, B received %d from A, A received %d from B", sent, gotB, gotA);
    check(sent == 400 && gotA == 200 && gotB == 200, "datagrams both ways over the ICE path (loopback: no loss)");

    if (frames > 0)
        runGekko(a, b, frames);

    znx_lobby_match_finished(a.lb);
    znx_lobby_match_finished(b.lb);
    const int statesBefore = b.roomStates;
    check(pumpUntil(a, b, 3000, [&] {
              znx_lobby_room_detail d;
              return b.roomStates > statesBefore && znx_lobby_current_room(b.lb, &d) == 0 &&
                     std::strcmp(d.state, "waiting") == 0;
          }), "MATCH_FINISHED -> room back to waiting");
    znx_lobby_leave_room(b.lb);
    check(pumpUntil(a, b, 3000, [&] { return b.left; }), "B: ROOM_LEAVE -> ROOM_LEFT");

    znx_lobby_disconnect(a.lb);
    znx_lobby_disconnect(b.lb);
    pumpUntil(a, b, 200, [] { return false; });
    znx_lobby_destroy(a.lb);
    znx_lobby_destroy(b.lb);
    stopMock();
    say("%s: %d failure(s)", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
