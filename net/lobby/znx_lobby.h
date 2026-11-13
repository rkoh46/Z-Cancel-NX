/* SPDX-License-Identifier: GPL-3.0-or-later */
/* znx_lobby: a client for RMG-K's rollback lobby ("Join Rollback Netplay", rmgk-lobby), as a
 * library with a C API. Protocol: docs/PLAN.md, "RMG-K lobby, in detail".
 *
 * Everything is driven by znx_lobby_poll() from one thread: no event loop, no callbacks into the
 * caller, non-blocking sockets. libjuice (the ICE transport) runs one thread of its own; the
 * datagram functions at the end (znx_lobby_send, znx_lobby_receive, the GekkoNet adapter) may be
 * called from the emulation thread while the UI thread polls.
 *
 * What RMG-K calls "ready" has no message of its own: a room is ready when every seated player
 * has a direct ICE path to every other and every path has a measured ping; the host then starts
 * (ROOM_START). znx_lobby_can_start() applies RMG-K's rule. */
#ifndef ZNX_LOBBY_H
#define ZNX_LOBBY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RMG-K 0.9.16's servers (LobbyConnectDialog.cpp:31-35): release builds use the first, builds
 * from an untagged commit the second. RMG-K's own override: the RMGK_LOBBY_URL variable. */
#define ZNX_LOBBY_URL_RELEASE "ws://216.128.157.98:8080/ws"
#define ZNX_LOBBY_URL_TEST    "ws://64.177.116.159:8080/ws"
/* What RMG-K 0.9.16 puts in HELLO.clientVersion (CORE_VERSION = the git tag). The server answers
 * HELLO_FAIL "version_mismatch" below 0.9.13 (RollbackLobbyDialog.cpp:2744-2746). */
#define ZNX_LOBBY_CLIENT_VERSION "v0.9.16"

typedef struct znx_lobby znx_lobby;

typedef struct znx_lobby_config {
    /* STUN for ICE. NULL: the server's (HELLO_OK.ice.stun), else stun.l.google.com:19302, as
     * RMG-K does (LobbyClient.cpp:969-977). "": no STUN at all. */
    const char* stun_host;
    int stun_port;
    /* 0: ignore the TURN relays HELLO_OK offers. */
    int use_server_turn;
    /* Local address the ICE agents bind to. NULL: all interfaces (RMG-K). "127.0.0.1" makes the
     * only candidate the loopback one (tests). */
    const char* bind_address;
    /* Optional HELLO fields RMG-K sends (LobbyClient.cpp:822-851). NULL/0 leaves them out. */
    const char* local_ip;      /* private IPv4 of this machine */
    const char* connection;    /* "lan" or "wifi" */
    int tz_offset_sec;         /* standard (non-DST) UTC offset, used to split North America */
    int anonymous;             /* ask the server to hide this player's country */
    /* 1: HELLO says saveData:true, i.e. this client applies the save section of the pre-match
     * manifest (otherwise rooms with save data cannot start while we are seated). */
    int save_data;
    /* Diagnostics, called from znx_lobby_poll's thread. */
    void (*log)(void* user, const char* line);
    void* log_user;
} znx_lobby_config;

typedef enum znx_lobby_event_type {
    ZNX_LOBBY_EV_NONE = 0,
    ZNX_LOBBY_EV_CONNECTED,          /* HELLO_OK: user_id is ours */
    ZNX_LOBBY_EV_HELLO_FAILED,       /* text: the server's reason ("username_taken", "version_mismatch", ...) */
    ZNX_LOBBY_EV_DISCONNECTED,       /* text: why */
    ZNX_LOBBY_EV_USERS,              /* the user list changed: znx_lobby_user_count/_user */
    ZNX_LOBBY_EV_ROOM_LIST,          /* znx_lobby_room_count/_room */
    ZNX_LOBBY_EV_ROOM_CREATED,       /* room_id */
    ZNX_LOBBY_EV_ROOM_CREATE_FAILED, /* text */
    ZNX_LOBBY_EV_ROOM_JOINED,        /* room_id */
    ZNX_LOBBY_EV_ROOM_JOIN_FAILED,   /* text */
    ZNX_LOBBY_EV_ROOM_STATE,         /* our room changed: znx_lobby_current_room */
    ZNX_LOBBY_EV_ROOM_LEFT,          /* text: reason ("kicked", ...) */
    ZNX_LOBBY_EV_CHAT,               /* channel ("lobby", "room", "spectate"), user_id, name, text */
    ZNX_LOBBY_EV_PEER_ICE,           /* user_id, value: 1 connected, 0 not (yet), -1 failed for good;
                                        value2: attempt number (1..20) */
    ZNX_LOBBY_EV_PEER_PING,          /* user_id, value: round trip in ms over the ICE path */
    ZNX_LOBBY_EV_MATCH_BEGIN,        /* MATCH_BEGIN arrived: match_id; negotiating */
    ZNX_LOBBY_EV_MATCH_READY,        /* pre-match sync done, start the game: znx_lobby_match */
    ZNX_LOBBY_EV_MATCH_FAILED,       /* text; the server has been told (MATCH_FINISHED) */
    ZNX_LOBBY_EV_MATCH_PEER_LEFT,    /* user_id, value: slot, text: reason */
    ZNX_LOBBY_EV_NOTICE              /* text: a server notice (MOD_NOTICE), name: severity */
} znx_lobby_event_type;

