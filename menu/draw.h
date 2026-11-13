// SPDX-License-Identifier: GPL-3.0-or-later
/* A small OpenGL 3.3 core renderer for the start screen: rounded rectangles, the project's mark
 * and text from one TrueType font (FreeType, glyphs cached in one texture). The same code draws
 * on the Switch (EGL, the context the menu makes on the main thread) and in the Mac preview
 * (an offscreen CGL context). OpenGL functions come from the caller's getproc, so nothing here
 * links against a particular OpenGL library. Coordinates are pixels of a 1280x720 screen, origin
 * top left. Colours are 0xRRGGBBAA. */
#ifndef ZNX_DRAW_H
#define ZNX_DRAW_H

#include <stddef.h>
#include <stdint.h>

/* Text sizes (18, 20, 22, 28 and 40 pixels); the only ones the atlas holds. */
enum znx_font { ZNX_FONT_SMALL, ZNX_FONT_BODY, ZNX_FONT_ROW, ZNX_FONT_TITLE, ZNX_FONT_BIG, ZNX_FONT_COUNT };

/* Text flags. */
#define ZNX_TEXT_TABULAR 1   /* every glyph in a run of digits, A-F and ':' gets the same width */
#define ZNX_TEXT_RIGHT   2   /* x is the right edge */
#define ZNX_TEXT_CENTER  4   /* x is the middle */

/* Before znx_draw_init: a second font that holds the console's button glyphs in the private-use area
 * (the Switch's shared "Nintendo Extended" font, U+E0A0..U+E0FF). NULL: none, and znx_draw_button draws
 * its own stand-ins. The data stays owned by the caller. */
void znx_draw_set_symbol_font(const void* font_data, size_t font_size);
/* 1 if the symbol font has this code point (after znx_draw_init). */
int znx_draw_has_symbol(unsigned code);

/* With the context current. font_data stays owned by the caller until znx_draw_shutdown. */
int znx_draw_init(void* (*getproc)(const char* name), const void* font_data, size_t font_size,
                  char* error, size_t error_size);
void znx_draw_shutdown(void);

void znx_draw_begin(unsigned framebuffer, int width, int height, uint32_t clear);
/* The same without clearing: for drawing over a finished picture. The caller saves and restores the
 * OpenGL state it shares with another renderer (switch/overlay_switch.c). */
void znx_draw_begin_over(unsigned framebuffer, int width, int height);
void znx_draw_end(void);

void znx_draw_rect(float x, float y, float w, float h, float radius, uint32_t color);
/* The outline of a rounded rectangle, `thickness` pixels wide, inside x, y, w, h. */
void znx_draw_ring(float x, float y, float w, float h, float radius, float thickness, uint32_t color);
/* The Z-Cancel NX mark (tools/make_logo.py), its letter `height` pixels tall, top left at x, y. */
void znx_draw_mark(float x, float y, float height, uint32_t color);

/* One line at baseline y. Returns its width. Understands UTF-8; glyphs the font lacks are skipped. */
float znx_draw_text(int font, float x, float y, const char* text, uint32_t color, int flags);
float znx_text_width(int font, const char* text, int flags);
/* Pixels from one baseline to the next, and the height of a capital above the baseline. */
float znx_line_height(int font);
float znx_cap_height(int font);
/* Text wrapped to width, first baseline at y. Returns the number of lines; with color 0 it only measures. */
int znx_draw_wrapped(int font, float x, float y, float width, const char* text, uint32_t color);
/* Shortens text with an ellipsis in the middle until it fits width; writes into out. */
void znx_text_fit(int font, const char* text, float width, char* out, size_t out_size);
void znx_text_fit_flags(int font, const char* text, float width, char* out, size_t out_size, int flags);


/* A controller button in a line of text, as the console's system software shows it: the glyph of the
 * symbol font when that is loaded, else a stand-in drawn here (a disc with the letter, a cross for the
 * D-pad). key: "A" "B" "X" "Y" "L" "R" "ZL" "ZR" "+" "-", "LR" (D-pad left and right), "DPAD", "LS".
 * knock is the colour of a stand-in's letter (the background). Returns the width; with draw 0 it only
 * measures. */
float znx_draw_button(int font, float x, float baseline, const char* key, uint32_t color, uint32_t knock, int draw);
/* The code points znx_draw_button uses for a key (0, 1 or 2), for the glyph check screen. */
int znx_button_codes(const char* key, unsigned codes[2]);

/* ---- the theme: the console's Basic White or Basic Black ---- */

struct znx_theme {
    uint32_t background, panel, selected;      /* the screen, the side pane, the selected row's fill */
    uint32_t text, text2, text3;               /* primary, secondary, disabled */
    uint32_t separator, line;                  /* header and footer rules; the thin lines between rows */
    uint32_t accent;                           /* values, progress */
    uint32_t highlight_a, highlight_b;         /* the selection outline pulses between these */
    uint32_t ready, missing;                   /* status dots */
};

void znx_theme_set(int dark);
int znx_theme_is_dark(void);
const struct znx_theme* znx_theme(void);
/* The selection outline's colour now: it moves between the two highlight colours and back every 72 frames
 * (1.2 s at 60 Hz) of znx_draw_begin. znx_theme_freeze(phase 0..1) holds it still (previews). */
uint32_t znx_theme_highlight(void);
void znx_theme_freeze(float phase);

#endif
