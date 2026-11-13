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

/* For a frame in which the plugin did not swap: waits until one display refresh after the
 * last swap or the last call of this, so the game keeps its 60 Hz without a swap to wait in. */
void znx_vidext_pace_without_swap(void);

#endif