typedef struct znx_lobby_event {
    znx_lobby_event_type type;
    uint64_t user_id;
    uint64_t room_id;
    uint64_t match_id;
    int value;
    int value2;
    char channel[16];
    char name[64];
    char text[512];
} znx_lobby_event;

typedef struct znx_lobby_user_info {
    uint64_t id;
    char username[64];
    char state[24];         /* the server's user state ("idle", "in_room", "in_game", "searching", "away", ...) */
    char region[24];        /* "us-east", "eu-west", ... (LobbyRegions.hpp) */
    char country[8];        /* ISO 3166-1 alpha-2, "" when anonymous */
    char client_version[32];
    char connection[16];
    uint64_t room_id;
    char room_name[64];
} znx_lobby_user_info;

typedef struct znx_lobby_room_info {
    uint64_t id;
    char name[64];
    uint64_t host_id;
    char host_name[64];
    char rom_name[96];
    char rom_md5[40];
    int players, max_players;
    char state[16];         /* "waiting" or "in_game" */
    int has_password;
    int late_join;
} znx_lobby_room_info;

typedef struct znx_lobby_seat {
    uint64_t user_id;       /* 0: empty seat */
    char username[64];
    int frame_delay;        /* what that player published (ROOM_UPDATE_DELAY), -1 unknown */
    int ice;                /* 1 connected, 0 connecting, -1 failed (always 1 for ourselves) */
    int ping_ms;            /* -1 not measured yet (0 for ourselves) */
} znx_lobby_seat;

typedef struct znx_lobby_room_detail {
    uint64_t id;            /* 0: not in a room */
    char name[64];
    uint64_t host_id;
    char rom_name[96];
    char rom_md5[40];
    char rom_region[16];
    int max_players;
    char state[16];
    int prediction;         /* the host's prediction window, 1..9 (default 7) */
    int prediction_auto;
    int pacing;
    znx_lobby_seat seats[4];   /* P1..P4 */
    int local_delay;        /* the delay this client will play with */
    int local_delay_auto;
} znx_lobby_room_detail;

typedef struct znx_lobby_match_peer {
    uint64_t user_id;
    char username[64];
    int slot;               /* 1..4 */
} znx_lobby_match_peer;

typedef struct znx_lobby_match_info {
    uint64_t match_id;
    uint64_t host_id;           /* sync authority (the room's host, not seat 1) */
    int local_slot;             /* our controller port, 1..4 */
    int seats;                  /* ports a late-join match emulates (MATCH_BEGIN.seats), 0 otherwise */
    int total_seats;            /* what to pass as `players` to the session: max(local, remotes, seats) */
    int local_delay;            /* our own input delay (local to every player) */
    int prediction;             /* the room's prediction window */
    int pacing;                 /* room pacing (RMG-K always 1, "smooth") */
    int num_peers;              /* remote players */
    znx_lobby_match_peer peers[3];
    const uint8_t* manifest;    /* the pre-match manifest ("RMGKPMAN", cheats and save data) */
    size_t manifest_size;
    uint64_t manifest_hash;     /* FNV-1a 64 of the manifest (LobbyClient.cpp:247-256) */
} znx_lobby_match_info;

znx_lobby* znx_lobby_create(const znx_lobby_config* config);
/* libjuice's own log lines (process-wide) to fn, INFO and up with verbose (WARN otherwise); NULL: stderr. */
void znx_lobby_set_ice_log(void (*fn)(const char* line), int verbose);
void znx_lobby_destroy(znx_lobby* lobby);

/* rom_md5: the MD5 RMG-K reports for the game (hex, compared case-insensitively). It is
 * used as the default ROM of rooms created with znx_lobby_create_room (HELLO.romHashes is left empty,
 * as RMG-K 0.9.16 sends it, RollbackLobbyDialog.cpp:2619). NULL client_version
 * sends ZNX_LOBBY_CLIENT_VERSION. Returns 0 on success (then wait for CONNECTED). */
int znx_lobby_connect(znx_lobby* lobby, const char* server_url, const char* username,
                      const char* rom_md5, const char* client_version);
void znx_lobby_disconnect(znx_lobby* lobby);
int znx_lobby_connected(const znx_lobby* lobby);
uint64_t znx_lobby_self_id(const znx_lobby* lobby);
/* The region HELLO_OK placed this client in ("us-east", ...), "" before. Valid until the next poll. */
const char* znx_lobby_self_region(const znx_lobby* lobby);

/* Returns 1 and fills *ev while there are events, 0 when there are none for now. Call it often
 * (every frame of the UI, every few ms in a match: ICE signalling and pings ride on it). */
