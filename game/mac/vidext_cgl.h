// SPDX-License-Identifier: GPL-3.0-or-later
/* vidext_cgl.c: the core's video extension on an offscreen OpenGL context (macOS). */
#ifndef ZNX_VIDEXT_CGL_H
#define ZNX_VIDEXT_CGL_H

#include <stdint.h>

void znx_cgl_begin(void (*print)(int is_error, const char* line));
/* Save the picture of the first buffer swap at or after this many completed frames. */
void znx_cgl_shot(int frame, const char* path);
/* Arms every --shot again (for --repeat: each run saves its own pictures, over the same files). */
void znx_cgl_shots_again(void);
void* znx_cgl_functions(void);  /* for CoreOverrideVidExt */
uint64_t znx_cgl_take_swap_ns(void);
int znx_cgl_take_swaps(void);

#endif
