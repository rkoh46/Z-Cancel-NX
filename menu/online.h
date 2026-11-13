// SPDX-License-Identifier: GPL-3.0-or-later
/* The online screens of the start screen: what the "Online play" row of a game opens. They follow
 * the start screen's look (menu_ui.h) and drive RMG-K's peer-to-peer netplay through net/p2p/znx_p2p.h:
 *
 *   Online play     Host a game, Join with a code, Join by IP address, Public games, Lobby
 *   Host            "Waiting for a player" with this console's connect code (or its address)
 *   Join            the system keyboard for the code or the address, then "Joining ..." until connected
 *   Public games    RMG-K's public list, only games with the chosen ROM's name
 *   Before a match  the other player, ping and the agreed delay; A ready, B leave
 *
 * menu.c hands its SCREEN_ONLINE to these functions. When both players are ready, update answers
 * ZNX_ONLINE_PLAY: the platform runs the game with znx_p2p_run_session as its session, then calls
 * znx_online_after_game and shows the menu again (the screen before the match, if the other player is
 * still there). */
#ifndef ZNX_ONLINE_H
#define ZNX_ONLINE_H

#include <stddef.h>
#include <stdint.h>

#include "settings.h"

/* What the platform offers for typing: the system keyboard on the console (libnx swkbd), a scripted
 * answer in the Mac preview. kind: 0 a name, 1 a connect code, 2 an IP address and port, 3 a chat line,
 * 4 a room name. Returns 1 with the text in out, 0 when the player cancelled. */
typedef int (*znx_keyboard_fn)(int kind, const char* header, const char* guide, const char* initial,
                               char* out, size_t out_size);

/* Where the lobby's diagnostics go, one line each, from any thread: defined by the platform (the
 * console's log, stderr on the Mac). */
void znx_log_line(const char* line);

struct znx_online_request {
    int game;                       /* enum znx_game_id */
    const char* title;              /* "Smash Remix 2.0.1" */
    const char* goodname;           /* the ROM's GoodName in RMG-K's database: the game name n02 sends */
    const char* rom_path;           /* the checked ROM */
    const char* rom_md5;            /* 32 upper-case hex digits */
    const char* base_dir;           /* sdmc:/switch/z-cancel-nx */
    struct znx_menu_settings* settings;   /* the menu's; the connect code is stored into it and saved */
    znx_keyboard_fn keyboard;
};

enum znx_online_result {
    ZNX_ONLINE_STAY,                /* still on an online screen */
    ZNX_ONLINE_BACK,                /* back to the game's screen */
    ZNX_ONLINE_MESSAGE,             /* show title/text, then the game's screen */
    ZNX_ONLINE_PLAY,                /* both are ready: run the match */
};

/* RMG-K's game name for one of the three games (mupen64plus.ini GoodName of its MD5). */
const char* znx_online_goodname(int game);

/* Opens the online menu for this game. The request's strings must stay valid while it is shown. */
void znx_online_enter(const struct znx_online_request* request);
/* One frame. pressed: buttons pressed this frame (with the menu's repeat); held: held now. */
enum znx_online_result znx_online_update(uint32_t pressed, uint32_t held, char* title, size_t title_size,
                                         char* text, size_t text_size);
void znx_online_draw(void);
/* After the match: ZNX_ONLINE_STAY (back to the screen before the match) or MESSAGE (why it ended;
 * the connection is closed). */
enum znx_online_result znx_online_after_game(char* title, size_t title_size, char* text, size_t text_size);
/* The connection is open (a lobby or a match). */
int znx_online_connected(void);

/* ---- the match ZNX_ONLINE_PLAY starts: peer to peer (net/p2p) or a lobby match (net/lobby_play.h) ---- */
struct znx_p2p_session_hooks;
/* 1 when it is a lobby match. */
int znx_online_lobby_match(void);
/* Runs it (the platform's session_run): znx_p2p_run_session or the lobby's. */
int znx_online_run_session(void* core, const struct znx_p2p_session_hooks* hooks);
/* The lobby match's cheats and save data, for bench_core's session_prepare (0 on success). */
int znx_online_prepare_core(void* core);
/* The controller ports the match synchronizes (2 peer to peer; the highest seat in a lobby match). */
int znx_online_input_players(void);
/* A lobby match became ready while another screen of the menu (a message) was up. */
int znx_online_match_ready(void);
/* Leaves whatever is open (the program is closing). */
void znx_online_close(void);

/* Preview only (znx-game --menu-shot): shows a screen with made-up content, no network. which:
 * "lobby", "lobby-mismatch", "host", "host-ip", "joining", "list", "list-empty", and the lobby's screens as
 * "room:<which>" (lobby_entry.h znx_lobby_debug). */
void znx_online_debug(const struct znx_online_request* request, const char* which);

#endif
