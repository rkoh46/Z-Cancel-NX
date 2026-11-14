// SPDX-License-Identifier: GPL-3.0-or-later
/* The settings the start screen edits, in <base>/z-cancel-nx.ini, the file the game already reads
 * (switch/game_switch.c). Every other line of the file is kept as it is; a key that is missing is
 * added at the end.
 *
 *   sound=0|1, stick_range=60..100        this console
 *   l_button=taunt|shield                 the Switch L button (a GameCube-style controller's left shoulder):
 *                                         the N64's L (taunt, the default) or Z (shield)
 *   c_stick=buttons|gamecube|all          the C stick: the N64's C buttons (default), or smash attacks and
 *                                         aerials on GameCube controllers (adapter) or on every controller
 *   name=<text>                           online: the name the other player sees (n02 LOGN, 31 bytes)
 *   input_delay=0..9                      online, as host: 0 = Auto (from the ping, as RMG-K), else frames
 *   public_list=0|1                       online, as host: list the game on RMG-K's public list
 *   net_overlay=0|1                       online: ping, rollbacks and delay in the corner during a match
 *   p2p_port=27886                        online, as host: the UDP port (RMG-K's Kaillera port setting)
 *   p2p_code=, p2p_token=                 the connect code the traversal server gave this console, kept
 *                                         as RMG-K keeps it (Kaillera P2PStaticCode / ...OwnerToken)
 *   nat_server=host:port, list_server=host:port   only for tests against a local stand-in; empty = RMG-K's
 *   net_vsync=0|1                         online: 0 (default) RMG-K's timer pacer with swap interval 0;
 *                                         1 the display's vsync with a frame dropped or repeated on drift
 *   lobby_server=ws://host:port/ws        the lobby (RMG-K's "Join Rollback Netplay"); empty = RMG-K v0.9.16's
 *                                         release server ws://216.128.157.98:8080/ws
 *   lobby_stun=off|host:port              STUN for the lobby's ICE; empty = the server's (RMG-K's behaviour)
 *   lobby_bind=a.b.c.d                    the ICE agents' local address; empty = this console's own address
 *   lobby_ice_test=nohost                 tests only: offer no local-network candidates, so a connection has to go
 *                                         through the public address (as with a player outside the house)
 *   theme=auto|light|dark                 the start screen's theme; auto (default) follows the console's
 *                                         Basic White / Basic Black setting
 *   debug_glyphs=0|1                      1: the start screen opens on a page of the console's button glyphs
 *                                         with their code points (to check them from a screenshot)
 * The three lobby_ keys, theme and debug_glyphs are read only: the menu never writes them (other lines of
 * the file are kept). */
#ifndef ZNX_SETTINGS_H
#define ZNX_SETTINGS_H

#define ZNX_STICK_MIN 60
#define ZNX_STICK_MAX 100
#define ZNX_STICK_DEFAULT 84

struct znx_menu_settings {
    int sound;        /* 1 on */
    int stick_range;  /* what a full tilt reads on the N64 axis */
    int l_shield;     /* 1: Switch L is the N64's Z (shield); 0: its L (taunt) */
    int c_mode;       /* 0 C buttons, 1 attacks on GameCube controllers, 2 attacks on every controller */
    char name[32];
    int input_delay;  /* 0 auto, 1..9 */
    int public_list;
    int net_overlay;
    int p2p_port;
    char p2p_code[32];
    char p2p_token[96];
    char nat_server[128];
    char list_server[128];
    int net_vsync;
    char lobby_server[160];
    char lobby_stun[128];
    char lobby_bind[64];
    char lobby_ice_test[16];
    char theme[8];    /* "auto", "light" or "dark" */
    int debug_glyphs;
};

/* Defaults, then whatever the file says. */
void znx_settings_load(const char* base, struct znx_menu_settings* settings);
/* 0 on success. */
int znx_settings_save(const char* base, const struct znx_menu_settings* settings);

#endif
