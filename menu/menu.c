// SPDX-License-Identifier: GPL-3.0-or-later
/* See menu.h.
 *
 * Design (the project's standing rules, ~/.claude/skills/unslop-native): the logo's palette, a
 * near-black field (#17161D) and one warm yellow (#FFCC33) spent on the selection marker, progress
 * and the mark; one neutral ramp for surfaces, three text roles; one font (Barlow Semi Condensed
 * Medium) in four sizes; everything on a 4-pixel grid in a 1280x720 frame. The selection marker
 * is the mark's own slant (0.17). A plain list, a detail panel for the selected row, the Switch's
 * button letters at the bottom. No animation: the screen changes when a button is pressed. */

#if defined(__GNUC__) && !defined(__clang__)
/* File names and paths are cut to the size of their field on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "draw.h"
#include "online.h"
#include "patcher.h"
#include "settings.h"

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

enum screen { SCREEN_SCAN, SCREEN_HOME, SCREEN_GAME, SCREEN_SETTINGS, SCREEN_BUILD, SCREEN_MESSAGE };

#define HOME_ROWS (ZNX_GAME_COUNT + 1)   /* the games, then Settings */

struct znx_menu {
    char base[256];
    char version[32];
    struct znx_catalog catalog;
    int scanned, scan_drawn;
    struct znx_menu_settings settings;
    enum screen screen;
    int home_row, game, game_row, settings_row;
    /* message */
    char message_title[128], message_text[640], message_detail[640];
    enum screen message_return;
    /* build */
    struct znx_job job;
    int job_running, job_debug;
    /* input */
    uint32_t last_held;
    int wait_release;
    int repeat_frames;
    int quit_frames;
    /* result */
    char rom_path[512];
};

static const char* const l_rom_word[] = { "", "byte-swapped", "little-endian" };

/* ---- small helpers ---- */

