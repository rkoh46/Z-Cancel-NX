// SPDX-License-Identifier: GPL-3.0-or-later
/* See menu.h.
 *
 * Design: the console's own system look, so the program reads as part of the Switch (docs/PLAN.md
 * "Switch-style UI"). The two system themes, Basic White and Basic Black, following the console's
 * setting; the system font and its button glyphs, loaded from the console at run time; System Settings'
 * layout in a 1280x720 frame: the mark and the page's name in the header over a thin rule, a list of
 * 70-pixel rows with thin lines on the left, values at the right in the accent colour, a detail pane on
 * the right, the button hints right-aligned over the footer's rule. The selected row has the system's
 * fill and its outline, which pulses between two colours: the one animation. */

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
#include "menu_ui.h"
#include "online.h"
#include "patcher.h"
#include "settings.h"

enum screen { SCREEN_SCAN, SCREEN_HOME, SCREEN_GAME, SCREEN_SETTINGS, SCREEN_BUILD, SCREEN_MESSAGE, SCREEN_ONLINE, SCREEN_GLYPHS };

#define SETTINGS_ROWS 8   /* sound, stick range, L button, C stick | name, input delay, public list, overlay */

#define HOME_ROWS (ZNX_GAME_COUNT + 1)   /* the games, then Settings */

