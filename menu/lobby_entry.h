// SPDX-License-Identifier: GPL-3.0-or-later
/* The "Lobby" row of the online menu: RMG-K's lobby mode (a lobby server, ICE, the prematch manifest).
 * Its client is being written separately in net/lobby/ (a library with a C API, no screens); this is
 * the one place the online screens call it. Until it is filled in, it only says the lobby is not there. */
#ifndef ZNX_LOBBY_ENTRY_H
#define ZNX_LOBBY_ENTRY_H

#include <stddef.h>

#include "online.h"

/* Called on the menu's thread, between two frames, when the player chooses "Lobby" for a game. It may
 * run its own screens later; today it answers ZNX_ONLINE_MESSAGE with title and text for the menu. */
enum znx_online_result znx_lobby_open(const struct znx_online_request* request, char* title, size_t title_size,
                                      char* text, size_t text_size);

#endif
