// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-net: the pieces of the program that RMG-K's own sources call back into (through the glue in
 * rmgk_glue.cpp and n02_glue.cpp). Everything here runs on the emulation thread unless noted. */
#ifndef ZNX_NET_H
#define ZNX_NET_H

#include <cstdint>

/* rmgk_glue.cpp -> znx-net.cpp */
void znx_net_state_saved(int frame);      /* a rollback save of the state after GekkoNet frame `frame` */
void znx_net_state_loaded(int frame);     /* a rollback load of the state after frame `frame` */
void znx_net_frame_resimulated(void);     /* one hidden (re-simulated) frame ran */
void znx_net_end_frame(int confirmed);    /* end of a visible frame; frames <= confirmed are final */
int znx_net_visible_frames(void);         /* visible frames completed (RMG-K's CoreGetCurrentFrameCount) */
const char* znx_net_log_dir(void);        /* where RMG-K's GekkoNet log goes when it is turned on */
bool znx_net_verbose_log(void);           /* RMG-K's Rollback_VerboseStats setting */

/* n02_glue.cpp -> znx-net.cpp: the n02 P2P client's callbacks (any thread that runs n02). */
void znx_net_chat(const char* nick, const char* message);
void znx_net_game_started(const char* game, int player);
void znx_net_game_ended(void);
void znx_net_peer_joined(void);
void znx_net_peer_left(void);
void znx_net_peer_info(const char* name, const char* app);
void znx_net_hosted_game(const char* game);
void znx_net_debug(const char* message);

/* netsim.c: artificial latency, jitter and loss on the n02 socket (k_socket.cpp's sendto and
 * bind are renamed to these when it is compiled for znx-net). */
extern "C" {
struct znx_netsim_config {
    int latency_ms;      /* added to every datagram this side sends */
    int jitter_ms;       /* +- uniformly, so datagrams can arrive out of order */
    double loss_pct;     /* dropped at random: only GekkoNet's datagrams (0xff "RMGKRB1"), see netsim.c */
    unsigned seed;
    int loopback_only;   /* bind the socket to 127.0.0.1 instead of every interface */
};
void znx_netsim_configure(const struct znx_netsim_config* config);
void znx_netsim_stats(uint64_t* sent, uint64_t* dropped, uint64_t* delayed);
void znx_netsim_shutdown(void);
}

#endif
