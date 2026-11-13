// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-game --menu-shot: the start screen (menu/) rendered on a Mac without a window, for looking
 * at it. The same menu code the Switch runs draws into an offscreen OpenGL 4.1 context; a scripted
 * button sequence drives it; PNGs come out. Nothing is played and nothing appears on screen.
 *
 *   znx-game --menu-shot OUT.png [--menu-keys "down,down,a,shot=x.png,job"] [--menu-base DIR]
 *            [--menu-build GAME,STEP,DONE,TOTAL[,download]] [--menu-notice "title|text|detail"]
 *            [--menu-text "first|second"] [--menu-online GAME,SCREEN]
 *            [--menu-theme light|dark] [--menu-font FILE|barlow] [--menu-glyphs] [--menu-overlay "Ping=41 ms|Delay=2"]
 *
 * --menu-base is the folder that stands in for sdmc:/switch/z-cancel-nx (default: menu-sd next to
 * the program). Keys: up down left right a b x y plus minus l r zl zr; "hold-minus" holds Minus for
 * a second; "wait" lets 30 frames pass; "job" waits for a build to finish (it really runs: the
 * patches in DIR/patches are applied, a download goes to ZNX_PATCH_URL_OVERRIDE if that is set);
 * "shot=FILE" saves the screen at that point; "sleep=MS" keeps drawing frames for MS milliseconds of real
 * time (the online screens wait for the network: the lobby runs for real, against whatever local test
 * servers the base folder's z-cancel-nx.ini names). --menu-text answers the system keyboard, one entry
 * per time it opens. --menu-online shows an online screen with made-up content (online.h
 * znx_online_debug: lobby, lobby-mismatch, host, host-ip, joining, list, list-empty). OUT.png is the
 * screen after the last key. --menu-theme stands in for the console's theme setting (Basic White or Basic
 * Black). The console draws with its own system font and button glyphs, which a Mac does not have: the
 * preview uses a system sans of this Mac that is close in width (--menu-font: another file, or "barlow"
 * for the bundled fallback font) and the stand-in button discs, and holds the selection outline's pulse
 * at its middle. --menu-glyphs opens the button glyph check (debug_glyphs=1); --menu-overlay shows the
 * online match's label over a stand-in for the game's picture instead of a menu screen. */

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "draw.h"
#include "menu.h"
#include "menu_mac.h"
#include "menu_ui.h"

#ifndef ZNX_VERSION
#define ZNX_VERSION "dev"
#endif

#define W 1280
#define H 720

static GLuint l_fbo;
static char l_texts[8][128];
static int l_text_count, l_text_next;

void znx_log_line(const char* line)
{
    fprintf(stderr, "%s\n", line);
}

/* The system keyboard, answered from --menu-text. */
static int keyboard(int kind, const char* header, const char* guide, const char* initial, char* out, size_t out_size)
{
    (void)guide;
    if (l_text_next >= l_text_count) {
        printf("menu: keyboard \"%s\" (kind %d, initial \"%s\") cancelled\n", header, kind, initial ? initial : "");
        return 0;
    }
    snprintf(out, out_size, "%s", l_texts[l_text_next++]);
    printf("menu: keyboard \"%s\" answered \"%s\"\n", header, out);
    return 1;
}

static void* getproc(const char* name)
{
    return dlsym(RTLD_DEFAULT, name);
}

