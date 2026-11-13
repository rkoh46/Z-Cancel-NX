// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-net: the callbacks RMG-K's n02 P2P core (n02/core/p2p_core.cpp, compiled unchanged from
 * ref/RMG-K) makes into its user interface. In RMG-K they go through n02_callbacks.cpp to the Qt
 * dialog (KailleraP2PDialog); here they go to znx-net.cpp. Also the two globals p2p_core.cpp
 * takes from n02_client.cpp. */

#include <cstdarg>
#include <cstdio>

#include "core/p2p_core.h"

#include "znx_net.h"

int p2p_30fps_mode = 0;               /* n02_client.cpp: halves the delay of the delay-based mode */
char recording_player_names[4][32];   /* n02_client.cpp: names for the .krec header */

void p2p_chat_callback(char* nick, char* msg)
{
    znx_net_chat(nick != nullptr ? nick : "", msg != nullptr ? msg : "");
}

void p2p_game_callback(char* game, int playerno, int maxplayers)
{
    (void)maxplayers;
    znx_net_game_started(game != nullptr ? game : "", playerno);
}

void p2p_end_game_callback()
{
    znx_net_game_ended();
}

void p2p_client_dropped_callback(char* nick, int playerno)
{
    (void)nick;
    (void)playerno;
}

void p2p_core_debug(char* format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    line[0] = 0;
    if (format != nullptr)
        vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    znx_net_debug(line);
}

void p2p_hosted_game_callback(char* game)
{
    znx_net_hosted_game(game != nullptr ? game : "");
}

void p2p_ping_callback(int ping)
{
    (void)ping;
}

void p2p_peer_left_callback()
{
    znx_net_peer_left();
}

void p2p_peer_joined_callback()
{
    znx_net_peer_joined();
}

void p2p_peer_info_callback(char* name, char* app)
{
    znx_net_peer_info(name != nullptr ? name : "", app != nullptr ? app : "");
}

int p2p_getSelectedDelay()
{
    return 0;   /* the delay-based mode's "auto"; the rollback mode never asks */
}

void p2p_ssrv_packet_recv_callback(char* cmd, int len, void* sadr)
{
    /* Super-server and NAT traversal replies (p2plist.smash64.net, nat.smash64.net): this step
     * talks to neither. */
    (void)cmd;
    (void)len;
    (void)sadr;
}
