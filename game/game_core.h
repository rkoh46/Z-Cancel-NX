// SPDX-License-Identifier: GPL-3.0-or-later
/* The part of the picture build that is the same on every platform: GLideN64's settings
 * and the frame time report. The frame loop itself is bench/bench_core.c (the same forced
 * netplay settings and M64CMD_ROLLBACK_EXECUTE driver as znx-bench); the platform
 * (switch/game_switch.c, game/mac/znx-game-mac.c) supplies the window and OpenGL context
 * through the core's video extension and says where lines go. */
#ifndef ZNX_GAME_CORE_H
#define ZNX_GAME_CORE_H

#include <stdint.h>

#include "bench_core.h"

struct znx_game_settings {
    int width, height;   /* of the window GLideN64 draws into */
    int vsync;           /* swap interval 1: the buffer swap paces the game */
    /* 0: GLideN64 draws straight into the window at its size, frame buffer emulation off,
     *    which is what a fresh RMG-K install does and what leaves RDRAM alone.
     * 1, 2, ...: frame buffer emulation on, drawing at that multiple of the N64's resolution
     *    and scaling up. Every copy into RDRAM stays off, but GLideN64 then still clears
     *    buffers in RDRAM itself (FrameBufferList::fillRDRAM): check with rdram_check. */
    int scale;
    int report_every;    /* "shown" line every K frames, 0 = never */
};

/* Timing the platform's video extension measures, read once per frame. */
struct znx_game_video {
    uint64_t (*take_swap_ns)(void);  /* time spent inside buffer swaps since the last call */
    int (*take_swaps)(void);         /* number of buffer swaps since the last call */
    uint64_t (*thread_cpu_ns)(void); /* running time of the calling (emulation) thread so far, or NULL:
                                        tells work from waiting, wherever the driver does its waiting */
};

void znx_game_begin(const struct znx_game_settings* settings, const struct znx_game_video* video,
                    void (*print)(int is_error, const char* line));

/* znx_host.video_config: writes the "Video-General" and "Video-GLideN64" sections and
 * prints the settings that matter for RDRAM. */
int znx_game_video_config(const struct znx_config* config);

/* znx_host.frame_done: accumulates the times and prints the report. Returns nothing; the
 * platform may wrap it to add its own per-frame work (pacing a frame that did not swap). */
void znx_game_frame_done(int frames, uint64_t frame_ns);

/* Swaps seen during the frame that just ended (valid inside the platform's wrapper, after
 * znx_game_frame_done). */
int znx_game_last_frame_swaps(void);

#endif