static const char* file_name(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void show_message(struct znx_menu* m, const char* title, const char* text, const char* detail, enum screen back)
{
    snprintf(m->message_title, sizeof(m->message_title), "%s", title);
    snprintf(m->message_text, sizeof(m->message_text), "%s", text);
    snprintf(m->message_detail, sizeof(m->message_detail), "%s", detail != NULL ? detail : "");
    m->message_return = back;
    m->screen = SCREEN_MESSAGE;
}

struct znx_menu* znx_menu_create(const struct znx_menu_config* config)
{
    struct znx_menu* m = calloc(1, sizeof(*m));
    if (m == NULL)
        return NULL;
    snprintf(m->base, sizeof(m->base), "%s", config->base_dir);
    snprintf(m->version, sizeof(m->version), "%s", config->version != NULL ? config->version : "");
    znx_settings_load(m->base, &m->settings);
    m->screen = SCREEN_SCAN;
    m->wait_release = 1;
    return m;
}

void znx_menu_destroy(struct znx_menu* m)
{
    if (m == NULL)
        return;
    if (m->job_running) {
        m->job.cancel = 1;
        znx_job_finish(&m->job);
    }
    free(m);
}

void znx_menu_resume(struct znx_menu* m)
{
    m->wait_release = 1;
    m->quit_frames = 0;
}

void znx_menu_notice(struct znx_menu* m, const char* title, const char* text, const char* detail)
{
    show_message(m, title, text, detail, m->scanned ? SCREEN_HOME : SCREEN_SCAN);
}

const char* znx_menu_rom_path(const struct znx_menu* m) { return m->rom_path; }
int znx_menu_busy(const struct znx_menu* m) { return m->screen == SCREEN_BUILD && !m->job_debug; }
int znx_menu_game(const struct znx_menu* m) { return m->game; }
void znx_menu_get_settings(const struct znx_menu* m, struct znx_menu_settings* s) { *s = m->settings; }

void znx_menu_debug_build(struct znx_menu* m, int game, int downloads, int step, uint64_t done, uint64_t total)
{
    memset(&m->job, 0, sizeof(m->job));
    m->job.game = game;
    m->job.downloads = downloads;
    m->job.step = step;
    m->job.status = ZNX_JOB_RUNNING;
    m->job.done = done;
    m->job.total = total;
    m->game = game;
    m->job_debug = 1;
    m->screen = SCREEN_BUILD;
}

/* ---- actions ---- */

static void rescan(struct znx_menu* m)
{
    znx_catalog_scan(m->base, &m->catalog);
    m->scanned = 1;
}

static void start_build(struct znx_menu* m, int game)
{
    const struct znx_catalog* c = &m->catalog;
    memset(&m->job, 0, sizeof(m->job));
    m->job.game = game;
    snprintf(m->job.base, sizeof(m->job.base), "%s", m->base);
    snprintf(m->job.vanilla_path, sizeof(m->job.vanilla_path), "%s", c->rom_path[ZNX_GAME_VANILLA]);
    snprintf(m->job.patch_path, sizeof(m->job.patch_path), "%s", c->patch_path[game]);
    m->game = game;
    m->job_debug = 0;
    if (!znx_job_start(&m->job)) {
        show_message(m, m->job.error_title, m->job.error_text, NULL, SCREEN_HOME);
        return;
    }
    m->job_running = 1;
    m->screen = SCREEN_BUILD;
}

static void missing_rom_message(struct znx_menu* m)
{
    char detail[640] = "";
    size_t used = 0;
    const struct znx_catalog* c = &m->catalog;
    int shown = c->rejected_count < ZNX_MAX_REJECTED ? c->rejected_count : ZNX_MAX_REJECTED;
    for (int i = 0; i < shown && used < sizeof(detail); ++i) {
        const struct znx_rejected* r = &c->rejected[i];
        used += (size_t)snprintf(detail + used, sizeof(detail) - used, "%s%s\t%s%s%s", i ? "\n" : "", r->name,
                                 r->reason, r->md5[0] ? " \xC2\xB7 MD5 " : "", r->md5);
    }
    if (c->rejected_count > shown && used < sizeof(detail))
        snprintf(detail + used, sizeof(detail) - used, "\nand %d more", c->rejected_count - shown);
    show_message(m, c->rejected_count > 0 ? "None of these files is the USA ROM" : "Your Super Smash Bros. ROM is missing",
                 "Copy the USA version of Super Smash Bros. into sdmc:/switch/z-cancel-nx/ or its roms folder, as a "
                 ".z64, .n64 or .v64 file; the name does not matter. Then choose the game again. Remix and +EXTRA are "
                 "built from it.",
                 detail, SCREEN_HOME);
}

static void choose_game(struct znx_menu* m, int game)
{
    m->game = game;
    switch (m->catalog.state[game]) {
    case ZNX_STATE_READY:
        m->game_row = 0;
        m->screen = SCREEN_GAME;
        break;
    case ZNX_STATE_NEEDS_BUILD:
    case ZNX_STATE_NEEDS_PATCH:
        start_build(m, game);
        break;
    case ZNX_STATE_NEEDS_ROM:
        rescan(m);   /* the player may just have copied it */
        if (m->catalog.state[game] == ZNX_STATE_NEEDS_ROM)
            missing_rom_message(m);
        else
            choose_game(m, game);
        break;
    }
}

static void open_online(struct znx_menu* m)
{
    char title[128], text[640];
    char md5[33];
    const struct znx_game_info* info = znx_game_info(m->game);
    struct znx_online_request request;
    snprintf(md5, sizeof(md5), "%s", info->md5);
    request.game = m->game;
    request.title = info->title;
    request.rom_path = m->catalog.rom_path[m->game];
    request.rom_md5 = md5;
    request.base_dir = m->base;
    if (znx_online_open(&request, title, sizeof(title), text, sizeof(text)) == ZNX_ONLINE_MESSAGE)
        show_message(m, title, text, NULL, SCREEN_GAME);
}

static void save_settings(struct znx_menu* m)
{
    if (znx_settings_save(m->base, &m->settings) != 0)
        show_message(m, "The settings could not be saved",
                     "z-cancel-nx.ini could not be written. Check that the SD card is not full or locked.",
                     NULL, SCREEN_HOME);
    else
        m->screen = SCREEN_HOME;
}

/* ---- input ---- */

static uint32_t pressed_with_repeat(struct znx_menu* m, uint32_t held)
{
    const uint32_t directions = ZNX_BTN_UP | ZNX_BTN_DOWN | ZNX_BTN_LEFT | ZNX_BTN_RIGHT;
    uint32_t pressed = held & ~m->last_held;
    if ((held & directions) != 0 && (held & directions) == (m->last_held & directions)) {
        /* Held: again after 400 ms, then every 100 ms. */
        if (++m->repeat_frames >= 24 && (m->repeat_frames - 24) % 6 == 0)
            pressed |= held & directions;
    } else {
        m->repeat_frames = 0;
    }
    m->last_held = held;
    return pressed;
}

static int move(int row, int count, uint32_t pressed)
{
    if (pressed & ZNX_BTN_UP) row = row > 0 ? row - 1 : 0;
    if (pressed & ZNX_BTN_DOWN) row = row < count - 1 ? row + 1 : count - 1;
    return row;
}

enum znx_menu_action znx_menu_update(struct znx_menu* m, uint32_t held)
{
    const uint32_t combo = ZNX_BTN_ZL | ZNX_BTN_ZR | ZNX_BTN_R | ZNX_BTN_PLUS;
    uint32_t pressed;

    if (m->screen == SCREEN_SCAN) {
        /* The first frame drew "looking for your ROM"; the scan happens on the second. */
        if (!m->scan_drawn) { m->scan_drawn = 1; return ZNX_MENU_NONE; }
        rescan(m);
        m->screen = SCREEN_HOME;
        m->home_row = 0;
        for (int g = 0; g < ZNX_GAME_COUNT; ++g)
            if (m->catalog.state[g] == ZNX_STATE_READY) { m->home_row = g; break; }
    }

    if (m->wait_release) {
        if (held != 0)
            return ZNX_MENU_NONE;
        m->wait_release = 0;
        m->last_held = 0;
    }
    pressed = pressed_with_repeat(m, held);

    /* Minus, or L + R + Z + Start on a GameCube controller, held half a second: the same
     * combination that leaves a game leaves the app from its first screen. */
    if (m->screen == SCREEN_HOME && ((held & ZNX_BTN_MINUS) || (held & combo) == combo)) {
        if (++m->quit_frames >= 30)
            return ZNX_MENU_QUIT;
    } else {
        m->quit_frames = 0;
    }

    switch (m->screen) {
    case SCREEN_SCAN:
        break;
    case SCREEN_HOME:
        m->home_row = move(m->home_row, HOME_ROWS, pressed);
        if (pressed & ZNX_BTN_B)
            return ZNX_MENU_QUIT;
        if (pressed & ZNX_BTN_A) {
            if (m->home_row < ZNX_GAME_COUNT)
                choose_game(m, m->home_row);
            else {
                m->settings_row = 0;
                m->screen = SCREEN_SETTINGS;
            }
        }
        break;
    case SCREEN_GAME:
        m->game_row = move(m->game_row, 2, pressed);
        if (pressed & ZNX_BTN_B)
            m->screen = SCREEN_HOME;
        else if (pressed & ZNX_BTN_A) {
            if (m->game_row == 0) {
                snprintf(m->rom_path, sizeof(m->rom_path), "%s", m->catalog.rom_path[m->game]);
                m->home_row = m->game;
                m->screen = SCREEN_HOME;   /* where the player lands after the game */
                return ZNX_MENU_PLAY;
            }
            open_online(m);
        }
        break;
    case SCREEN_SETTINGS:
        m->settings_row = move(m->settings_row, 2, pressed);
        if (m->settings_row == 0 && (pressed & (ZNX_BTN_LEFT | ZNX_BTN_RIGHT | ZNX_BTN_A)))
            m->settings.sound = !m->settings.sound;
        if (m->settings_row == 1) {
            if (pressed & ZNX_BTN_LEFT) m->settings.stick_range--;
            if (pressed & ZNX_BTN_RIGHT) m->settings.stick_range++;
            if (pressed & ZNX_BTN_Y) m->settings.stick_range = ZNX_STICK_DEFAULT;
            if (m->settings.stick_range < ZNX_STICK_MIN) m->settings.stick_range = ZNX_STICK_MIN;
            if (m->settings.stick_range > ZNX_STICK_MAX) m->settings.stick_range = ZNX_STICK_MAX;
        }
        if (pressed & ZNX_BTN_B)
            save_settings(m);
        break;
    case SCREEN_BUILD:
        if (m->job_debug)
            break;
        if ((pressed & ZNX_BTN_B) && m->job.status == ZNX_JOB_RUNNING)
            m->job.cancel = 1;
        if (m->job.status != ZNX_JOB_RUNNING) {
            znx_job_finish(&m->job);
            m->job_running = 0;
            rescan(m);
            if (m->job.status == ZNX_JOB_DONE && m->catalog.state[m->job.game] == ZNX_STATE_READY) {
                m->game = m->job.game;
                m->game_row = 0;
                m->screen = SCREEN_GAME;
            } else if (m->job.status == ZNX_JOB_FAILED) {
                show_message(m, m->job.error_title, m->job.error_text, NULL, SCREEN_HOME);
            } else if (m->job.status == ZNX_JOB_DONE) {
                show_message(m, "The ROM was built but is not found",
                             "It was written to the roms folder and checked, but the folder scan does not see it.",
                             m->job.result_path, SCREEN_HOME);
            } else {
                m->screen = SCREEN_HOME;
            }
            m->wait_release = 1;
        }
        break;
    case SCREEN_MESSAGE:
        if (pressed & (ZNX_BTN_A | ZNX_BTN_B))
            m->screen = m->message_return;
        break;
    }
    return ZNX_MENU_NONE;
}

/* ---- drawing pieces ---- */

/* A button as the Switch draws it: a disc with its letter. */
static float draw_key(float x, const char* letter)
{
    const float d = 32;
    znx_draw_rect(x, FOOTER_MID - d / 2, d, d, d / 2, C_KEY);
    znx_draw_text(ZNX_FONT_BODY, x + d / 2, FOOTER_MID + znx_cap_height(ZNX_FONT_BODY) / 2, letter, C_TEXT1, ZNX_TEXT_CENTER);
    return d;
}

/* The D-pad, for "change": four arms around a gap (not a plus sign, which is the Plus button),
 * the left and right ones lit. */
static float draw_dpad_key(float x)
{
    const float d = 32, cx = x + d / 2, cy = FOOTER_MID, arm = 6, t = 6, gap = 2;
    znx_draw_rect(x, FOOTER_MID - d / 2, d, d, d / 2, C_KEY);
    znx_draw_rect(cx - gap - arm - t / 2 + 1, cy - t / 2, arm, t, 1, C_TEXT1);   /* left */
    znx_draw_rect(cx + gap + t / 2 - 1, cy - t / 2, arm, t, 1, C_TEXT1);         /* right */
    znx_draw_rect(cx - t / 2, cy - gap - arm - t / 2 + 1, t, arm, 1, C_TEXT3);   /* up */
    znx_draw_rect(cx - t / 2, cy + gap + t / 2 - 1, t, arm, 1, C_TEXT3);         /* down */
    return d;
}

struct hint { const char* key; const char* label; };

/* Hints right-aligned in the footer; key "+-" means the D-pad. */
static void draw_footer(const struct hint* hints, int count, const char* left)
{
    float x = 1280 - MARGIN;
    znx_draw_rect(MARGIN, FOOTER_LINE, 1280 - 2 * MARGIN, 1, 0, C_HAIRLINE);
    for (int i = count - 1; i >= 0; --i) {
        float w = znx_text_width(ZNX_FONT_BODY, hints[i].label, 0);
        x -= w;
        znx_draw_text(ZNX_FONT_BODY, x, FOOTER_MID + znx_cap_height(ZNX_FONT_BODY) / 2, hints[i].label, C_TEXT2, 0);
        x -= 12 + 32;
        if (!strcmp(hints[i].key, "+-")) draw_dpad_key(x);
        else draw_key(x, hints[i].key);
        x -= 32;
    }
    if (left != NULL)
        znx_draw_text(ZNX_FONT_SMALL, MARGIN, FOOTER_MID + znx_cap_height(ZNX_FONT_SMALL) / 2, left, C_TEXT3, 0);
}

static void draw_header(const struct znx_menu* m)
{
    znx_draw_mark(MARGIN, 40, 28, C_ACCENT);
    znx_draw_text(ZNX_FONT_ROW, MARGIN + 44, 68, "Z-Cancel NX", C_TEXT1, 0);
    if (m->version[0])
        znx_draw_text(ZNX_FONT_SMALL, 1280 - MARGIN, 68, m->version, C_TEXT3, ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
}

static void draw_title(const char* title)
{
    znx_draw_text(ZNX_FONT_TITLE, MARGIN, TITLE_BASE, title, C_TEXT1, 0);
}

/* A list row: title, and below it an optional status line with a coloured dot (dot 0: none). */
static void draw_row(float y, float h, int selected, const char* title, uint32_t title_color,
                     const char* status, uint32_t status_color, uint32_t dot, const char* value)
{
    const float x = LIST_X, text_x = LIST_X + 32;
    if (selected) {
        znx_draw_rect(x, y, LIST_W, h, 12, C_SELECTED);
        znx_draw_slant(x + 12, y + h / 2 - 20, 6, 40, SLANT, C_ACCENT);
    }
    if (status == NULL) {
        float base = y + h / 2 + znx_cap_height(ZNX_FONT_ROW) / 2;
        znx_draw_text(ZNX_FONT_ROW, text_x, base, title, title_color, 0);
        if (value != NULL)
            znx_draw_text(ZNX_FONT_BODY, x + LIST_W - 32, base, value, C_TEXT2, ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
        return;
    }
    znx_draw_text(ZNX_FONT_ROW, text_x, y + 36, title, title_color, 0);
    if (value != NULL)
        znx_draw_text(ZNX_FONT_ROW, x + LIST_W - 32, y + 36, value, C_TEXT1, ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
    {
        float sx = text_x;
        const float base = y + 64;
        if (dot != 0) {
            znx_draw_rect(sx, base - znx_cap_height(ZNX_FONT_SMALL) / 2 - 4, 8, 8, 4, dot);
            sx += 16;
        }
        char fitted[256];
        znx_text_fit(ZNX_FONT_SMALL, status, LIST_W - (sx - x) - 32, fitted, sizeof(fitted));
        znx_draw_text(ZNX_FONT_SMALL, sx, base, fitted, status_color, 0);
    }
}

/* The detail panel: key/value lines, then a paragraph, then a quieter note. Returns the panel's
 * bottom. A field with mono set is an identifier (an MD5): small tabular figures. */
struct field { const char* key; const char* value; int mono; };

#define PANEL_PAD 24
#define FIELD_STEP 32

static float panel_height(const struct field* fields, int count, const char* paragraph, float extra, const char* note)
{
    const float key_w = 88, value_w = PANEL_W - PANEL_PAD * 2 - key_w, text_w = PANEL_W - PANEL_PAD * 2;
    float height = PANEL_PAD;
    for (int i = 0; i < count; ++i)
        height += FIELD_STEP * (fields[i].mono ? 1 : znx_draw_wrapped(ZNX_FONT_BODY, 0, 0, value_w, fields[i].value, 0));
    if (paragraph != NULL)
        height += (count > 0 ? 24 : 0) + znx_draw_wrapped(ZNX_FONT_BODY, 0, 0, text_w, paragraph, 0) * znx_line_height(ZNX_FONT_BODY);
    height += extra;
    if (note != NULL)
        height += 12 + znx_draw_wrapped(ZNX_FONT_SMALL, 0, 0, text_w, note, 0) * znx_line_height(ZNX_FONT_SMALL);
    return height + PANEL_PAD - 4;
}

/* extra: height of a block the caller draws after the paragraph; its top is stored in *extra_y. */
static float draw_panel_ex(float y, const struct field* fields, int count, const char* paragraph, float extra,
                           float* extra_y, const char* note)
{
    const float x = PANEL_X, key_w = 88, value_x = PANEL_X + PANEL_PAD + key_w;
    const float value_w = PANEL_W - PANEL_PAD * 2 - key_w, text_w = PANEL_W - PANEL_PAD * 2;
    const float height = panel_height(fields, count, paragraph, extra, note);
    float cursor = y + PANEL_PAD;   /* top of the next line box; baselines sit 24 below */
    char fitted[256];

    znx_draw_rect(x, y, PANEL_W, height, 16, C_CARD);
    for (int i = 0; i < count; ++i) {
        const float base = cursor + 24;
        int lines = 1;
        znx_draw_text(ZNX_FONT_SMALL, x + PANEL_PAD, base, fields[i].key, C_TEXT3, 0);
        if (fields[i].mono) {
            znx_text_fit_flags(ZNX_FONT_SMALL, fields[i].value, value_w, fitted, sizeof(fitted), ZNX_TEXT_TABULAR);
            znx_draw_text(ZNX_FONT_SMALL, value_x, base, fitted, C_TEXT1, ZNX_TEXT_TABULAR);
        } else {
            lines = znx_draw_wrapped(ZNX_FONT_BODY, value_x, base, value_w, fields[i].value, C_TEXT1);
        }
        cursor += FIELD_STEP * lines;
    }
    if (paragraph != NULL) {
        if (count > 0) {
            cursor += 12;
            znx_draw_rect(x + PANEL_PAD, cursor, text_w, 1, 0, C_HAIRLINE);
            cursor += 12;
        }
        cursor += znx_draw_wrapped(ZNX_FONT_BODY, x + PANEL_PAD, cursor + 24, text_w, paragraph, C_TEXT2) *
                  znx_line_height(ZNX_FONT_BODY);
    }
    if (extra_y != NULL)
        *extra_y = cursor;
    cursor += extra;
    if (note != NULL) {
        cursor += 12;
        znx_draw_wrapped(ZNX_FONT_SMALL, x + PANEL_PAD, cursor + 18, text_w, note, C_TEXT3);
    }
    return y + height;
}

static float draw_panel(float y, const struct field* fields, int count, const char* paragraph, const char* note)
{
    return draw_panel_ex(y, fields, count, paragraph, 0, NULL, note);
}

static void status_of(const struct znx_menu* m, int game, char* text, size_t size, uint32_t* dot, uint32_t* color)
{
    const struct znx_game_info* info = znx_game_info(game);
    const struct znx_catalog* c = &m->catalog;
    char zip[32];
    *color = C_TEXT2;
    switch (c->state[game]) {
    case ZNX_STATE_READY:
        *dot = C_READY;
        snprintf(text, size, "Ready \xC2\xB7 %s", file_name(c->rom_path[game]));
        break;
    case ZNX_STATE_NEEDS_BUILD:
        *dot = C_TEXT3;
        snprintf(text, size, "Built from your ROM when you choose it");
        break;
    case ZNX_STATE_NEEDS_PATCH:
        *dot = C_TEXT3;
        znx_format_size(info->zip_size, zip, sizeof(zip));
        snprintf(text, size, "Needs the patch: A downloads it from GitHub, %s", zip);
        break;
    default:
        *dot = C_MISSING;
        snprintf(text, size, game == ZNX_GAME_VANILLA ? "Needs your ROM" : "Needs your Super Smash Bros. ROM first");
        break;
    }
}

static int format_index(const char* format)
{
    return !strcmp(format, "v64") ? 1 : !strcmp(format, "n64") ? 2 : 0;
}

/* ---- screens ---- */

static void draw_home(struct znx_menu* m)
{
    const struct znx_catalog* c = &m->catalog;
    const struct hint hints[] = { { "A", "Choose" }, { "B", "Quit" } };
    char status[256], size[32], value[64];
    uint32_t dot, color;
    float y = LIST_Y;

    draw_title("Choose a game");
    for (int g = 0; g < ZNX_GAME_COUNT; ++g) {
        status_of(m, g, status, sizeof(status), &dot, &color);
        draw_row(y, ROW_H, m->home_row == g, znx_game_info(g)->title, C_TEXT1, status, color, dot, NULL);
        y += ROW_H + ROW_GAP;
    }
    y += 16;
    snprintf(value, sizeof(value), "Sound %s \xC2\xB7 stick %d", m->settings.sound ? "on" : "off", m->settings.stick_range);
    draw_row(y, 64, m->home_row == ZNX_GAME_COUNT, "Settings", C_TEXT1, NULL, 0, 0, value);

    /* The panel: what the selected row is and what A does. */
    if (m->home_row < ZNX_GAME_COUNT) {
        const int g = m->home_row;
        const struct znx_game_info* info = znx_game_info(g);
        struct field fields[4];
        int count = 0;
        char paragraph[512], file[160], zip[32], need[32];
        const char* note = NULL;
        znx_format_size(info->size, size, sizeof(size));
        switch (c->state[g]) {
        case ZNX_STATE_READY:
            if (g == ZNX_GAME_VANILLA && format_index(c->vanilla_format) != 0)
                snprintf(file, sizeof(file), "%s, %s", file_name(c->rom_path[g]), l_rom_word[format_index(c->vanilla_format)]);
            else
                snprintf(file, sizeof(file), "%s", file_name(c->rom_path[g]));
            fields[count++] = (struct field){ "File", file, 0 };
            fields[count++] = (struct field){ "MD5", info->md5, 1 };
            fields[count++] = (struct field){ "Size", size, 0 };
            snprintf(paragraph, sizeof(paragraph), "A opens it: play on this console, or online.");
            note = "Online, every player needs a ROM with this MD5. RMG-K's lobby compares it.";
            break;
        case ZNX_STATE_NEEDS_BUILD:
            fields[count++] = (struct field){ "Patch", file_name(c->patch_path[g]), 0 };
            fields[count++] = (struct field){ "From", info->source, 0 };
            fields[count++] = (struct field){ "MD5", info->md5, 1 };
            snprintf(paragraph, sizeof(paragraph),
                     "A builds it from your ROM with this patch and checks the result. It needs %s on the SD card.", size);
            break;
        case ZNX_STATE_NEEDS_PATCH:
            znx_format_size(info->zip_size, zip, sizeof(zip));
            znx_format_size(info->zip_size + info->patch_size + info->size, need, sizeof(need));
            fields[count++] = (struct field){ "From", info->source, 0 };
            fields[count++] = (struct field){ "File", info->zip_name, 0 };
            fields[count++] = (struct field){ "MD5", info->md5, 1 };
            snprintf(paragraph, sizeof(paragraph),
                     "A downloads the release (%s) from GitHub, keeps only the patch and builds the ROM from yours. "
                     "It needs %s free while it works.", zip, need);
            snprintf(file, sizeof(file), "Without internet: copy %s into the patches folder.", info->patch_name);
            note = file;
            break;
        default:
            if (g == ZNX_GAME_VANILLA) {
                snprintf(paragraph, sizeof(paragraph),
                         "Copy the USA version of Super Smash Bros. into sdmc:/switch/z-cancel-nx/, as a .z64, .n64 "
                         "or .v64 file. Any name works: it is recognised by its MD5.");
                fields[count++] = (struct field){ "MD5", info->md5, 1 };
                fields[count++] = (struct field){ "Size", size, 0 };
            } else {
                snprintf(paragraph, sizeof(paragraph),
                         "%s is built on this console from your Super Smash Bros. ROM, so that comes first.", info->title);
                fields[count++] = (struct field){ "From", info->source, 0 };
                fields[count++] = (struct field){ "MD5", info->md5, 1 };
            }
            if (c->rejected_count > 0) {
                snprintf(file, sizeof(file), "%d file%s found but not used: A shows why.", c->rejected_count,
                         c->rejected_count == 1 ? " was" : "s were");
                note = file;
            }
            break;
        }
        if (c->patch_problem[g][0])
            note = c->patch_problem[g];
        draw_panel(LIST_Y, fields, count, paragraph, note);
    } else {
        char path[320];
        snprintf(path, sizeof(path), "%s/z-cancel-nx.ini", m->base);
        const struct field fields[] = { { "File", path, 0 } };
        draw_panel(LIST_Y, fields, 1, "Sound and how far a full stick tilt reaches. They apply to every game.", NULL);
    }
    draw_footer(hints, 2, m->base);
}

static void draw_game(struct znx_menu* m)
{
    const struct znx_game_info* info = znx_game_info(m->game);
    const struct hint hints[] = { { "A", "Choose" }, { "B", "Back" } };
    char size[32];
    const struct field fields[] = {
        { "File", file_name(m->catalog.rom_path[m->game]), 0 },
        { "MD5", info->md5, 1 },
        { "Size", size, 0 },
    };
    znx_format_size(info->size, size, sizeof(size));

    draw_title(info->title);
    draw_row(LIST_Y, ROW_H, m->game_row == 0, "Play", C_TEXT1, "On this console, up to four controllers", C_TEXT2, 0, NULL);
    draw_row(LIST_Y + ROW_H + ROW_GAP, ROW_H, m->game_row == 1, "Online play", C_TEXT1,
             "Against players on RMG-K. Not in this version yet", C_TEXT3, 0, NULL);
    draw_panel(LIST_Y, fields, 3,
               "To come back here from the game, hold \xE2\x88\x92\xC2\xA0(Minus), or L\xC2\xA0+\xC2\xA0R\xC2\xA0+\xC2\xA0Z\xC2\xA0+\xC2\xA0Start "
               "on a GameCube controller.",
               NULL);
    draw_footer(hints, 2, m->base);
}

/* The stick range scale in the panel: 60 to 100, RMG-K's 84 marked, the value in yellow. */
static void draw_stick_scale(float x, float y, float w, int value)
{
    const float track_y = y + 36;
    const float vx = x + (float)(value - ZNX_STICK_MIN) / (ZNX_STICK_MAX - ZNX_STICK_MIN) * w;
    const float dx = x + (float)(ZNX_STICK_DEFAULT - ZNX_STICK_MIN) / (ZNX_STICK_MAX - ZNX_STICK_MIN) * w;
    char label[8];
    znx_draw_rect(x, track_y, w, 4, 2, C_HAIRLINE);
    znx_draw_rect(x, track_y, vx - x, 4, 2, C_ACCENT);
    znx_draw_rect(dx - 1, track_y - 8, 2, 20, 1, C_TEXT3);
    znx_draw_slant(vx - 3, track_y - 12, 6, 28, SLANT, C_ACCENT);
    snprintf(label, sizeof(label), "%d", ZNX_STICK_MIN);
    znx_draw_text(ZNX_FONT_SMALL, x, track_y + 36, label, C_TEXT3, ZNX_TEXT_TABULAR);
    snprintf(label, sizeof(label), "%d", ZNX_STICK_MAX);
    znx_draw_text(ZNX_FONT_SMALL, x + w, track_y + 36, label, C_TEXT3, ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
    znx_draw_text(ZNX_FONT_SMALL, dx, track_y + 36, "84, RMG-K", C_TEXT3, ZNX_TEXT_CENTER | ZNX_TEXT_TABULAR);
}

static void draw_settings(struct znx_menu* m)
{
    const struct hint hints_sound[] = { { "+-", "Change" }, { "B", "Save and go back" } };
    const struct hint hints_stick[] = { { "+-", "Change" }, { "Y", "Default" }, { "B", "Save and go back" } };
    char value[16];
    char path[320];
    snprintf(path, sizeof(path), "%s/z-cancel-nx.ini", m->base);

    draw_title("Settings");
    draw_row(LIST_Y, ROW_H, m->settings_row == 0, "Sound", C_TEXT1, "Game sound on the TV or the console's speakers",
             C_TEXT2, 0, m->settings.sound ? "On" : "Off");
    snprintf(value, sizeof(value), "%d", m->settings.stick_range);
    draw_row(LIST_Y + ROW_H + ROW_GAP, ROW_H, m->settings_row == 1, "Stick range", C_TEXT1,
             "What a full tilt reads on the N64 stick", C_TEXT2, 0, value);
    if (m->settings_row == 0) {
        draw_panel(LIST_Y, NULL, 0, "Off runs the game silent. Sound does not change the game itself, so it never "
                   "matters for online play.", path);
    } else {
        float scale_y = 0;
        draw_panel_ex(LIST_Y, NULL, 0,
                      "What the game reads when a stick is pushed all the way. RMG-K's default is 84; lower makes "
                      "full tilt harder to reach, higher makes it easier.", 96, &scale_y, path);
        draw_stick_scale(PANEL_X + PANEL_PAD + 8, scale_y, PANEL_W - PANEL_PAD * 2 - 16, m->settings.stick_range);
    }
    if (m->settings_row == 0) draw_footer(hints_sound, 2, NULL);
    else draw_footer(hints_stick, 3, NULL);
}

static void draw_build(struct znx_menu* m)
{
    const struct znx_game_info* info = znx_game_info(m->game);
    const struct znx_job* job = &m->job;
    const struct hint hints[] = { { "B", "Cancel" } };
    char title[160], out[600], done_text[64], a[32], b[32];
    const char* names[ZNX_STEP_COUNT] = { "Download the patch", "Build the ROM", "Check the result" };
    float y = LIST_Y;
    const int step = job->step;
    const uint64_t done = job->done, total = job->total;

    snprintf(title, sizeof(title), "Building %s", info->title);
    draw_title(title);
    for (int s = job->downloads ? 0 : 1; s < ZNX_STEP_COUNT; ++s) {
        const int current = s == step, finished = s < step;
        const float h = current ? 104 : 64;
        const char* detail = NULL;
        char detail_text[160];
        if (current)
            znx_draw_rect(LIST_X, y, LIST_W, h, 12, C_SELECTED);
        if (current)
            znx_draw_slant(LIST_X + 12, y + 16, 6, 40, SLANT, C_ACCENT);
        znx_draw_text(ZNX_FONT_ROW, LIST_X + 32, y + 46 - (current ? 10 : 0) - (current ? 0 : 4), names[s],
                      current ? C_TEXT1 : finished ? C_TEXT2 : C_TEXT3, 0);
        if (finished)
            snprintf(done_text, sizeof(done_text), "Done");
        else if (current && total > 0)
            snprintf(done_text, sizeof(done_text), "%d%%", (int)(done * 100 / total));
        else
            done_text[0] = '\0';
        if (done_text[0])
            znx_draw_text(ZNX_FONT_BODY, LIST_X + LIST_W - 32, y + 36, done_text, finished ? C_TEXT3 : C_TEXT1,
                          ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
        if (current) {
            const float bar_w = LIST_W - 64;
            const float fill = total > 0 ? bar_w * (float)((double)done / (double)total) : 0;
            znx_draw_rect(LIST_X + 32, y + 56, bar_w, 6, 3, C_HAIRLINE);
            if (fill > 0)
                znx_draw_rect(LIST_X + 32, y + 56, fill < 6 ? 6 : fill, 6, 3, C_ACCENT);
            if (s == ZNX_STEP_DOWNLOAD) {
                znx_format_size(done, a, sizeof(a));
                znx_format_size(total, b, sizeof(b));
                snprintf(detail_text, sizeof(detail_text), "%s of %s from github.com", a, b);
                detail = detail_text;
            } else if (s == ZNX_STEP_BUILD) {
                detail = "Applying the patch to your ROM";
            } else {
                detail = "Comparing the MD5 with RMG-K's";
            }
            znx_draw_text(ZNX_FONT_SMALL, LIST_X + 32, y + 88, detail, C_TEXT2, ZNX_TEXT_TABULAR);
        }
        y += h + ROW_GAP;
    }
    snprintf(out, sizeof(out), "roms/%s", info->out_name);
    {
        const struct field fields[] = {
            { "From", info->source, 0 },
            { "Patch", info->patch_name, 0 },
            { "To", out, 0 },
            { "MD5", info->md5, 1 },
        };
        draw_panel(LIST_Y, fields, 4, NULL,
                   "The finished ROM is checked against this MD5, the one RMG-K players have. A ROM that differs is deleted.");
    }
    draw_footer(hints, 1, m->base);
}

static void draw_message(struct znx_menu* m)
{
    const struct hint hints[] = { { "A", "OK" } };
    const float width = 760;
    float y = TITLE_BASE;
    int lines = znx_draw_wrapped(ZNX_FONT_TITLE, MARGIN, y, width, m->message_title, C_TEXT1);
    y += (lines - 1) * znx_line_height(ZNX_FONT_TITLE) + 48;
    lines = znx_draw_wrapped(ZNX_FONT_BODY, MARGIN, y, width, m->message_text, C_TEXT2);
    y += lines * 32 + 16;
    if (m->message_detail[0]) {
        /* One entry per line: "name<TAB>details" shows the name, and under it the details small. */
        const char* p = m->message_detail;
        int entries = 0, two_line = 0;
        for (const char* q = p; *q;) {
            size_t n = strcspn(q, "\n");
            entries++;
            if (memchr(q, '\t', n) != NULL) two_line++;
            q += n;
            if (*q == '\n') q++;
        }
        znx_draw_rect(MARGIN, y, width + 48, 24 + entries * 32 + two_line * 24 + 8, 12, C_CARD);
        y += 24;
        while (*p) {
            char raw[256], line[256];
            size_t n = strcspn(p, "\n");
            char* tab;
            if (n >= sizeof(raw)) n = sizeof(raw) - 1;
            memcpy(raw, p, n);
            raw[n] = '\0';
            tab = strchr(raw, '\t');
            if (tab == NULL) {
                /* A plain line (a log line, a path): small. */
                znx_text_fit_flags(ZNX_FONT_SMALL, raw, width, line, sizeof(line), ZNX_TEXT_TABULAR);
                znx_draw_text(ZNX_FONT_SMALL, MARGIN + 24, y + 20, line, C_TEXT2, ZNX_TEXT_TABULAR);
                y += 32;
            } else {
                *tab = '\0';
                znx_text_fit(ZNX_FONT_BODY, raw, width, line, sizeof(line));
                znx_draw_text(ZNX_FONT_BODY, MARGIN + 24, y + 24, line, C_TEXT1, 0);
                y += 32;
            }
            if (tab != NULL) {
                znx_text_fit_flags(ZNX_FONT_SMALL, tab + 1, width, line, sizeof(line), ZNX_TEXT_TABULAR);
                znx_draw_text(ZNX_FONT_SMALL, MARGIN + 24, y + 16, line, C_TEXT2, ZNX_TEXT_TABULAR);
                y += 24;
            }
            p += n;
            if (*p == '\n') p++;
        }
    }
    draw_footer(hints, 1, NULL);
}

static void draw_scan(struct znx_menu* m)
{
    draw_title("Looking for your ROM");
    znx_draw_text(ZNX_FONT_BODY, MARGIN, LIST_Y + 24, m->base, C_TEXT2, 0);
}

void znx_menu_draw(struct znx_menu* m, unsigned framebuffer, int width, int height)
{
    znx_draw_begin(framebuffer, width, height, C_FIELD);
    draw_header(m);
    switch (m->screen) {
    case SCREEN_SCAN: draw_scan(m); break;
    case SCREEN_HOME: draw_home(m); break;
    case SCREEN_GAME: draw_game(m); break;
    case SCREEN_SETTINGS: draw_settings(m); break;
    case SCREEN_BUILD: draw_build(m); break;
    case SCREEN_MESSAGE: draw_message(m); break;
    }
    znx_draw_end();
}