struct znx_menu {
    char base[256];
    char version[32];
    struct znx_catalog catalog;
    int scanned, scan_drawn, debug_glyphs;
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
    /* online */
    znx_keyboard_fn keyboard;
    struct znx_online_request online;
    char online_md5[33];
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
    m->keyboard = config->keyboard;
    znx_settings_load(m->base, &m->settings);
    m->debug_glyphs = config->debug_glyphs || m->settings.debug_glyphs;
    {
        /* the theme: the ini's theme key if it names one, else the console's setting, else Basic White */
        char line[200];
        int dark = 0;
        if (!strcmp(m->settings.theme, "dark") || !strcmp(m->settings.theme, "light")) {
            dark = m->settings.theme[0] == 'd';
            snprintf(line, sizeof(line), "menu: theme %s (theme=%s in z-cancel-nx.ini)", dark ? "dark" : "light", m->settings.theme);
        } else if (config->console_theme >= 0) {
            dark = config->console_theme != 0;
            snprintf(line, sizeof(line), "menu: theme %s (console setting)", dark ? "dark" : "light");
        } else {
            snprintf(line, sizeof(line), "menu: theme light (console setting unavailable: %s)",
                     config->theme_note != NULL ? config->theme_note : "not asked");
        }
        znx_theme_set(dark);
        znx_log_line(line);
    }
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
    znx_online_close();
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
    /* five fit on the message screen under the text */
    const int most = ZNX_MAX_REJECTED < 5 ? ZNX_MAX_REJECTED : 5;
    int shown = c->rejected_count < most ? c->rejected_count : most;
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

static void online_request(struct znx_menu* m, int game)
{
    const struct znx_game_info* info = znx_game_info(game);
    snprintf(m->online_md5, sizeof(m->online_md5), "%s", info->md5);
    m->online.game = game;
    m->online.title = info->title;
    m->online.goodname = znx_online_goodname(game);
    m->online.rom_path = m->catalog.rom_path[game];
    m->online.rom_md5 = m->online_md5;
    m->online.base_dir = m->base;
    m->online.settings = &m->settings;
    m->online.keyboard = m->keyboard;
}

static void open_online(struct znx_menu* m)
{
    online_request(m, m->game);
    znx_online_enter(&m->online);
    m->screen = SCREEN_ONLINE;
}

void znx_menu_online_finished(struct znx_menu* m)
{
    char title[128], text[640];
    m->wait_release = 1;
    m->quit_frames = 0;
    if (znx_online_after_game(title, sizeof(title), text, sizeof(text)) == ZNX_ONLINE_STAY)
        m->screen = SCREEN_ONLINE;
    else
        /* back to the lobby's room if it is still open (how a lobby match ended), else the game's screen */
        show_message(m, title, text, NULL, znx_online_connected() ? SCREEN_ONLINE : SCREEN_GAME);
}

void znx_menu_debug_online(struct znx_menu* m, int game, const char* which)
{
    m->game = game;
    online_request(m, game);
    znx_online_debug(&m->online, which);
    m->screen = SCREEN_ONLINE;
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
        m->screen = m->debug_glyphs ? SCREEN_GLYPHS : SCREEN_HOME;
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
    case SCREEN_GLYPHS:
        if (pressed & (ZNX_BTN_A | ZNX_BTN_B))
            m->screen = SCREEN_HOME;
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
        m->settings_row = move(m->settings_row, SETTINGS_ROWS, pressed);
        switch (m->settings_row) {
        case 0:
            if (pressed & (ZNX_BTN_LEFT | ZNX_BTN_RIGHT | ZNX_BTN_A))
                m->settings.sound = !m->settings.sound;
            break;
        case 1:
            if (pressed & ZNX_BTN_LEFT) m->settings.stick_range--;
            if (pressed & ZNX_BTN_RIGHT) m->settings.stick_range++;
            if (pressed & ZNX_BTN_Y) m->settings.stick_range = ZNX_STICK_DEFAULT;
            if (m->settings.stick_range < ZNX_STICK_MIN) m->settings.stick_range = ZNX_STICK_MIN;
            if (m->settings.stick_range > ZNX_STICK_MAX) m->settings.stick_range = ZNX_STICK_MAX;
            break;
        case 2:
            if (pressed & (ZNX_BTN_LEFT | ZNX_BTN_RIGHT | ZNX_BTN_A))
                m->settings.l_shield = !m->settings.l_shield;
            break;
        case 3:
            if (pressed & (ZNX_BTN_RIGHT | ZNX_BTN_A)) m->settings.c_mode = (m->settings.c_mode + 1) % 3;
            if (pressed & ZNX_BTN_LEFT) m->settings.c_mode = (m->settings.c_mode + 2) % 3;
            break;
        case 4:
            if ((pressed & ZNX_BTN_A) && m->keyboard != NULL) {
                char name[64];
                if (m->keyboard(0, "Your name", "Lobby: up to 20 letters, numbers, _ - .", m->settings.name, name, sizeof(name))) {
                    size_t n = strlen(name);
                    while (n > 0 && name[n - 1] == ' ') name[--n] = '\0';
                    if (name[strspn(name, " ")] != '\0')
                        snprintf(m->settings.name, sizeof(m->settings.name), "%s", name + strspn(name, " "));
                }
                m->wait_release = 1;
            }
            break;
        case 5:
            /* Auto, 1..9 (RMG-K allows 0 too; a frame of delay is the least that keeps rollbacks rare) */
            if (pressed & ZNX_BTN_LEFT) m->settings.input_delay = m->settings.input_delay > 0 ? m->settings.input_delay - 1 : 0;
            if (pressed & ZNX_BTN_RIGHT) m->settings.input_delay = m->settings.input_delay < 9 ? m->settings.input_delay + 1 : 9;
            if (pressed & ZNX_BTN_Y) m->settings.input_delay = 0;
            break;
        case 6:
            if (pressed & (ZNX_BTN_LEFT | ZNX_BTN_RIGHT | ZNX_BTN_A))
                m->settings.public_list = !m->settings.public_list;
            break;
        default:
            if (pressed & (ZNX_BTN_LEFT | ZNX_BTN_RIGHT | ZNX_BTN_A))
                m->settings.net_overlay = !m->settings.net_overlay;
            break;
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
        if (m->message_return == SCREEN_ONLINE && znx_online_match_ready()) {
            /* the lobby's host started a match while a message was up: the other players are booting */
            m->screen = SCREEN_ONLINE;
            snprintf(m->rom_path, sizeof(m->rom_path), "%s", m->catalog.rom_path[m->game]);
            return ZNX_MENU_PLAY_ONLINE;
        }
        if (pressed & (ZNX_BTN_A | ZNX_BTN_B))
            m->screen = m->message_return;
        break;
    case SCREEN_ONLINE: {
        char title[128], text[640];
        switch (znx_online_update(pressed, held, title, sizeof(title), text, sizeof(text))) {
        case ZNX_ONLINE_BACK:
            m->screen = SCREEN_GAME;
            break;
        case ZNX_ONLINE_MESSAGE:
            show_message(m, title, text, NULL, SCREEN_ONLINE);
            m->wait_release = 1;   /* the system keyboard may have been up: start from no buttons */
            break;
        case ZNX_ONLINE_PLAY:
            snprintf(m->rom_path, sizeof(m->rom_path), "%s", m->catalog.rom_path[m->game]);
            return ZNX_MENU_PLAY_ONLINE;
        default:
            break;
        }
        break;
    }
    }
    return ZNX_MENU_NONE;
}

/* ---- drawing pieces ---- */

/* The selected row's outline is drawn after everything else, so the lines of the rows next to it do not
 * cross it. */
static struct { int on; float y; } l_focus;

static float mid_base(int font, float cy)
{
    return cy + znx_cap_height(font) / 2;
}

static int hint_rank(const char* key)
{
    return !strcmp(key, "A") ? 0 : !strcmp(key, "B") ? 1 : !strcmp(key, "X") ? 2 : !strcmp(key, "Y") ? 3 : 4;
}

void znx_ui_footer(const struct hint* hints, int count, const char* left)
{
    const float base = mid_base(ZNX_FONT_ROW, FOOTER_MID);
    float x = RIGHT_EDGE;
    znx_draw_rect(SEP_INSET, FOOTER_LINE, 1280 - 2 * SEP_INSET, 1, 0, C_SEP);
    /* as the system orders them: A at the far right, B to its left, then the others */
    for (int rank = 0; rank <= 4; ++rank)
        for (int i = 0; i < count; ++i) {
            const char* key = !strcmp(hints[i].key, "+-") ? "LR" : hints[i].key;
            if (hint_rank(hints[i].key) != rank)
                continue;
            x -= znx_text_width(ZNX_FONT_ROW, hints[i].label, 0);
            znx_draw_text(ZNX_FONT_ROW, x, base, hints[i].label, C_TEXT1, 0);
            x -= 8 + znx_draw_button(ZNX_FONT_ROW, 0, base, key, C_TEXT1, C_FIELD, 0);
            znx_draw_button(ZNX_FONT_ROW, x, base, key, C_TEXT1, C_FIELD, 1);
            x -= 32;
        }
    if (left != NULL && left[0]) {
        char fitted[256];
        znx_text_fit(ZNX_FONT_SMALL, left, x - MARGIN, fitted, sizeof(fitted));
        znx_draw_text(ZNX_FONT_SMALL, MARGIN, mid_base(ZNX_FONT_SMALL, FOOTER_MID), fitted, C_TEXT2, 0);
    }
}

/* The header: the mark on its tile, as system software shows an application's icon, the version, the rule. */
static void draw_header(const struct znx_menu* m)
{
    znx_draw_rect(MARGIN, 24, 40, 40, 6, 0x17161DFFu);     /* the logo's field (assets/logo.png) */
    znx_draw_mark(MARGIN + 10, 33, 22.5f, 0xFFCC33FFu);    /* and its yellow */
    if (m->version[0])
        znx_draw_text(ZNX_FONT_SMALL, RIGHT_EDGE, mid_base(ZNX_FONT_SMALL, 44), m->version, C_TEXT2, ZNX_TEXT_RIGHT);
    znx_draw_rect(SEP_INSET, HEADER_LINE, 1280 - 2 * SEP_INSET, 1, 0, C_SEP);
}

void znx_ui_title(const char* title)
{
    char fitted[200];
    znx_text_fit(ZNX_FONT_TITLE, title, RIGHT_EDGE - 120 - (MARGIN + 56), fitted, sizeof(fitted));
    znx_draw_text(ZNX_FONT_TITLE, MARGIN + 56, mid_base(ZNX_FONT_TITLE, 44), fitted, C_TEXT1, 0);
}

void znx_ui_row_frame(float y, int selected)
{
    znx_draw_rect(LIST_X, y, LIST_W, 1, 0, C_LINE);
    znx_draw_rect(LIST_X, y + ROW_H, LIST_W, 1, 0, C_LINE);
    if (selected) {
        znx_draw_rect(LIST_X, y + 1, LIST_W, ROW_H - 1, 2, C_SELECTED);
        l_focus.on = 1;
        l_focus.y = y;
    }
}

static void flush_focus(void)
{
    if (l_focus.on)   /* 5 pixels wide around the fill, over the row's two lines */
        znx_draw_ring(LIST_X - 5, l_focus.y - 4, LIST_W + 10, ROW_H + 9, 6, 5, znx_theme_highlight());
    l_focus.on = 0;
}

void znx_ui_row(float y, int selected, const char* title, uint32_t title_color, const char* value,
                uint32_t value_color, uint32_t dot)
{
    const float base = mid_base(ZNX_FONT_ROW, y + ROW_H / 2), right = LIST_X + LIST_W - ROW_PAD;
    float taken = 0;
    char fitted[256];
    znx_ui_row_frame(y, selected);
    if (value != NULL && value[0]) {
        /* the value keeps its place; a long title gives way */
        znx_text_fit(ZNX_FONT_ROW, value, LIST_W - 2 * ROW_PAD - 160, fitted, sizeof(fitted));
        taken = znx_draw_text(ZNX_FONT_ROW, right, base, fitted, value_color, ZNX_TEXT_RIGHT) + 24;
        if (dot != 0) {
            znx_draw_rect(right - taken + 8, y + ROW_H / 2 - 4, 8, 8, 4, dot);
            taken += 16;
        }
    }
    znx_text_fit(ZNX_FONT_ROW, title, LIST_W - 2 * ROW_PAD - taken, fitted, sizeof(fitted));
    znx_draw_text(ZNX_FONT_ROW, LIST_X + ROW_PAD, base, fitted, title_color, 0);
}

float znx_ui_section(float y, const char* label)
{
    znx_draw_rect(LIST_X, y + 12, 4, 24, 0, C_TEXT2);
    znx_draw_text(ZNX_FONT_SMALL, LIST_X + 16, mid_base(ZNX_FONT_SMALL, y + 24), label, C_TEXT1, 0);
    return 48;
}

void znx_ui_scrollbar(float y, int visible, int first, int count)
{
    const float track = visible * ROW_H, x = LIST_X + LIST_W + 12;
    if (count <= visible)
        return;
    znx_draw_rect(x, y + track * first / count, 4, track * visible / count, 2, C_TEXT3);
}

float znx_ui_steps(float y, const char* const* names, int count, int current, const char* value, const char* detail,
                   float progress)
{
    for (int i = 0; i < count; ++i, y += ROW_H) {
        if (i < current)
            znx_ui_row(y, 0, names[i], C_TEXT2, "Done", C_TEXT2, 0);
        else if (i == current)
            znx_ui_row(y, 0, names[i], C_TEXT1, value, C_ACCENT, 0);
        else
            znx_ui_row(y, 0, names[i], C_TEXT3, NULL, 0, 0);
    }
    y += 24;
    if (detail != NULL && detail[0]) {
        char fitted[256];
        znx_text_fit(ZNX_FONT_SMALL, detail, LIST_W - 2 * ROW_PAD, fitted, sizeof(fitted));
        znx_draw_text(ZNX_FONT_SMALL, LIST_X + ROW_PAD, y + 16, fitted, C_TEXT2, 0);
        y += 32;
    }
    if (progress >= 0) {
        const float w = LIST_W - 2 * ROW_PAD, fill = w * (progress > 1 ? 1 : progress);
        znx_draw_rect(LIST_X + ROW_PAD, y, w, 4, 2, C_LINE);
        if (fill > 0)
            znx_draw_rect(LIST_X + ROW_PAD, y, fill < 4 ? 4 : fill, 4, 2, C_ACCENT);
        y += 4;
    }
    return y;
}

static int is_md5(const char* text)
{
    return strlen(text) == 32 && strspn(text, "0123456789ABCDEFabcdef") == 32;
}

float znx_ui_panel_ex(float y, const struct field* fields, int count, const char* paragraph, float extra,
                      float* extra_y, const char* note)
{
    const float x = PANEL_X, key_w = 120, value_x = PANEL_X + key_w, value_w = PANEL_W - key_w;
    float cursor = y + 4;   /* top of the next line box; baselines sit 24 below */
    char fitted[256];

    /* the pane's surface, from the header's rule to the footer's */
    znx_draw_rect(PANE_X, HEADER_LINE + 1, 1280 - PANE_X, FOOTER_LINE - HEADER_LINE - 1, 0, C_PANEL);
    for (int i = 0; i < count; ++i) {
        const float base = cursor + 24;
        znx_draw_text(ZNX_FONT_SMALL, x, base, fields[i].key, C_TEXT2, 0);
        if (fields[i].mono && is_md5(fields[i].value)) {
            /* 32 hexadecimal digits: two lines of two groups, so nothing is cut */
            const char* v = fields[i].value;
            snprintf(fitted, sizeof(fitted), "%.8s %.8s", v, v + 8);
            znx_draw_text(ZNX_FONT_SMALL, value_x, base, fitted, C_TEXT1, ZNX_TEXT_TABULAR);
            snprintf(fitted, sizeof(fitted), "%.8s %.8s", v + 16, v + 24);
            znx_draw_text(ZNX_FONT_SMALL, value_x, base + 24, fitted, C_TEXT1, ZNX_TEXT_TABULAR);
            cursor += FIELD_STEP + 24;
        } else if (fields[i].mono) {
            /* an address, port, code or server: one line, cut to fit rather than wrapped, in plain figures */
            znx_text_fit_flags(ZNX_FONT_BODY, fields[i].value, value_w, fitted, sizeof(fitted), 0);
            znx_draw_text(ZNX_FONT_BODY, value_x, base, fitted, C_TEXT1, 0);
            cursor += FIELD_STEP;
        } else {
            const int lines = znx_draw_wrapped(ZNX_FONT_BODY, value_x, base, value_w, fields[i].value, C_TEXT1);
            cursor += FIELD_STEP + (lines > 1 ? (lines - 1) * znx_line_height(ZNX_FONT_BODY) : 0);
        }
    }
    if (paragraph != NULL && paragraph[0]) {
        if (count > 0) {
            cursor += 12;
            znx_draw_rect(x, cursor, PANEL_W, 1, 0, C_LINE);
            cursor += 12;
        }
        cursor += znx_draw_wrapped(ZNX_FONT_BODY, x, cursor + 24, PANEL_W, paragraph, C_TEXT1) *
                  znx_line_height(ZNX_FONT_BODY) + 4;
    }
    if (extra_y != NULL)
        *extra_y = cursor;
    cursor += extra;
    if (note != NULL && note[0]) {
        cursor += 12;
        cursor += znx_draw_wrapped(ZNX_FONT_SMALL, x, cursor + 20, PANEL_W, note, C_TEXT2) * znx_line_height(ZNX_FONT_SMALL);
    }
    return cursor;
}

float znx_ui_panel(float y, const struct field* fields, int count, const char* paragraph, const char* note)
{
    return znx_ui_panel_ex(y, fields, count, paragraph, 0, NULL, note);
}

void znx_ui_overlay(const char* const* labels, const char* const* values, int count)
{
    /* Fixed colours: it sits on the game's picture or its black margin, whatever the theme. */
    const float x = 8, y = 8, pad = 12, row = 24;
    float w = 0;
    for (int i = 0; i < count; ++i) {
        const float line = znx_text_width(ZNX_FONT_SMALL, labels[i], 0) + 12 + znx_text_width(ZNX_FONT_SMALL, values[i], 0);
        if (line > w) w = line;
    }
    w += 2 * pad;
    znx_draw_rect(x, y, w, count * row + 16, 8, 0x2D2D2DD0u);
    for (int i = 0; i < count; ++i) {
        const float base = y + 8 + i * row + 18;
        znx_draw_text(ZNX_FONT_SMALL, x + pad, base, labels[i], 0xC8C8C8FFu, 0);
        znx_draw_text(ZNX_FONT_SMALL, x + w - pad, base, values[i], 0xFFFFFFFFu, ZNX_TEXT_RIGHT);
    }
}

/* The value at the right of a game's row: its state in a word. */
static void status_of(const struct znx_menu* m, int game, const char** text, uint32_t* dot, uint32_t* color)
{
    *color = C_TEXT2;
    *dot = 0;
    switch (m->catalog.state[game]) {
    case ZNX_STATE_READY:
        *dot = C_READY;
        *color = C_TEXT1;
        *text = "Ready";
        break;
    case ZNX_STATE_NEEDS_BUILD:
        *text = "Not built";
        break;
    case ZNX_STATE_NEEDS_PATCH:
        *text = "No patch";
        break;
    default:
        *dot = C_MISSING;
        *text = "No ROM";
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
    const struct hint hints[] = { { "A", "OK" }, { "B", "Quit" } };
    const char* status;
    char size[32];
    uint32_t dot, color;
    float y = LIST_Y;

    znx_ui_title("Z-Cancel NX");
    for (int g = 0; g < ZNX_GAME_COUNT; ++g) {
        status_of(m, g, &status, &dot, &color);
        znx_ui_row(y, m->home_row == g, znx_game_info(g)->title, C_TEXT1, status, color, dot);
        y += ROW_H;
    }
    /* Settings follows straight on: a gap between two rules read as an empty row */
    znx_ui_row(y, m->home_row == ZNX_GAME_COUNT, "Settings", C_TEXT1, NULL, 0, 0);

    /* The pane: what the selected row is and what A does. */
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
        znx_ui_panel(LIST_Y, fields, count, paragraph, note);
    } else {
        char path[320], summary[160];
        snprintf(path, sizeof(path), "%s/z-cancel-nx.ini", m->base);
        snprintf(summary, sizeof(summary), "%s \xC2\xB7 sound %s \xC2\xB7 stick %d", m->settings.name,
                 m->settings.sound ? "on" : "off", m->settings.stick_range);
        const struct field fields[] = { { "Now", summary, 0 }, { "File", path, 0 } };
        znx_ui_panel(LIST_Y, fields, 2, "Sound, how far a full stick tilt reaches, and your name and delay for online play.", NULL);
    }
    znx_ui_footer(hints, 2, m->base);
}

static void draw_game(struct znx_menu* m)
{
    const struct znx_game_info* info = znx_game_info(m->game);
    const struct hint hints[] = { { "A", "OK" }, { "B", "Back" } };
    char size[32];
    const struct field fields[] = {
        { "File", file_name(m->catalog.rom_path[m->game]), 0 },
        { "MD5", info->md5, 1 },
        { "Size", size, 0 },
    };
    znx_format_size(info->size, size, sizeof(size));

    znx_ui_title(info->title);
    znx_ui_row(LIST_Y, m->game_row == 0, "Play", C_TEXT1, NULL, 0, 0);
    znx_ui_row(LIST_Y + ROW_H, m->game_row == 1, "Online play", C_TEXT1, NULL, 0, 0);
    znx_ui_panel(LIST_Y, fields, 3,
                 m->game_row == 0 ? "On this console, with up to four controllers."
                                  : "Against players on RMG-K or another Switch, with rollback.",
                 "To come back here from the game, hold \xE2\x88\x92\xC2\xA0(Minus), or L\xC2\xA0+\xC2\xA0R\xC2\xA0+\xC2\xA0Z\xC2\xA0+\xC2\xA0Start "
                 "on a GameCube controller.");
    znx_ui_footer(hints, 2, m->base);
}

/* The stick range scale in the pane: 60 to 100, RMG-K's 84 marked, the value in the accent. */
static void draw_stick_scale(float x, float y, float w, int value)
{
    const float track_y = y + 36;
    const float vx = x + (float)(value - ZNX_STICK_MIN) / (ZNX_STICK_MAX - ZNX_STICK_MIN) * w;
    const float dx = x + (float)(ZNX_STICK_DEFAULT - ZNX_STICK_MIN) / (ZNX_STICK_MAX - ZNX_STICK_MIN) * w;
    char label[8];
    znx_draw_rect(x, track_y, w, 4, 2, C_LINE);
    znx_draw_rect(x, track_y, vx - x, 4, 2, C_ACCENT);
    znx_draw_rect(dx - 1, track_y - 6, 2, 16, 1, C_TEXT2);
    znx_draw_rect(vx - 8, track_y - 6, 16, 16, 8, C_ACCENT);
    snprintf(label, sizeof(label), "%d", ZNX_STICK_MIN);
    znx_draw_text(ZNX_FONT_SMALL, x, track_y + 40, label, C_TEXT2, 0);
    snprintf(label, sizeof(label), "%d", ZNX_STICK_MAX);
    znx_draw_text(ZNX_FONT_SMALL, x + w, track_y + 40, label, C_TEXT2, ZNX_TEXT_RIGHT);
    znx_draw_text(ZNX_FONT_SMALL, dx, track_y + 40, "84, RMG-K", C_TEXT2, ZNX_TEXT_CENTER);
}

static void switch_row(float y, int selected, const char* title, int on)
{
    /* as System Settings shows a switch: On in the accent, Off quiet */
    znx_ui_row(y, selected, title, C_TEXT1, on ? "On" : "Off", on ? C_ACCENT : C_TEXT2, 0);
}

static void draw_settings(struct znx_menu* m)
{
    const struct hint hints_toggle[] = { { "+-", "Change" }, { "B", "Save and go back" } };
    const struct hint hints_stick[] = { { "+-", "Change" }, { "Y", "Default" }, { "B", "Save and go back" } };
    const struct hint hints_name[] = { { "A", "Change" }, { "B", "Save and go back" } };
    const struct hint hints_delay[] = { { "+-", "Change" }, { "Y", "Auto" }, { "B", "Save and go back" } };
    const struct znx_menu_settings* s = &m->settings;
    char stick[16], delay[32], path[320];
    float y = LIST_Y;
    snprintf(path, sizeof(path), "%s/z-cancel-nx.ini", m->base);
    snprintf(stick, sizeof(stick), "%d", s->stick_range);
    if (s->input_delay == 0) snprintf(delay, sizeof(delay), "Auto");
    else snprintf(delay, sizeof(delay), "%d frame%s", s->input_delay, s->input_delay == 1 ? "" : "s");

    znx_ui_title("Settings");
    /* Eight rows and a section are taller than the list: with an Online row selected the list is shown from
     * the Online section, so this console's rows scroll out above (a row is drawn only if it fits whole). */
    y -= m->settings_row >= 4 ? 4 * ROW_H + 16 : 0;
#define SETTINGS_FITS(top, h) ((top) >= LIST_Y - 1 && (top) + (h) <= FOOTER_LINE + 1)
    if (SETTINGS_FITS(y, ROW_H)) switch_row(y, m->settings_row == 0, "Sound", s->sound);
    y += ROW_H;
    if (SETTINGS_FITS(y, ROW_H)) znx_ui_row(y, m->settings_row == 1, "Stick range", C_TEXT1, stick, C_ACCENT, 0);
    y += ROW_H;
    if (SETTINGS_FITS(y, ROW_H))
        znx_ui_row(y, m->settings_row == 2, "L button", C_TEXT1, s->l_shield ? "Shield" : "Taunt", C_ACCENT, 0);
    y += ROW_H;
    if (SETTINGS_FITS(y, ROW_H))
        znx_ui_row(y, m->settings_row == 3, "C stick", C_TEXT1,
                   s->c_mode == 2 ? "Attacks, every controller" : s->c_mode == 1 ? "Attacks, GameCube" : "C buttons",
                   C_ACCENT, 0);
    y += ROW_H + 16;
    y += SETTINGS_FITS(y, 48) ? znx_ui_section(y, "Online") : 48;
    if (SETTINGS_FITS(y, ROW_H)) znx_ui_row(y, m->settings_row == 4, "Name", C_TEXT1, s->name, C_ACCENT, 0);
    y += ROW_H;
    if (SETTINGS_FITS(y, ROW_H)) znx_ui_row(y, m->settings_row == 5, "Input delay", C_TEXT1, delay, C_ACCENT, 0);
    y += ROW_H;
    if (SETTINGS_FITS(y, ROW_H)) switch_row(y, m->settings_row == 6, "Public list", s->public_list);
    y += ROW_H;
    if (SETTINGS_FITS(y, ROW_H)) switch_row(y, m->settings_row == 7, "Match overlay", s->net_overlay);
#undef SETTINGS_FITS
    znx_ui_scrollbar(LIST_Y, 6, m->settings_row >= 4 ? 4 : 0, 10);

    switch (m->settings_row) {
    case 0:
        znx_ui_panel(LIST_Y, NULL, 0, "Game sound on the TV or the console's speakers. Off runs the game silent. Sound "
                     "does not change the game itself, so it never matters for online play.", path);
        znx_ui_footer(hints_toggle, 2, NULL);
        break;
    case 1: {
        float scale_y = 0;
        znx_ui_panel_ex(LIST_Y, NULL, 0,
                        "What the game reads when a stick is pushed all the way. RMG-K's default is 84; lower makes "
                        "full tilt harder to reach, higher makes it easier.", 96, &scale_y, path);
        draw_stick_scale(PANEL_X + 8, scale_y, PANEL_W - 16, s->stick_range);
        znx_ui_footer(hints_stick, 3, NULL);
        break;
    }
    case 2:
        znx_ui_panel(LIST_Y, NULL, 0, "What the Switch L button does: the N64's L (taunt) or its Z (shield). On a "
                     "GameCube-style controller whose left shoulder is the Switch L, Shield makes it shield like the "
                     "right one. Smash Remix can put taunt on the d-pad (Dpad ctrl on the character screen).",
                     "Only your own controllers change; it never matters for online play.");
        znx_ui_footer(hints_toggle, 2, NULL);
        break;
    case 3:
        znx_ui_panel(LIST_Y, NULL, 0, "C buttons: the C stick presses the N64's C buttons (jump). Attacks: a flick of "
                     "the C stick is a smash attack on the ground and an aerial in the air, in that direction, as "
                     "Melee's C stick. GameCube means controllers on the GameCube adapter.",
                     "Only your own controllers change; it never matters for online play.");
        znx_ui_footer(hints_toggle, 2, NULL);
        break;
    case 4:
        znx_ui_panel(LIST_Y, NULL, 0, "The name the other player sees in RMG-K's Peer to Peer window, on the public "
                     "list when you host, and in the lobby.",
                     "Up to 31 letters. The lobby keeps only letters, numbers, _ - and . and the first 20 of them.");
        znx_ui_footer(hints_name, 2, NULL);
        break;
    case 5:
        znx_ui_panel(LIST_Y, NULL, 0, "Frames between pressing a button and the game seeing it, when you host. Auto "
                     "picks it from the ping as RMG-K does: 1 up to 50 ms, 2 up to 100, 3 up to 150, 4 up to 220, "
                     "then 5. Lower feels sharper but rolls back more.",
                     "When you join, the host's delay is used.");
        znx_ui_footer(hints_delay, 3, NULL);
        break;
    case 6:
        znx_ui_panel(LIST_Y, NULL, 0, "When you host, the game appears on RMG-K's list of waiting games with your name "
                     "and code, and anyone with the same ROM can join. Off: only players you give the code to.", NULL);
        znx_ui_footer(hints_toggle, 2, NULL);
        break;
    default:
        znx_ui_panel(LIST_Y, NULL, 0, "Ping, delay and rollbacks in a small label at the top left during an online "
                     "match. It is drawn over the picture only and never changes the game.", NULL);
        znx_ui_footer(hints_toggle, 2, NULL);
        break;
    }
}

static void draw_build(struct znx_menu* m)
{
    const struct znx_game_info* info = znx_game_info(m->game);
    const struct znx_job* job = &m->job;
    const struct hint hints[] = { { "B", "Cancel" } };
    static const char* const names[ZNX_STEP_COUNT] = { "Download the patch", "Build the ROM", "Check the result" };
    const int first = job->downloads ? 0 : 1;
    const uint64_t done = job->done, total = job->total;
    char title[160], out[600], percent[16], detail[160], a[32], b[32];

    snprintf(title, sizeof(title), "Building %s", info->title);
    znx_ui_title(title);
    if (total > 0) snprintf(percent, sizeof(percent), "%d%%", (int)(done * 100 / total));
    else percent[0] = '\0';
    if (job->step == ZNX_STEP_DOWNLOAD) {
        znx_format_size(done, a, sizeof(a));
        znx_format_size(total, b, sizeof(b));
        snprintf(detail, sizeof(detail), "%s of %s from github.com", a, b);
    } else if (job->step == ZNX_STEP_BUILD) {
        snprintf(detail, sizeof(detail), "Applying the patch to your ROM");
    } else {
        snprintf(detail, sizeof(detail), "Comparing the MD5 with RMG-K's");
    }
    znx_ui_steps(LIST_Y, names + first, ZNX_STEP_COUNT - first, job->step - first, percent, detail,
                 total > 0 ? (float)((double)done / (double)total) : -1.0f);
    snprintf(out, sizeof(out), "roms/%s", info->out_name);
    {
        const struct field fields[] = {
            { "From", info->source, 0 },
            { "Patch", info->patch_name, 0 },
            { "To", out, 0 },
            { "MD5", info->md5, 1 },
        };
        znx_ui_panel(LIST_Y, fields, 4, NULL,
                   "The finished ROM is checked against this MD5, the one RMG-K players have. A ROM that differs is deleted.");
    }
    znx_ui_footer(hints, 1, m->base);
}

/* A message: what happened as the heading, what to do under it, then the details as a plain list (one
 * entry per line of message_detail; "name<TAB>details" puts the details small under the name), and OK
 * where there is room for it. */
static void draw_message(struct znx_menu* m)
{
    const struct hint hints[] = { { "A", "OK" } };
    const float width = 800, wide = RIGHT_EDGE - MARGIN;
    float y = LIST_Y + 36;
    int lines;
    znx_ui_title("Z-Cancel NX");
    lines = znx_draw_wrapped(ZNX_FONT_TITLE, MARGIN, y, width, m->message_title, C_TEXT1);
    y += (lines - 1) * znx_line_height(ZNX_FONT_TITLE) + 44;
    lines = znx_draw_wrapped(ZNX_FONT_BODY, MARGIN, y, width, m->message_text, C_TEXT1);
    y += (lines - 1) * znx_line_height(ZNX_FONT_BODY) + 28;
    if (m->message_detail[0]) {
        const char* p = m->message_detail;
        znx_draw_rect(MARGIN, y, wide, 1, 0, C_LINE);
        while (*p) {
            char raw[256], line[256];
            size_t n = strcspn(p, "\n");
            char* tab;
            if (n >= sizeof(raw)) n = sizeof(raw) - 1;
            memcpy(raw, p, n);
            raw[n] = '\0';
            tab = strchr(raw, '\t');
            if (y + (tab != NULL ? 52 : 32) > FOOTER_LINE - 8)
                break;   /* no room left: the rest is in latest.log */
            if (tab == NULL) {
                /* A plain line (a log line, a path): small. */
                znx_text_fit(ZNX_FONT_SMALL, raw, wide - 2 * ROW_PAD, line, sizeof(line));
                znx_draw_text(ZNX_FONT_SMALL, MARGIN + ROW_PAD, y + 23, line, C_TEXT2, 0);
                y += 32;
            } else {
                /* a file and why it is not used: the name, and under it the details small */
                *tab = '\0';
                znx_text_fit(ZNX_FONT_BODY, raw, wide - 2 * ROW_PAD, line, sizeof(line));
                znx_draw_text(ZNX_FONT_BODY, MARGIN + ROW_PAD, y + 23, line, C_TEXT1, 0);
                znx_text_fit(ZNX_FONT_SMALL, tab + 1, wide - 2 * ROW_PAD, line, sizeof(line));
                znx_draw_text(ZNX_FONT_SMALL, MARGIN + ROW_PAD, y + 44, line, C_TEXT2, 0);
                y += 52;
                znx_draw_rect(MARGIN, y, wide, 1, 0, C_LINE);
            }
            p += n;
            if (*p == '\n') p++;
        }
        if (strchr(m->message_detail, '\t') == NULL) {
            y += 8;
            znx_draw_rect(MARGIN, y, wide, 1, 0, C_LINE);
        }
    }
    /* OK, the one thing to press, with the selection outline as a system dialog has it */
    y += 32;
    if (y + ROW_H <= FOOTER_LINE - 12) {
        znx_ui_row_frame(y, 1);
        znx_draw_text(ZNX_FONT_ROW, LIST_X + LIST_W / 2, mid_base(ZNX_FONT_ROW, y + ROW_H / 2), "OK", C_ACCENT, ZNX_TEXT_CENTER);
    }
    znx_ui_footer(hints, 1, NULL);
}

static void draw_scan(struct znx_menu* m)
{
    znx_ui_title("Z-Cancel NX");
    znx_draw_text(ZNX_FONT_ROW, MARGIN + ROW_PAD, LIST_Y + 44, "Looking for your ROM", C_TEXT1, 0);
    znx_draw_text(ZNX_FONT_SMALL, MARGIN + ROW_PAD, LIST_Y + 76, m->base, C_TEXT2, 0);
    znx_ui_footer(NULL, 0, NULL);
}

/* debug_glyphs=1: the console's button glyphs with their code points, to check from one screenshot
 * that the ones the footer uses are the right ones. */
static void draw_glyphs(struct znx_menu* m)
{
    const struct hint hints[] = { { "A", "OK" }, { "B", "Back" }, { "Y", "Y" }, { "X", "X" }, { "+-", "D-pad" } };
    static const char* const used_keys[] = { "A", "B", "X", "Y", "LR" };
    const float cell_w = 70, cell_h = 64, top = LIST_Y + 48;
    unsigned used[16];
    int used_count = 0, present = 0;
    char line[200];
    (void)m;
    for (int k = 0; k < (int)(sizeof(used_keys) / sizeof(used_keys[0])); ++k) {
        unsigned codes[2];
        const int n = znx_button_codes(used_keys[k], codes);
        for (int i = 0; i < n; ++i) used[used_count++] = codes[i];
    }
    for (unsigned code = 0xE0A0; code <= 0xE0FF; ++code)
        present += znx_draw_has_symbol(code);
    znx_ui_title("Button glyphs");
    if (present > 0)
        snprintf(line, sizeof(line), "The console's symbol font: %d of 96 glyphs from U+E0A0. The footer uses the ones "
                 "whose code is in the accent colour.", present);
    else
        snprintf(line, sizeof(line), "The console's symbol font is not loaded: the footer shows stand-ins drawn by "
                 "this program. The codes in the accent colour are the ones it would use.");
    znx_draw_wrapped(ZNX_FONT_SMALL, MARGIN, LIST_Y + 4, RIGHT_EDGE - MARGIN, line, C_TEXT1);
    for (int i = 0; i < 96; ++i) {
        const unsigned code = 0xE0A0 + (unsigned)i;
        const float x = MARGIN + (i % 16) * cell_w, y = top + (i / 16) * cell_h;
        char glyph[4] = { (char)(0xE0 | (code >> 12)), (char)(0x80 | ((code >> 6) & 0x3F)), (char)(0x80 | (code & 0x3F)), 0 };
        char label[8];
        int is_used = 0;
        for (int k = 0; k < used_count; ++k)
            if (used[k] == code) is_used = 1;
        snprintf(label, sizeof(label), "%04X", code);
        if (znx_draw_has_symbol(code))
            znx_draw_text(ZNX_FONT_ROW, x + cell_w / 2, y + 28, glyph, C_TEXT1, ZNX_TEXT_CENTER);
        else
            znx_draw_rect(x + cell_w / 2 - 2, y + 18, 4, 4, 2, C_TEXT3);
        znx_draw_text(ZNX_FONT_SMALL, x + cell_w / 2, y + 52, label, is_used ? C_ACCENT : present ? C_TEXT2 : C_TEXT3,
                      ZNX_TEXT_CENTER);
    }
    znx_draw_wrapped(ZNX_FONT_SMALL, MARGIN, top + 6 * cell_h + 24, RIGHT_EDGE - MARGIN,
                     "Expected: A E0E0, B E0E1, X E0E2, Y E0E3, L E0E4, R E0E5, ZL E0E6, ZR E0E7, D-pad E0EA, D-pad left "
                     "E0ED and right E0EE, Plus E0EF, Minus E0F0, left stick E0C1. The footer below shows A, B, X, Y and "
                     "the D-pad's left and right as hints.", C_TEXT2);
    znx_ui_footer(hints, 5, znx_theme_is_dark() ? "Theme: Basic Black" : "Theme: Basic White");
}

void znx_menu_draw(struct znx_menu* m, unsigned framebuffer, int width, int height)
{
    znx_draw_begin(framebuffer, width, height, C_FIELD);
    l_focus.on = 0;
    draw_header(m);
    switch (m->screen) {
    case SCREEN_SCAN: draw_scan(m); break;
    case SCREEN_HOME: draw_home(m); break;
    case SCREEN_GAME: draw_game(m); break;
    case SCREEN_SETTINGS: draw_settings(m); break;
    case SCREEN_BUILD: draw_build(m); break;
    case SCREEN_MESSAGE: draw_message(m); break;
    case SCREEN_ONLINE: znx_online_draw(); break;
    case SCREEN_GLYPHS: draw_glyphs(m); break;
    }
    flush_focus();
    znx_draw_end();
}
