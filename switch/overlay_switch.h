// SPDX-License-Identifier: GPL-3.0-or-later
/* overlay_switch.c: the online match's small overlay (ping, delay, rollbacks), drawn over GLideN64's
 * finished frame in its own context, in the margin beside the picture. Never touches the game. */
#ifndef ZNX_OVERLAY_SWITCH_H
#define ZNX_OVERLAY_SWITCH_H

/* With GLideN64's context current, right before the buffer swap. */
void znx_overlay_draw(int width, int height, const char* const* labels, const char* const* values, int count);
/* With the same context still current, before it is destroyed. */
void znx_overlay_shutdown(void);

#endif
