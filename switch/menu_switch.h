// SPDX-License-Identifier: GPL-3.0-or-later
/* menu_switch.c: the start screen (menu/) on the console. */
#ifndef ZNX_MENU_SWITCH_H
#define ZNX_MENU_SWITCH_H

#include <stdbool.h>

#include "menu.h"

/* Shows the start screen on the shared screen (screen_switch.c) on the calling (main) thread, at the
 * display's 60 Hz, until the player picks Play or quits, or the system asks the program to close.
 * Leaves no OpenGL context current. ZNX_MENU_QUIT also when the screen cannot be drawn (logged). */
enum znx_menu_action znx_menu_switch_run(struct znx_menu* menu);
/* True if the last znx_menu_switch_run could not show the screen at all (why is in the log). */
bool znx_menu_switch_failed(void);

#endif