int znx_lobby_poll(znx_lobby* lobby, znx_lobby_event* ev);

int znx_lobby_user_count(const znx_lobby* lobby);
int znx_lobby_user(const znx_lobby* lobby, int index, znx_lobby_user_info* out);
int znx_lobby_room_count(const znx_lobby* lobby);
int znx_lobby_room(const znx_lobby* lobby, int index, znx_lobby_room_info* out);
int znx_lobby_current_room(const znx_lobby* lobby, znx_lobby_room_detail* out);

/* Rooms. max_players 2..4; delay is stored by the server for older clients (each player now picks
 * their own); prediction 1..9; password NULL for none; rom_region e.g. "USA" (may be NULL). */
int znx_lobby_create_room(znx_lobby* lobby, const char* name, const char* rom_name, const char* rom_md5,
                          const char* rom_region, int max_players, int delay, int prediction,
                          const char* password);
int znx_lobby_join_room(znx_lobby* lobby, uint64_t room_id, const char* password);
int znx_lobby_leave_room(znx_lobby* lobby);
/* "lobby" or "room". */
int znx_lobby_send_chat(znx_lobby* lobby, const char* channel, const char* text);
/* delay -1: Auto (RMG-K's table from the worst measured ping, RollbackLobbyDialog.cpp:113-121). */
int znx_lobby_set_delay(znx_lobby* lobby, int delay);
/* Host only. prediction 0: Auto (7). */
int znx_lobby_set_prediction(znx_lobby* lobby, int prediction);
/* Host only: RMG-K's Start button rule (RollbackLobbyDialog.cpp:4370-4399). */
int znx_lobby_can_start(const znx_lobby* lobby);
int znx_lobby_start_match(znx_lobby* lobby);
int znx_lobby_swap_seats(znx_lobby* lobby, int slot_a, int slot_b);
int znx_lobby_kick(znx_lobby* lobby, uint64_t user_id);

/* Host: the pre-match manifest the next match sends to the players. NULL/0 sends RMG-K's empty one
 * (no cheats, blank save). Build real ones with RMG-K's CoreSerializeNetplayCheats /
 * CoreSerializeNetplaySaveData layout (docs/PLAN.md). The bytes are copied. */
int znx_lobby_set_prematch_manifest(znx_lobby* lobby, const uint8_t* data, size_t size);

/* After MATCH_READY. */
int znx_lobby_match(const znx_lobby* lobby, znx_lobby_match_info* out);
/* The game is running (MATCH_CONNECTED) / over (MATCH_FINISHED, the room goes back to waiting). */
void znx_lobby_match_started(znx_lobby* lobby);
void znx_lobby_match_finished(znx_lobby* lobby);

/* ---- datagrams over the room's ICE mesh (any thread) ---- */
/* channel: 3 is the first GekkoNet session of a match; payload at most 4000 bytes. */
int znx_lobby_send(znx_lobby* lobby, uint64_t user_id, int channel, const void* data, size_t size);
/* Takes one queued datagram of that channel: returns its size (copied up to cap) and the sender,
 * or -1 when there is none. */
int znx_lobby_receive(znx_lobby* lobby, int channel, uint64_t* from_user_id, void* buf, size_t cap);
/* 1 while the direct ICE path to that room member is up (connected or completed). */
int znx_lobby_peer_connected(znx_lobby* lobby, uint64_t user_id);
/* "ice:<userId>", the GekkoNetAddress text RMG-K's adapter uses (rmgk_gekko.cpp:1561-1578, 2449). */
int znx_lobby_peer_address(uint64_t user_id, char* out, size_t cap);

/* A GekkoNetAdapter (GekkoNet's include/gekkonet.h) carrying the session over this lobby's mesh
 * on `channel` exactly like RMG-K's ice adapter (rmgk_gekko.cpp:1555-1634). Returned as void* so
 * this header does not need gekkonet.h; pass it to gekko_net_adapter_set. At most 4 lobbies can
 * hand out adapters at once (GekkoNet's adapter has no user pointer). Defined in lobby_gekko.cpp. */
void* znx_lobby_gekko_adapter(znx_lobby* lobby, int channel);
void znx_lobby_gekko_adapter_release(znx_lobby* lobby);

/* For RMG-K's session driver compiled with -DNETPLAY: the LobbyIce::send / take_packets /
 * peer_connected it calls go to this lobby (lobby_ice_rmgk.cpp). NULL unbinds. */
void znx_lobby_bind_rmgk(znx_lobby* lobby);
/* Tests only (lobby_ice_rmgk.cpp): latency (+- jitter) and loss on every datagram the session sends
 * through LobbyIce::send (GekkoNet's channel), like net/netsim.cpp on the n02 socket. */
void znx_lobby_rmgk_netsim(int latency_ms, int jitter_ms, double loss_pct, unsigned seed);
void znx_lobby_rmgk_netsim_stats(uint64_t* sent, uint64_t* dropped);
void znx_lobby_rmgk_netsim_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
