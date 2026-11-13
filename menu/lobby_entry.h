// SPDX-License-Identifier: GPL-3.0-or-later
/* The lobby screens: what the online menu's "Lobby" row opens. RMG-K's "Join Rollback Netplay" lobby
 * (a lobby server, rooms, ICE between the players, the pre-match manifest), driven through
 * net/lobby_play.h, in the start screen's look (menu_ui.h):
 *
 *   Connecting      the server it is connecting to, the name, the game
 *   Rooms           rooms of the chosen game only (the room's ROM MD5 is ours): name, host, players, ping;
 *                   the first row opens a room (name through the system keyboard, 2-4 seats)
 *   Room            seats with each player's ICE state, ping and delay; the room chat (Y writes, through
 *                   the system keyboard); our delay (left/right); the host's Start once RMG-K's rule allows it
 *   Starting        direct connections to every player, then the pre-match sync, then the game
 * Errors (server unreachable, version mismatch, name refused, kicked, ICE failed, the match not
 * starting) come back as ZNX_ONLINE_MESSAGE.
 *
 * online.c hands its lobby screen to these functions. */
#ifndef ZNX_LOBBY_ENTRY_H
#define ZNX_LOBBY_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#include "online.h"

/* Opens the lobby for this game (connects). Returns ZNX_ONLINE_STAY, or MESSAGE when it cannot start. */
enum znx_online_result znx_lobby_enter(const struct znx_online_request* request, char* title, size_t title_size,
                                       char* text, size_t text_size);
/* One frame. BACK: the lobby was left (closed); MESSAGE: show title/text, then the lobby again if
 * znx_lobby_active(), else the online menu; PLAY: the match starts. */
enum znx_online_result znx_lobby_update(uint32_t pressed, uint32_t held, char* title, size_t title_size, char* text,
                                        size_t text_size);
void znx_lobby_draw(void);
int znx_lobby_active(void);
/* A match is ready to start (also while another screen, such as a message, is up). */
int znx_lobby_match_ready(void);
void znx_lobby_close(void);
/* After the match: STAY (back in the room) or MESSAGE (how it ended; the lobby may be gone). */
enum znx_online_result znx_lobby_after_game(char* title, size_t title_size, char* text, size_t text_size);

/* For the platform's game loop (game_switch.c through online.c). */
int znx_lobby_run_session(void* core, const struct znx_p2p_session_hooks* hooks);
int znx_lobby_prepare_core(void* core);
int znx_lobby_input_players(void);

/* Preview only: a lobby screen with made-up content, no network. which: "connecting", "rooms",
 * "rooms-empty", "create", "room", "room-host", "room-4", "room-ice-failed", "starting", "sync". */
void znx_lobby_debug(const struct znx_online_request* request, const char* which);

#endif
