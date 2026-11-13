// SPDX-License-Identifier: GPL-3.0-or-later
/* The two settings the start screen edits, in <base>/z-cancel-nx.ini, the file the game already
 * reads (switch/game_switch.c): sound=0|1 and stick_range=60..100. Every other line of the file is
 * kept as it is; a key that is missing is added at the end. */
#ifndef ZNX_SETTINGS_H
#define ZNX_SETTINGS_H

#define ZNX_STICK_MIN 60
#define ZNX_STICK_MAX 100
#define ZNX_STICK_DEFAULT 84

struct znx_menu_settings {
    int sound;        /* 1 on */
    int stick_range;  /* what a full tilt reads on the N64 axis */
};

/* Defaults, then whatever the file says. */
void znx_settings_load(const char* base, struct znx_menu_settings* settings);
/* 0 on success. */
int znx_settings_save(const char* base, const struct znx_menu_settings* settings);

#endif
