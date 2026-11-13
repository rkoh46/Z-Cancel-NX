// SPDX-License-Identifier: GPL-3.0-or-later
/* The start screen's look, shared by its screens (menu.c) and the online screens (online.c): the
 * palette, the 1280x720 layout grid and the drawing pieces (header, title, list rows, detail panel,
 * footer with the Switch's button letters). Defined in menu.c. */
#ifndef ZNX_MENU_UI_H
#define ZNX_MENU_UI_H

#include <stdint.h>

/* ---- palette ---- */

#define C_FIELD    0x17161DFFu
#define C_CARD     0x1D1C24FFu   /* the detail panel: one step up */
#define C_SELECTED 0x25232DFFu   /* the selected row: two steps up */
#define C_HAIRLINE 0x2B2932FFu
#define C_KEY      0x3A3843FFu   /* button discs */
#define C_TEXT1    0xEDE9E1FFu
#define C_TEXT2    0xA7A3AEFFu
#define C_TEXT3    0x7D7986FFu
#define C_ACCENT   0xFFCC33FFu
#define C_READY    0x7DBE95FFu
#define C_MISSING  0xD9806FFFu

/* ---- layout (1280x720) ---- */

#define MARGIN 64
#define LIST_X 64
#define LIST_W 608
#define PANEL_X 704
#define PANEL_W 512
#define TITLE_BASE 152
#define LIST_Y 192
#define ROW_H 80
#define ROW_GAP 8
#define FOOTER_LINE 640
#define FOOTER_MID 676
#define SLANT 0.17f
#define PANEL_PAD 24
#define FIELD_STEP 32

struct hint { const char* key; const char* label; };
/* key: a letter, "+-" for the D-pad */
void znx_ui_footer(const struct hint* hints, int count, const char* left);
void znx_ui_title(const char* title);
/* A list row: title, and below it an optional status line with a coloured dot (dot 0: none); value on the right. */
void znx_ui_row(float y, float h, int selected, const char* title, uint32_t title_color,
                const char* status, uint32_t status_color, uint32_t dot, const char* value);

/* The detail panel: key/value lines, then a paragraph, then a quieter note. A field with mono set is an
 * identifier (an MD5, a code): small tabular figures. */
struct field { const char* key; const char* value; int mono; };
float znx_ui_panel(float y, const struct field* fields, int count, const char* paragraph, const char* note);
/* extra: height of a block the caller draws after the paragraph; its top is stored in *extra_y. */
float znx_ui_panel_ex(float y, const struct field* fields, int count, const char* paragraph, float extra,
                      float* extra_y, const char* note);

#endif
