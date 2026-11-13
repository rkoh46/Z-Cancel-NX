// SPDX-License-Identifier: GPL-3.0-or-later
/* sysfont_switch.c: the console's own shared fonts and theme setting, for the start screen and the
 * match overlay. Nothing of Nintendo's is in this program: the fonts are read from the console's
 * memory at run time (libnx's pl service), the theme from its settings (set:sys). */
#ifndef ZNX_SYSFONT_SWITCH_H
#define ZNX_SYSFONT_SWITCH_H

#include <stdbool.h>
#include <stddef.h>

/* The system's standard font (a TrueType file in shared memory) and its "Nintendo Extended" font with
 * the button glyphs. Asked for once, on the first call (the main thread, from the start screen); later
 * calls from any thread return the same pointers, which stay valid until the program ends. Either may
 * come back NULL (logged once: "menu: system font loaded (N bytes)" / "menu: system font unavailable:
 * 0x..., using the bundled font"). Returns true if the standard font is there. */
bool znx_sysfont_get(const void** standard, size_t* standard_size, const void** extended, size_t* extended_size);
/* The standard font turned out to be unreadable (FreeType refused it): stop offering it. */
void znx_sysfont_reject(void);

/* The console's theme: 1 Basic Black, 0 Basic White, -1 if the setting cannot be read (why is written
 * into note). */
int znx_sysfont_console_theme(char* note, size_t note_size);

#endif