static void chunk(FILE* f, const char* type, const unsigned char* data, uint32_t size)
{
    unsigned char head[8] = { size >> 24, size >> 16, size >> 8, size, type[0], type[1], type[2], type[3] };
    uint32_t crc = (uint32_t)crc32(crc32(0, head + 4, 4), data, size);
    unsigned char tail[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
    fwrite(head, 1, 8, f);
    if (size != 0)
        fwrite(data, 1, size, f);
    fwrite(tail, 1, 4, f);
}

static int save_png(const char* path)
{
    const size_t row = (size_t)W * 3;
    unsigned char* pixels = malloc(row * H);
    unsigned char* raw = malloc((row + 1) * H);
    uLongf packed_size = compressBound((row + 1) * H);
    unsigned char* packed = malloc(packed_size);
    unsigned char header[13] = { W >> 24, W >> 16, W >> 8, W & 255, H >> 24, H >> 16, H >> 8, H & 255, 8, 2, 0, 0, 0 };
    FILE* f;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, l_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    for (int y = 0; y < H; ++y) {
        raw[(row + 1) * y] = 0;
        memcpy(raw + (row + 1) * y + 1, pixels + row * (H - 1 - y), row);
    }
    compress2(packed, &packed_size, raw, (row + 1) * H, 6);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        chunk(f, "IHDR", header, 13);
        chunk(f, "IDAT", packed, (uint32_t)packed_size);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
        printf("menu: wrote %s\n", path);
    } else {
        fprintf(stderr, "menu: cannot write %s\n", path);
    }
    free(pixels); free(raw); free(packed);
    return f != NULL;
}

