// SPDX-License-Identifier: GPL-3.0-or-later
/* input_switch.c: the console's controllers as N64 controllers. */
#ifndef ZNX_INPUT_SWITCH_H
#define ZNX_INPUT_SWITCH_H
#include <stdint.h>

/* Call once before the first frame. stick_range is what a fully tilted stick reads on the N64 axis
 * (RMG-K's default is 84, 66% of the protocol's 127); 0 picks that default. */
void znx_input_init(int stick_range);
/* The same, with l_shield 1 making the Switch L button the N64's Z (shield) instead of L (taunt), and the
 * C stick as smash attacks and aerials (c_mode 1 or 2) instead of the C buttons. */
void znx_input_init_ex(int stick_range, int l_shield, int c_mode);   /* c_mode: 0 C buttons, 1 attacks on
                                                                       GameCube controllers, 2 attacks on all */
/* 1 when these held buttons are the quit combination (see znx_input_quit_requested). */
int znx_input_is_quit_combo(uint64_t held);

/* Reads every controller once. Call at the start of each frame that takes live input, on one thread. */
void znx_input_poll(void);

/* Player 0..3's input from the last poll as the core's rollback input word (the controller's reply bytes,
 * first byte highest: buttons, buttons, stick x, stick y; the format of the files in bench/inputs). 0 when that
 * controller is not connected. */
uint32_t znx_input_buttons(int player);
int znx_input_connected(int player);

/* 1 once the quit combination has been held for half a second: Minus, or L + R + Z + Start on a GameCube
 * controller (Plus + R + any two of L, ZL, ZR: the adapter reports the shoulders as ZL and ZR, GameCube-style
 * wireless controllers can report the left one as L). */
int znx_input_quit_requested(void);
#endif
