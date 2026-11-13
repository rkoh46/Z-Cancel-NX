// SPDX-License-Identifier: GPL-3.0-or-later
/* The Linux self-test's znx-net (builder/build-switch.sh --selftest-net) is the peer-to-peer build: the
 * container's GCC 12 has no <format>, which RMG-Core's Cheats.cpp (part of the lobby's manifest code)
 * needs. These stand in for net/lobby_play.cpp and the lobby's test hooks so znx-net links; --lobby says
 * it is not in this build. The console and the Mac build have the real ones. */
#include <cstdio>
#include <cstring>

#include "lobby_play.h"

extern "C" {

void znx_lobby_play_username(const char* name, char* out, size_t out_size) { std::snprintf(out, out_size, "%s", name); }
int znx_lobby_play_open(const struct znx_lobby_play_config*, char* error, size_t error_size)
{
    std::snprintf(error, error_size, "the lobby is not in this self-test build (peer to peer only)");
    return 1;
}
void znx_lobby_play_close(void) {}
int znx_lobby_play_is_open(void) { return 0; }
void znx_lobby_play_poll(void) {}
void znx_lobby_play_status(struct znx_lobby_play_status* s) { std::memset(s, 0, sizeof(*s)); }
int znx_lobby_play_take_problem(char*, size_t, char*, size_t) { return ZNX_LPP_NONE; }
int znx_lobby_play_rooms(struct znx_lobby_play_room*, int, int* all) { if (all) *all = 0; return 0; }
int znx_lobby_play_room_view(struct znx_lobby_play_room_view*) { return -1; }
int znx_lobby_play_chat(struct znx_lobby_play_chat_line*, int) { return 0; }
int znx_lobby_play_create_room(const char*, int) { return -1; }
int znx_lobby_play_join_room(uint64_t) { return -1; }
int znx_lobby_play_leave_room(void) { return -1; }
int znx_lobby_play_send_chat(const char*) { return -1; }
int znx_lobby_play_set_delay(int) { return -1; }
int znx_lobby_play_start(void) { return -1; }
int znx_lobby_play_test_manifest(int, int) { return -1; }
int znx_lobby_play_input_players(void) { return 2; }
int znx_lobby_play_local_slot(void) { return 0; }
int znx_lobby_play_local_delay(void) { return 0; }
int znx_lobby_play_prepare_core(void*) { return 1; }
int znx_lobby_play_run_session(void*, const struct znx_p2p_session_hooks*) { return 1; }
int znx_lobby_play_after_game(char*, size_t, char*, size_t) { return 0; }
void znx_lobby_rmgk_netsim(int, int, double, unsigned) {}
void znx_lobby_rmgk_netsim_stats(uint64_t* sent, uint64_t* dropped) { *sent = *dropped = 0; }
void znx_lobby_rmgk_netsim_shutdown(void) {}

}
