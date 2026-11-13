// SPDX-License-Identifier: GPL-3.0-or-later
/* RMG-K's "Peer to Peer" rollback netplay, the part RMG-K keeps in its Qt dialog
 * (RMG/UserInterface/Dialog/Kaillera/KailleraP2PDialog.cpp, KailleraNetplayDialog.cpp,
 * KailleraWaitingGamesDialog.cpp), rewritten without Qt, plus the session that follows it.
 * Shared by the Switch program and by znx-net on the Mac (and its Linux self-test), so the code
 * tested on the Mac is the code the console runs.
 *
 * What is on the wire is RMG-K's own code, compiled unchanged: the n02 P2P client (lobby,
 * chat, ready, the tunnel GekkoNet's datagrams travel in), GekkoNet and the session driver
 * rmgk_gekko.cpp. This file drives them the way the dialog does:
 *   - host: n02 on UDP 27886 (or another port), a connect code from RMG-K's NAT traversal
 *     server (N02TRAV1|CLAIM/CLAIMOK/CLAIMACK, HOSTOPEN/HOSTOK, HOSTKEEP, HOSTCLOSE, PEER,
 *     PUNCH), optionally the public list (ENLISP/ENLIST/UNENLIST, WHATISMYIP);
 *   - join: by connect code (JOIN -> HOST, PUNCH, then the n02 login) or by IP[:port];
 *   - before the match: LOGN, the RMGK: chat settings (MODE:RB, RBDELAY, RBPRED, RBPACE), the
 *     delay chosen from the ping as RMG-K chooses it, ready, LOAD;
 *   - the match: start_p2p_session with the host as player 1 and the joiner as player 2,
 *     rmgk_gekko::execute(), n02 DROP to leave, the peer's DROP/EXIT or GekkoNet's timeout to end.
 *
 * Everything runs on one lobby thread that polls the n02 socket every millisecond (the dialog's
 * 1 ms step timer) and does the dialog's once-a-second housekeeping; the caller only reads a
 * status snapshot and sets a few things. The match itself runs on the caller's emulation thread
 * (znx_p2p_run_session) while the lobby thread keeps polling n02 for CHAT/DROP/EXIT, as the
 * dialog's timer does with p2p_rollback_process_control. */
#ifndef ZNX_P2P_H
#define ZNX_P2P_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "RMG-K " + CORE_VERSION (RMG/UserInterface/MainWindow.cpp; the git tag): what RMG-K v0.9.16 sends,
 * so its players see no "Emulator/version difference" warning. */
#define ZNX_P2P_APP "RMG-K v0.9.16"
#define ZNX_P2P_DEFAULT_PORT 27886
#define ZNX_P2P_NAT_HOST "nat.smash64.net"       /* KailleraTraversalConfig.hpp */
#define ZNX_P2P_NAT_PORT 6364
#define ZNX_P2P_LIST_HOST "p2plist.smash64.net"  /* KailleraP2PDialog.cpp kSsrvHost */
#define ZNX_P2P_LIST_PORT 27887

struct znx_p2p_config {
    int host;                   /* 1 host (player 1), 0 join (player 2) */
    int port;                   /* host: UDP port to listen on (0 = 27886); join: ignored (ephemeral) */
    const char* game;           /* our ROM's GoodName (RMG-K's ROM database), what the host announces */
    const char* username;       /* up to 31 bytes */
    const char* app;            /* NULL: ZNX_P2P_APP */
    const char* join_target;    /* join: a connect code ("ABC@12") or "ip[:port]" (27886 when no port) */
    int delay;                  /* host: 0 = automatic from the ping (RMG-K's default mode), 1..9 fixed */
    int prediction;             /* host: 0 = RMG-K's default window (7) */
    int use_codes;              /* host: get a connect code from the traversal server */
    int enlist;                 /* host: put the game on the public list */
    const char* nat_host;       /* NULL: ZNX_P2P_NAT_HOST; a test points these at a local mock server */
    int nat_port;
    const char* list_host;      /* NULL: ZNX_P2P_LIST_HOST */
    int list_port;
    const char* code;           /* host: the saved connect code and its owner token ("" or NULL: none) */
    const char* token;
    /* Called (on the lobby thread) when the traversal server gives this host a code to keep, or
     * says the saved one is no longer valid (code and token ""). RMG-K saves them the same way
     * (Kaillera_P2PStaticCode / Kaillera_P2PStaticCodeOwnerToken). */
    void (*save_identity)(const char* code, const char* token, void* user);
    /* Diagnostics, one line each (any thread). */
    void (*log)(const char* line, void* user);
    void* user;
};

enum znx_p2p_phase {
    ZNX_P2P_CLOSED,       /* nothing open */
    ZNX_P2P_WAITING,      /* host: waiting for a player */
    ZNX_P2P_CONNECTING,   /* join: finding the host / logging in */
    ZNX_P2P_LOBBY,        /* both connected, before the match */
    ZNX_P2P_STARTING,     /* both ready: the match is starting (start the emulation, then run the session) */
    ZNX_P2P_IN_GAME,      /* the session runs */
    ZNX_P2P_FAILED,       /* could not connect, or the peer left: message says why */
};

