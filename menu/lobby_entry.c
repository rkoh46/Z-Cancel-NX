// SPDX-License-Identifier: GPL-3.0-or-later
/* See lobby_entry.h. A stub until the lobby work (net/lobby/) fills it in. */

#include "lobby_entry.h"

#include <stdio.h>

enum znx_online_result znx_lobby_open(const struct znx_online_request* request, char* title, size_t title_size,
                                      char* text, size_t text_size)
{
    (void)request;
    snprintf(title, title_size, "The lobby is not in this version yet");
    snprintf(text, text_size,
             "RMG-K's online lobby is being built. Until then, play peer to peer: host a game and give the other "
             "player your code, or join theirs.");
    return ZNX_ONLINE_MESSAGE;
}
