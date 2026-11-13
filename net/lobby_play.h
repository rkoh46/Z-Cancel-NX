// SPDX-License-Identifier: GPL-3.0-or-later
/* A lobby session from a player's side: what RMG-K's RollbackLobbyDialog does around LobbyClient
 * (RMG/UserInterface/Dialog/Lobby/RollbackLobbyDialog.cpp, v0.9.16), without Qt and without screens.
 * The Switch's lobby screens (menu/lobby_entry.c) and znx-net --lobby (the Mac test client) both drive
 * exactly this code, so what the Mac tests is what the console runs:
 *
 *   connect (HELLO with clientVersion v0.9.16) -> the room list of the chosen ROM only (rooms whose
 *   ROM MD5 is ours) -> create or join a room -> in the room: seats with each player's delay, ICE
 *   state and ping, room chat, our own delay (Auto from the worst ping, or fixed), the host's Start
 *   when RMG-K's rule allows it -> MATCH_BEGIN: wait for every ICE path, pre-match sync -> READY:
 *   the program boots the game with znx_lobby_play_prepare_core (cheats and save data from the
 *   manifest, before frame 0) and runs znx_lobby_play_run_session (RMG-K's start_lobby_session over
 *   the room's ICE mesh) -> back in the room.
 *
 * Threads: everything is called from one thread (the menu's), except during the match, when the
 * session's control thread polls the lobby (the menu is not running then). Calls are serialised by
 * one lock anyway. */
#ifndef ZNX_LOBBY_PLAY_H
#define ZNX_LOBBY_PLAY_H

#include <stddef.h>
#include <stdint.h>

#include "lobby/znx_lobby.h"
#include "p2p/znx_p2p.h"