struct znx_p2p_status {
    int phase;                  /* enum znx_p2p_phase */
    int host;
    int port;                   /* our n02 port */
    char code[32];              /* host: the connect code, "" until the traversal server gives one */
    int code_state;             /* host: 0 asking, 1 code live, 2 no code (traversal unreachable: by IP only) */
    char public_address[64];    /* host: ip:port as the list server sees it (WHATISMYIP), or "" */
    char peer_name[32];
    char peer_app[128];         /* the peer's app string as it sent it (host side), or "" */
    char host_game[128];        /* join: the game the host announced */
    int game_mismatch;          /* join: the host plays another game than ours */
    int not_rollback;           /* join: the host is in RMG-K's delay-based mode */
    int app_mismatch;           /* the peer's app string differs from ours (RMG-K warns the same way) */
    int ping;                   /* ms, -1 unknown */
    int delay;                  /* the frame delay the match will use (join: the host's) */
    int delay_auto;             /* the host chose it from the ping */
    int prediction;
    int local_ready, peer_ready;
    int can_ready;              /* join: false until the host's game and settings are known and match */
    int enlisted;               /* host: on the public list */
    int player;                 /* 1 or 2 once the match starts */
    char message[256];          /* FAILED: why; otherwise a status line ("" when there is nothing to say) */
};

/* Opens the n02 socket and starts the lobby thread. 0 on success; otherwise error says why. */
int znx_p2p_open(const struct znx_p2p_config* config, char* error, size_t error_size);
/* Leaves: UNENLIST, HOSTCLOSE, n02 EXIT, closes the socket and stops the thread. Safe to call twice. */
void znx_p2p_close(void);
int znx_p2p_is_open(void);
void znx_p2p_status(struct znx_p2p_status* status);
/* The player presses Ready (or takes it back). */
void znx_p2p_set_ready(int ready);
/* Host: change the delay setting in the lobby (0 = automatic). */
void znx_p2p_set_delay(int delay);

/* ---- the match ---- */

struct znx_p2p_session_hooks {
    /* rmgk_gekko's debug hooks around every visible frame (on the emulation thread); false stops. */
    int (*begin_visible)(void* user);
    int (*end_visible)(void* user);
    /* RollbackNetcode's core wrappers, seen through net/rmgk_glue.cpp. */
    void (*state_saved)(int frame, void* user);
    void (*state_loaded)(int frame, void* user);
    void (*frame_resimulated)(void* user);
    void (*end_frame)(int confirmed, void* user);   /* frames <= confirmed are final */
    void (*timesync_scale)(double scale, void* user); /* the time sync's speed for the next frames */
    /* 1: a DROP from the peer ends the session at once (the dialog does that). 0: the program decides
     * (znx-net keeps running until its own frames are final, then stops). */
    int stop_on_peer_drop;
    int verbose_log;            /* RMG-K's Rollback_VerboseStats: GekkoNet's log in log_dir/Logs */
    const char* log_dir;
    void* user;
};

/* What ended the last session. */
enum znx_p2p_end {
    ZNX_P2P_END_NONE,
    ZNX_P2P_END_LOCAL,          /* we left (znx_p2p_leave_game): the peer was told with n02 DROP */
    ZNX_P2P_END_PEER_DROP,      /* the peer left the match (DROP); still in the lobby */
    ZNX_P2P_END_PEER_LEFT,      /* the peer left the lobby (EXIT) */
    ZNX_P2P_END_TIMEOUT,        /* nothing from the peer for GekkoNet's 5 s, or no frame for 20 s */
    ZNX_P2P_END_ERROR,          /* the session could not start */
};

/* Runs the match on the calling (emulation) thread once the core is set up as for any session
 * (bench_core.c's session_run): core is the core library's handle. Returns 0 when it ended
 * normally (any of the ends above but ERROR). */
int znx_p2p_run_session(void* core, const struct znx_p2p_session_hooks* hooks);
/* Leave the match from any thread: n02 DROP to the peer, then the session stops at the next frame. */
void znx_p2p_leave_game(void);
/* Tell the peer with n02 DROP but keep the session running (znx-net: its frames are final, the peer's
 * may not be yet, so it keeps sending inputs until the peer's DROP arrives). */
void znx_p2p_send_drop(void);
/* Stop the session without telling the peer (the program is closing, or znx_p2p_send_drop came first). */
void znx_p2p_stop_game(void);
int znx_p2p_last_end(char* text, size_t text_size);   /* enum znx_p2p_end and a sentence for the player */
int znx_p2p_peer_dropped(void);   /* the peer's DROP arrived during this session */

/* While a session runs (any thread): for the overlay. */
struct znx_p2p_live {
    int ping, delay, rollbacks, deepest, frames;
    float frames_ahead;
    double speed;               /* the time sync's speed scale */
};
void znx_p2p_live(struct znx_p2p_live* live);
int znx_p2p_visible_frames(void);

/* ---- the public list ---- */

struct znx_p2p_listing {
    char game[128];
    char app[128];              /* without " {CC:code}" */
    char user[64];
    char address[64];           /* ip:port as listed */
    char code[32];              /* the connect code (only games with one are kept, as RMG-K does) */
};

/* Parses plist.txt (lines "game|app {CC:code}|user|ip:port|...", or one flat pipe stream); keeps
 * games with a connect code. Returns how many were stored (at most max). */
int znx_p2p_parse_list(const char* body, struct znx_p2p_listing* out, int max);
/* GET http://host:port/plist.txt (blocking, about 6 s at most). Returns the count, or -1 with error set. */
int znx_p2p_fetch_list(const char* host, int port, struct znx_p2p_listing* out, int max, char* error, size_t error_size);

/* "abc12", "ABC@012", "abc-12" -> "ABC@12" (KailleraP2PDialog normalizeTraversalCode); 0 if it is not a code. */
int znx_p2p_normalize_code(const char* input, char* out, size_t out_size);
/* RMG-K's automatic delay for a ping (automaticRollbackFrameDelayForPing). */
int znx_p2p_auto_delay(int ping_ms);

#ifdef __cplusplus
}
#endif

#endif