static void* read_all(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    void* data;
    long n;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)n);
    if (data != NULL && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static uint32_t key_bit(const char* key)
{
    static const struct { const char* name; uint32_t bit; } keys[] = {
        { "up", ZNX_BTN_UP }, { "down", ZNX_BTN_DOWN }, { "left", ZNX_BTN_LEFT }, { "right", ZNX_BTN_RIGHT },
        { "a", ZNX_BTN_A }, { "b", ZNX_BTN_B }, { "x", ZNX_BTN_X }, { "y", ZNX_BTN_Y },
        { "plus", ZNX_BTN_PLUS }, { "minus", ZNX_BTN_MINUS }, { "l", ZNX_BTN_L }, { "r", ZNX_BTN_R },
        { "zl", ZNX_BTN_ZL }, { "zr", ZNX_BTN_ZR },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (!strcasecmp(key, keys[i].name))
            return keys[i].bit;
    return 0;
}

/* One frame: update with these buttons held, then draw. Returns the menu's answer. */
static enum znx_menu_action frame(struct znx_menu* menu, uint32_t held)
{
    enum znx_menu_action action = znx_menu_update(menu, held);
    znx_menu_draw(menu, l_fbo, W, H);
    glFinish();
    return action;
}

static const char* action_name(enum znx_menu_action a)
{
    return a == ZNX_MENU_PLAY ? "play" : a == ZNX_MENU_QUIT ? "quit" : a == ZNX_MENU_PLAY_ONLINE ? "play online" : NULL;
}

int znx_menu_mac_main(int argc, char** argv)
{
    char exe[4096], dir[4096], base[4300], font_path[4300], error[256];
    uint32_t exe_size = sizeof(exe);
    const char* out = NULL;
    const char* keys = "";
    const char* build = NULL;
    const char* notice = NULL;
    const char* base_arg = NULL;
    const char* online = NULL;
    const char* theme = NULL;
    const char* font_arg = NULL;
    const char* overlay = NULL;
    int glyphs = 0;
    CGLPixelFormatObj format = NULL;
    CGLContextObj context = NULL;
    GLint formats = 0;
    GLuint color;
    void* font;
    size_t font_size = 0;
    struct znx_menu* menu;
    struct znx_menu_config config;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--menu-shot") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--menu-keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--menu-base") && i + 1 < argc) base_arg = argv[++i];
        else if (!strcmp(argv[i], "--menu-build") && i + 1 < argc) build = argv[++i];
        else if (!strcmp(argv[i], "--menu-notice") && i + 1 < argc) notice = argv[++i];
        else if (!strcmp(argv[i], "--menu-online") && i + 1 < argc) online = argv[++i];
        else if (!strcmp(argv[i], "--menu-theme") && i + 1 < argc) theme = argv[++i];
        else if (!strcmp(argv[i], "--menu-font") && i + 1 < argc) font_arg = argv[++i];
        else if (!strcmp(argv[i], "--menu-overlay") && i + 1 < argc) overlay = argv[++i];
        else if (!strcmp(argv[i], "--menu-glyphs")) glyphs = 1;
        else if (!strcmp(argv[i], "--menu-text") && i + 1 < argc) {
            char copy[1024];
            snprintf(copy, sizeof(copy), "%s", argv[++i]);
            for (char* t = strtok(copy, "|"); t != NULL && l_text_count < 8; t = strtok(NULL, "|"))
                snprintf(l_texts[l_text_count++], sizeof(l_texts[0]), "%s", t);
        }
        else {
            fprintf(stderr, "znx-game --menu-shot OUT.png [--menu-keys K,K,...] [--menu-base DIR] "
                            "[--menu-build GAME,STEP,DONE,TOTAL[,download]] [--menu-notice \"title|text|detail\"] "
                            "[--menu-theme light|dark] [--menu-font FILE|barlow] [--menu-glyphs] [--menu-overlay \"A=1|B=2\"]\n");
            return 2;
        }
    }
    if (out == NULL)
        return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (_NSGetExecutablePath(exe, &exe_size) != 0 || realpath(exe, dir) == NULL)
        return 1;
    strcpy(dir, dirname(dir));
    snprintf(base, sizeof(base), "%s", base_arg != NULL ? base_arg : "");
    if (base_arg == NULL) {
        snprintf(base, sizeof(base), "%s/menu-sd", dir);
        mkdir(base, 0755);
    }
    {
        /* The console's system font is not on a Mac. A system sans of this Mac that is close to it in
         * width and weight stands in (read from where the system keeps it, never copied); the bundled
         * font is what the console falls back to. */
        static const char* const stand_ins[] = { "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                                 "/System/Library/Fonts/Helvetica.ttc" };
        font = NULL;
        if (font_arg != NULL && strcmp(font_arg, "barlow") != 0) {
            snprintf(font_path, sizeof(font_path), "%s", font_arg);
            font = read_all(font_path, &font_size);
        }
        for (size_t i = 0; font == NULL && font_arg == NULL && i < sizeof(stand_ins) / sizeof(stand_ins[0]); ++i) {
            snprintf(font_path, sizeof(font_path), "%s", stand_ins[i]);
            font = read_all(font_path, &font_size);
        }
        if (font == NULL) {
            snprintf(font_path, sizeof(font_path), "%s/romfs/fonts/BarlowSemiCondensed-Medium.ttf", dir);
            font = read_all(font_path, &font_size);
        }
        if (font == NULL) {
            fprintf(stderr, "menu: no font at %s\n", font_path);
            return 1;
        }
        printf("menu: preview font %s (the console uses its system font and button glyphs)\n", font_path);
    }

    {
        const CGLPixelFormatAttribute attributes[] = {
            kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
            kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0
        };
        if (CGLChoosePixelFormat(attributes, &format, &formats) != kCGLNoError || format == NULL ||
            CGLCreateContext(format, NULL, &context) != kCGLNoError) {
            fprintf(stderr, "menu: no OpenGL 4.1 context\n");
            return 1;
        }
        CGLDestroyPixelFormat(format);
        CGLSetCurrentContext(context);
    }
    glGenFramebuffers(1, &l_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, l_fbo);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "menu: framebuffer incomplete\n");
        return 1;
    }

    if (!znx_draw_init(getproc, font, font_size, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    config.base_dir = base;
    config.version = ZNX_VERSION;
    config.keyboard = keyboard;
    config.console_theme = theme == NULL ? -1 : !strcmp(theme, "dark");
    config.theme_note = "the Mac preview; --menu-theme sets it";
    config.debug_glyphs = glyphs;
    znx_theme_freeze(0.5f);
    menu = znx_menu_create(&config);
    if (notice != NULL) {
        char copy[1200], *title, *text, *detail;
        snprintf(copy, sizeof(copy), "%s", notice);
        title = strtok(copy, "|");
        text = strtok(NULL, "|");
        detail = strtok(NULL, "|");
        frame(menu, 0);
        frame(menu, 0);   /* scanned */
        znx_menu_notice(menu, title ? title : "", text ? text : "", detail);
    }
    frame(menu, 0);    /* "looking for your ROM" */
    frame(menu, 0);    /* scanned */
    if (build != NULL) {
        int game = 1, step = 0, downloads = 0;
        unsigned long long done = 0, total = 1;
        char flag[32] = "";
        sscanf(build, "%d,%d,%llu,%llu,%31s", &game, &step, &done, &total, flag);
        downloads = !strcmp(flag, "download");
        znx_menu_debug_build(menu, game, downloads, step, done, total);
        frame(menu, 0);
    }
    if (online != NULL) {
        int game = 0;
        char which[64] = "";
        sscanf(online, "%d,%63s", &game, which);
        znx_menu_debug_online(menu, game, which);
        frame(menu, 0);
    }

    {
        char copy[2048];
        snprintf(copy, sizeof(copy), "%s", keys);
        for (char* key = strtok(copy, ","); key != NULL; key = strtok(NULL, ",")) {
            enum znx_menu_action action = ZNX_MENU_NONE;
            if (!strncmp(key, "shot=", 5)) {
                save_png(key + 5);
            } else if (!strncmp(key, "sleep=", 6)) {
                const int ms = atoi(key + 6);
                for (int t = 0; t < ms && action == ZNX_MENU_NONE; t += 16) {
                    action = frame(menu, 0);
                    usleep(16000);
                }
            } else if (!strcmp(key, "wait")) {
                for (int i = 0; i < 30; ++i) action = action ? action : frame(menu, 0);
            } else if (!strcmp(key, "job")) {
                /* Polled like the console's 60 Hz loop until the build has ended, 5 minutes at most. */
                for (int i = 0; i < 18000 && znx_menu_busy(menu); ++i) {
                    action = frame(menu, 0);
                    usleep(16000);
                }
                action = action ? action : frame(menu, 0);
            } else if (!strcmp(key, "hold-minus")) {
                for (int i = 0; i < 40 && action == ZNX_MENU_NONE; ++i) action = frame(menu, ZNX_BTN_MINUS);
                if (action == ZNX_MENU_NONE) action = frame(menu, 0);
            } else {
                uint32_t bit = key_bit(key);
                if (bit == 0) {
                    fprintf(stderr, "menu: unknown key %s\n", key);
                    return 2;
                }
                action = frame(menu, bit);
                if (action == ZNX_MENU_NONE) action = frame(menu, 0);
            }
            if (action_name(action) != NULL) {
                printf("menu: %s%s%s\n", action_name(action), action != ZNX_MENU_QUIT ? " " : "",
                       action != ZNX_MENU_QUIT ? znx_menu_rom_path(menu) : "");
                znx_menu_resume(menu);
            }
        }
    }
    frame(menu, 0);
    if (overlay != NULL) {
        /* the match overlay over a stand-in for the game: black margins, a flat 4:3 picture */
        char copy[256];
        const char* labels[4];
        const char* values[4];
        int count = 0;
        snprintf(copy, sizeof(copy), "%s", overlay);
        for (char* t = strtok(copy, "|"); t != NULL && count < 4; t = strtok(NULL, "|")) {
            char* eq = strchr(t, '=');
            if (eq == NULL) continue;
            *eq = '\0';
            labels[count] = t;
            values[count++] = eq + 1;
        }
        znx_draw_begin(l_fbo, W, H, 0x000000FFu);
        znx_draw_rect(160, 0, 960, 720, 0, 0x5B7FA6FFu);
        znx_draw_rect(160, 470, 960, 250, 0, 0x4F8A4BFFu);
        znx_ui_overlay(labels, values, count);
        znx_draw_end();
        glFinish();
    }
    save_png(out);

    znx_menu_destroy(menu);
    znx_draw_shutdown();
    glDeleteFramebuffers(1, &l_fbo);
    glDeleteTextures(1, &color);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(context);
    free(font);
    return 0;
}
