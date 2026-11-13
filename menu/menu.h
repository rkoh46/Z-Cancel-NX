// SPDX-License-Identifier: GPL-3.0-or-later
/* The start screen: which game (vanilla, Remix, Remix +EXTRA), then Play or Online play, and a
 * small settings page. Platform-neutral: the platform (switch/menu_switch.c on the console,
 * game/mac/menu_mac.c for screenshots) owns the OpenGL context and the controllers, calls
 * znx_draw_init with the font, then once a frame znx_menu_update with the buttons held and
 * znx_menu_draw, until update answers PLAY or QUIT. The menu object lives for the whole program,
 * so after a game the player comes back to where they were. */
#ifndef ZNX_MENU_H
#define ZNX_MENU_H

#include <stdint.h>

#include "online.h"
#include "settings.h"

/* Buttons, by the Switch's names (a GameCube controller's A, B, X, Y, Start come in as the same). */
#define ZNX_BTN_UP     0x0001u
#define ZNX_BTN_DOWN   0x0002u
#define ZNX_BTN_LEFT   0x0004u
#define ZNX_BTN_RIGHT  0x0008u
#define ZNX_BTN_A      0x0010u
#define ZNX_BTN_B      0x0020u
#define ZNX_BTN_X      0x0040u
#define ZNX_BTN_Y      0x0080u
#define ZNX_BTN_PLUS   0x0100u
#define ZNX_BTN_MINUS  0x0200u
#define ZNX_BTN_L      0x0400u
#define ZNX_BTN_R      0x0800u
#define ZNX_BTN_ZL     0x1000u
#define ZNX_BTN_ZR     0x2000u

/* PLAY_ONLINE: both players are ready in an online lobby; run the ROM with znx_p2p_run_session as the
 * session, then call znx_menu_online_finished. */
enum znx_menu_action { ZNX_MENU_NONE, ZNX_MENU_PLAY, ZNX_MENU_QUIT, ZNX_MENU_PLAY_ONLINE };

struct znx_menu;

struct znx_menu_config {
    const char* base_dir;   /* sdmc:/switch/z-cancel-nx */
    const char* version;    /* shown small in the corner */
    znx_keyboard_fn keyboard;   /* the system keyboard (names, connect codes, addresses); NULL: none */
    int console_theme;      /* the console's theme: 0 Basic White, 1 Basic Black, -1 not known */
    const char* theme_note; /* with -1: why, for the log; may be NULL */
    int debug_glyphs;       /* start on the button glyph check (also the ini's debug_glyphs=1) */
};

struct znx_menu* znx_menu_create(const struct znx_menu_config* config);
void znx_menu_destroy(struct znx_menu* menu);

/* Called when the menu is shown again (after a game): input is ignored until every button has
 * been let go (the quit combination is usually still held). */
void znx_menu_resume(struct znx_menu* menu);

/* Shows a message first (for example why the last game stopped). detail may be NULL. */
void znx_menu_notice(struct znx_menu* menu, const char* title, const char* text, const char* detail);

/* One frame, 60 times a second. held: the ZNX_BTN_* bits held now on any controller. */
enum znx_menu_action znx_menu_update(struct znx_menu* menu, uint32_t held);
/* With znx_draw_init done in the current context. */
void znx_menu_draw(struct znx_menu* menu, unsigned framebuffer, int width, int height);

/* After ZNX_MENU_PLAY: the ROM to run, which game it is, and the settings as saved. */
const char* znx_menu_rom_path(const struct znx_menu* menu);
int znx_menu_game(const struct znx_menu* menu);
void znx_menu_get_settings(const struct znx_menu* menu, struct znx_menu_settings* settings);

/* After an online match (ZNX_MENU_PLAY_ONLINE): back to the screen before the match if the other player
 * is still there, else a message saying how it ended. Call before showing the menu again. */
void znx_menu_online_finished(struct znx_menu* menu);

/* 1 while a ROM is being built (the build screen is up). */
int znx_menu_busy(const struct znx_menu* menu);

/* Preview only: an online screen with made-up content (online.h znx_online_debug). */
void znx_menu_debug_online(struct znx_menu* menu, int game, const char* which);

/* Preview only: shows the build screen with made-up progress (no thread, nothing written). */
void znx_menu_debug_build(struct znx_menu* menu, int game, int downloads, int step, uint64_t done, uint64_t total);

#endif
