// SPDX-License-Identifier: GPL-3.0-or-later
/* input_switch.c: the console's controllers as N64 controllers. */
#ifndef ZNX_INPUT_SWITCH_H
#define ZNX_INPUT_SWITCH_H
#include <stdint.h>

/* Call once before the first frame. stick_range is what a fully tilted stick reads on the N64 axis
 * (RMG-K's default is 84, 66% of the protocol's 127); 0 picks that default. */
void znx_input_init(int stick_range);

/* Reads every controller once. Call at the start of each frame that takes live input, on one thread. */
void znx_input_poll(void);

/* The N64 BUTTONS word of player 0..3 from the last poll (0 when that controller is not connected). */
uint32_t znx_input_buttons(int player);
int znx_input_connected(int player);

/* 1 once the quit combination has been held for half a second: Minus, or L + R + Z + Start on a GameCube
 * controller (ZL + ZR + R + Plus). */
int znx_input_quit_requested(void);
#endif
