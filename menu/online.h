// SPDX-License-Identifier: GPL-3.0-or-later
/* The hook the start screen's "Online play" row calls. Netplay (GekkoNet rollback against RMG-K,
 * connect codes, the lobby) is being built separately in net/; this is where it plugs in.
 *
 * Today znx_online_open only says that online play is not there yet. The netplay work replaces
 * online.c: it may run its own screens (it is called on the menu's thread with the menu's OpenGL
 * context current, between two frames) and either answer with a message for the menu to show, or
 * return ZNX_ONLINE_PLAY with a session for the front to run instead of a local game. */
#ifndef ZNX_ONLINE_H
#define ZNX_ONLINE_H

#include <stddef.h>

struct znx_online_request {
    int game;                 /* enum znx_game_id */
    const char* title;        /* "Smash Remix 2.0.1" */
    const char* rom_path;     /* the checked ROM */
    const char* rom_md5;      /* what RMG-K's lobby compares, 32 upper-case hex digits */
    const char* base_dir;     /* sdmc:/switch/z-cancel-nx */
};

enum znx_online_result {
    ZNX_ONLINE_MESSAGE,       /* show title/text and return to the game's screen */
    ZNX_ONLINE_PLAY,          /* a session was set up: start the game (the netplay work defines how) */
};

/* title/text: what the menu shows for ZNX_ONLINE_MESSAGE. */
enum znx_online_result znx_online_open(const struct znx_online_request* request,
                                       char* title, size_t title_size, char* text, size_t text_size);

#endif
