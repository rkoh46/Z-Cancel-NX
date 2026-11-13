// SPDX-License-Identifier: GPL-3.0-or-later
/* The start screen's look, shared by its screens (menu.c), the online screens (online.c) and the lobby's
 * (lobby_entry.c): the console's own system look (System Settings): the theme's colours (draw.h), the
 * 1280x720 layout and the drawing pieces (header, list rows with thin lines and the pulsing selection
 * outline, the detail pane, the footer with the console's button glyphs). Defined in menu.c. */
#ifndef ZNX_MENU_UI_H
#define ZNX_MENU_UI_H

#include <stdint.h>

#include "draw.h"

/* ---- colours: the theme's roles ---- */

#define C_FIELD    (znx_theme()->background)
#define C_PANEL    (znx_theme()->panel)
#define C_SELECTED (znx_theme()->selected)
#define C_SEP      (znx_theme()->separator)   /* the header's and the footer's rule */
#define C_LINE     (znx_theme()->line)        /* between rows */
#define C_TEXT1    (znx_theme()->text)
#define C_TEXT2    (znx_theme()->text2)
#define C_TEXT3    (znx_theme()->text3)       /* disabled, not yet */
#define C_ACCENT   (znx_theme()->accent)      /* values, progress */
#define C_READY    (znx_theme()->ready)
#define C_MISSING  (znx_theme()->missing)

/* ---- layout (1280x720), System Settings' proportions ---- */

#define SEP_INSET 30        /* the rules stop this far from the screen's edges */
#define HEADER_LINE 88
#define FOOTER_LINE 647
#define FOOTER_MID 684
#define MARGIN 80           /* header title block, list and footer text start here */
#define RIGHT_EDGE 1200     /* pane text, version and hints end here */
#define LIST_X 80
#define LIST_W 560
#define LIST_Y 120
#define ROW_H 70
#define ROW_PAD 20          /* text inside a row */
#define PANE_X 680          /* the detail pane's surface, to the right edge */
#define PANEL_X 720         /* its text */
#define PANEL_W 480
#define FIELD_STEP 32

struct hint { const char* key; const char* label; };
/* The hints, right-aligned as the system orders them (A at the far right, then B, then the rest); key: a
 * button's letter, "+-" for the D-pad's left and right. left: small status text, or NULL. */
void znx_ui_footer(const struct hint* hints, int count, const char* left);
/* The page's name in the header, beside the mark. */
void znx_ui_title(const char* title);
/* A 70-pixel list row at y: title on the left, value on the right (NULL: none) with an optional dot
 * before it (0: none). The selected row gets the system's fill and outline. */
void znx_ui_row(float y, int selected, const char* title, uint32_t title_color, const char* value,
                uint32_t value_color, uint32_t dot);
/* The same row for a caller that draws its own content: lines, fill and outline only. */
void znx_ui_row_frame(float y, int selected);
/* A group's heading above its rows: the system's small bar and label. Returns its height (48). */
float znx_ui_section(float y, const char* label);
/* A thin bar on the right of a list that shows `visible` of `count` rows from `first`. */
void znx_ui_scrollbar(float y, int visible, int first, int count);
/* Steps of something that takes a while, as rows: finished ones say Done, the current one shows value
 * (may be NULL) in the accent; under the list a line of detail and, with progress 0..1, a thin bar
 * (progress < 0: none). Returns the bottom. */
float znx_ui_steps(float y, const char* const* names, int count, int current, const char* value, const char* detail,
                   float progress);

/* The detail pane: key/value lines, then a paragraph, then a quieter note. A field with mono set is an
 * identifier (an MD5, a code): small tabular figures; an MD5 takes two lines. */
struct field { const char* key; const char* value; int mono; };
float znx_ui_panel(float y, const struct field* fields, int count, const char* paragraph, const char* note);
/* extra: height of a block the caller draws after the paragraph; its top is stored in *extra_y. */
float znx_ui_panel_ex(float y, const struct field* fields, int count, const char* paragraph, float extra,
                      float* extra_y, const char* note);

/* The online match's label, over the game's finished picture (switch/overlay_switch.c): a small dark
 * rounded rectangle with white text, top left. Between znx_draw_begin_over and znx_draw_end. */
void znx_ui_overlay(const char* const* labels, const char* const* values, int count);

#endif