#ifdef __cplusplus
extern "C" {
#endif

struct znx_lobby_play_config {
    const char* server_url;     /* NULL or "": ZNX_LOBBY_URL_RELEASE (what RMG-K v0.9.16 uses) */
    const char* username;       /* any text: made into a valid lobby name (znx_lobby_play_username) */
    const char* rom_md5;        /* our ROM, 32 hex digits */
    const char* rom_name;       /* its GoodName: the room's ROM name when we create one (RMG-K's) */
    const char* stun;           /* NULL: the server's STUN (RMG-K's behaviour); "" or "off": none; "host:port" */
    int no_turn;                /* 1: ignore the TURN relays HELLO_OK offers */
    const char* bind_address;   /* the ICE agents' local address; NULL: every interface */
    const char* local_ip;       /* HELLO.localIp, NULL to leave it out */
    const char* connection;     /* HELLO.connection ("wifi", "lan"), NULL to leave it out */
    void (*log)(const char* line);
};

enum znx_lobby_play_phase {
    ZNX_LP_CLOSED,
    ZNX_LP_CONNECTING,          /* WebSocket and HELLO */
    ZNX_LP_ROOMS,               /* connected, in no room */
    ZNX_LP_ENTERING,            /* ROOM_CREATE / ROOM_JOIN sent */
    ZNX_LP_ROOM,                /* in a room */
    ZNX_LP_STARTING,            /* MATCH_BEGIN: waiting for every ICE path, then the pre-match sync */
    ZNX_LP_READY,               /* start the game now (znx_lobby_play_run_session) */
    ZNX_LP_IN_GAME,
    ZNX_LP_FAILED,              /* the connection is gone; znx_lobby_play_take_problem says why */
};

/* Something to tell the player once, as a message screen. */
enum znx_lobby_play_problem {
    ZNX_LPP_NONE,
    ZNX_LPP_UNREACHABLE,        /* the server did not answer */
    ZNX_LPP_VERSION,            /* HELLO_FAIL version_mismatch */
    ZNX_LPP_REFUSED,            /* any other HELLO_FAIL (name taken, server full, ...) */
    ZNX_LPP_DISCONNECTED,       /* the connection dropped */
    ZNX_LPP_CREATE_FAILED,
    ZNX_LPP_JOIN_FAILED,
    ZNX_LPP_KICKED,
    ZNX_LPP_ROOM_CLOSED,        /* ROOM_LEFT for another reason than our own leaving */
    ZNX_LPP_MATCH_FAILED,       /* the match did not start (ICE, pre-match sync) */
    ZNX_LPP_ICE_FAILED,         /* no direct path to a player after RMG-K's 20 attempts */
};

struct znx_lobby_play_status {
    int phase;                  /* enum znx_lobby_play_phase */
    char server[128];           /* the URL in use */
    char username[32];          /* the name the lobby knows us by */
    char region[24];            /* HELLO_OK's */
    int users_online;
    int seconds;                /* in this phase */
    /* STARTING */
    int start_step;             /* 0 waiting for direct connections, 1 pre-match sync */
    int start_connected, start_peers;
    int start_seconds_left;     /* until RMG-K's limit (15 s for ICE, 10 s for the sync) */
};

struct znx_lobby_play_room {
    uint64_t id;
    char name[64];
    char host[64];
    int players, max_players;
    int in_game;
    int has_password;
    int ping_estimate;          /* RMG-K's region estimate between us and the host (LobbyRegions.hpp), 0 unknown */
};

struct znx_lobby_play_seat {
    uint64_t user_id;           /* 0: empty */
    char name[64];
    int delay;                  /* the delay that player published, -1 not yet */
    int ice;                    /* 1 direct path up, 0 connecting, -1 failed for good */
    int attempt;                /* ICE attempt 1..20 */
    int ping;                   /* ms over the ICE path, -1 not measured */
    int self, host;
};

struct znx_lobby_play_room_view {
    uint64_t id;                /* 0: not in a room */
    char name[64];
    char host_name[64];
    char rom_name[96];
    int same_rom;               /* the room's ROM MD5 is ours */
    int is_host;
    int in_game;
    int max_players, seated;
    struct znx_lobby_play_seat seats[4];
    int local_delay, delay_auto;
    int prediction;
    int can_start;              /* host: RMG-K's Start rule */
    char start_note[160];       /* host: why Start is not possible yet; others: who starts */
};

struct znx_lobby_play_chat_line {
    char name[64];              /* "" for a line of our own (system) */
    char text[256];
    int self;
};

/* RMG-K's username rule (LobbyConnectDialog.cpp:76-80): 2-20 of A-Z a-z 0-9 _ . -; anything else
 * becomes '_' (a space) or is dropped, and a name too short becomes "Switch". */
void znx_lobby_play_username(const char* name, char* out, size_t out_size);

int znx_lobby_play_open(const struct znx_lobby_play_config* config, char* error, size_t error_size);
void znx_lobby_play_close(void);
int znx_lobby_play_is_open(void);
/* Every frame of the menu. */
void znx_lobby_play_poll(void);
void znx_lobby_play_status(struct znx_lobby_play_status* status);
/* The problem to show (and clears it): enum znx_lobby_play_problem with a title and a sentence. */
int znx_lobby_play_take_problem(char* title, size_t title_size, char* text, size_t text_size);

/* Rooms of our ROM (newest list from the server). *all_rooms: rooms of every game. */
int znx_lobby_play_rooms(struct znx_lobby_play_room* out, int max, int* all_rooms);
int znx_lobby_play_room_view(struct znx_lobby_play_room_view* out);
/* The last `max` room chat lines, oldest first. */
int znx_lobby_play_chat(struct znx_lobby_play_chat_line* out, int max);

/* Actions; 0 when sent. */
int znx_lobby_play_create_room(const char* name, int max_players);
int znx_lobby_play_join_room(uint64_t room_id);
int znx_lobby_play_leave_room(void);
int znx_lobby_play_send_chat(const char* text);
int znx_lobby_play_set_delay(int delay);        /* -1 Auto, else 0..9 frames */
int znx_lobby_play_start(void);                 /* host */

/* Host: the next match's pre-match manifest (net/lobby/znx_lobby.h). Tests: znx_lobby_play_test_manifest
 * builds one with RMG-Core's own serializers: one cheat writing `cheat_value` at 0x80700000 and/or a
 * 32 KB SRAM save of a fixed pattern. */
int znx_lobby_play_test_manifest(int cheat_value, int with_save);

/* ---- the match (READY) ---- */
/* The match's seats as CoreStartEmulation computes them (the `players` of start_lobby_session). */
int znx_lobby_play_input_players(void);
/* Our seat, 1..4. */
int znx_lobby_play_local_slot(void);
int znx_lobby_play_local_delay(void);
/* With the ROM open and the plugins attached, before the save data and frame 0 (bench_core's
 * session_prepare): the manifest's cheats (CoreDeserializeNetplayCheats, CoreSetNetplayCheats,
 * CoreApplyNetplayCheats) and save data (qUncompress, CoreDeserializeNetplaySaveData,
 * CoreApplyNetplaySaveData(true)), as Emulation.cpp/LobbyClient.cpp do. 0 on success. */
int znx_lobby_play_prepare_core(void* core);
/* Runs the match on the calling (emulation) thread: znx_p2p_run_lobby_session. */
int znx_lobby_play_run_session(void* core, const struct znx_p2p_session_hooks* hooks);
/* After the game: MATCH_FINISHED if not sent yet. Returns 1 when we are back in the room, 0 when
 * there is a message to show instead (title/text: how the match ended or why the lobby is gone). */
int znx_lobby_play_after_game(char* title, size_t title_size, char* text, size_t text_size);

#ifdef __cplusplus
}
#endif

#endif
