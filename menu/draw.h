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

/* Text sizes in pixels; the only ones the atlas holds. */
enum znx_font { ZNX_FONT_SMALL, ZNX_FONT_BODY, ZNX_FONT_ROW, ZNX_FONT_TITLE, ZNX_FONT_COUNT };

/* Text flags. */
#define ZNX_TEXT_TABULAR 1   /* every glyph in a run of digits, A-F and ':' gets the same width */
#define ZNX_TEXT_RIGHT   2   /* x is the right edge */
#define ZNX_TEXT_CENTER  4   /* x is the middle */

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
/* A parallelogram leaning right like the mark: the top edge is shifted by shear * h to the right
 * of the bottom edge. */
void znx_draw_slant(float x, float y, float w, float h, float shear, uint32_t color);
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

#endif
