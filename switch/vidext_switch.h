// SPDX-License-Identifier: GPL-3.0-or-later
/* vidext_switch.c: the core's video extension on EGL and libnx. */
#ifndef ZNX_VIDEXT_SWITCH_H
#define ZNX_VIDEXT_SWITCH_H

#include <stdint.h>

/* The table for CoreOverrideVidExt (an m64p_video_extension_functions). */
void* znx_vidext_functions(void);

/* Since the last call: time spent inside eglSwapBuffers, and how many swaps there were.
 * Called on the emulation thread, like everything that touches the context. */
uint64_t znx_vidext_take_swap_ns(void);
int znx_vidext_take_swaps(void);
unsigned znx_vidext_take_late(uint64_t* longest_ns);

/* For a frame in which the plugin did not swap: waits until one display refresh after the
 * last swap or the last call of this, so the game keeps its 60 Hz without a swap to wait in. */
void znx_vidext_pace_without_swap(void);

/* ---- an online match (game_switch.c, during znx_p2p_run_session) ---- */

enum znx_net_present {
    ZNX_PRESENT_LOCAL,     /* not online: swap interval 1, the display paces the game */
    ZNX_PRESENT_TIMER,     /* RMG-K's pacer (rmgk_gekko::pace_before_present, a timer at 60 Hz x the time
                              sync's speed) decides when a frame is shown; swap interval 0 */
    ZNX_PRESENT_VSYNC,     /* swap interval 1; the 60 Hz x speed timeline is followed by showing a frame
                              twice or skipping one when the drift reaches a frame */
};

/* Before the run. overlay: draw the match overlay in the margin. */
void znx_vidext_set_netplay(enum znx_net_present mode, int overlay);
/* The overlay's lines (on the emulation thread, any time). */
void znx_vidext_set_overlay(const char* const* labels, const char* const* values, int count);
/* The time sync's speed (RollbackNetcode's CoreRollbackSetTimesyncScale), for ZNX_PRESENT_VSYNC. */
void znx_vidext_set_speed(double scale);
/* ZNX_PRESENT_VSYNC: once per visible frame, after it ran. Keeps the timeline and paces a frame in
 * which the plugin did not swap. */
void znx_vidext_net_frame_end(void);

#endif
