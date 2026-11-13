// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-game --menu-shot: the start screen (menu/) rendered on a Mac without a window, for looking
 * at it. The same menu code the Switch runs draws into an offscreen OpenGL 4.1 context; a scripted
 * button sequence drives it; PNGs come out. Nothing is played and nothing appears on screen.
 *
 *   znx-game --menu-shot OUT.png [--menu-keys "down,down,a,shot=x.png,job"] [--menu-base DIR]
 *            [--menu-build GAME,STEP,DONE,TOTAL[,download]] [--menu-notice "title|text|detail"]
 *
 * --menu-base is the folder that stands in for sdmc:/switch/z-cancel-nx (default: menu-sd next to
 * the program). Keys: up down left right a b x y plus minus l r zl zr; "hold-minus" holds Minus for
 * a second; "wait" lets 30 frames pass; "job" waits for a build to finish (it really runs: the
 * patches in DIR/patches are applied, a download goes to ZNX_PATCH_URL_OVERRIDE if that is set);
 * "shot=FILE" saves the screen at that point. OUT.png is the screen after the last key. */

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

#ifndef ZNX_VERSION
#define ZNX_VERSION "dev"
#endif

#define W 1280
#define H 720

static GLuint l_fbo;

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
    return a == ZNX_MENU_PLAY ? "play" : a == ZNX_MENU_QUIT ? "quit" : NULL;
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
        else {
            fprintf(stderr, "znx-game --menu-shot OUT.png [--menu-keys K,K,...] [--menu-base DIR] "
                            "[--menu-build GAME,STEP,DONE,TOTAL[,download]] [--menu-notice \"title|text|detail\"]\n");
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
    snprintf(font_path, sizeof(font_path), "%s/romfs/fonts/BarlowSemiCondensed-Medium.ttf", dir);
    font = read_all(font_path, &font_size);
    if (font == NULL) {
        fprintf(stderr, "menu: no font at %s\n", font_path);
        return 1;
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

    {
        char copy[2048];
        snprintf(copy, sizeof(copy), "%s", keys);
        for (char* key = strtok(copy, ","); key != NULL; key = strtok(NULL, ",")) {
            enum znx_menu_action action = ZNX_MENU_NONE;
            if (!strncmp(key, "shot=", 5)) {
                save_png(key + 5);
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
                printf("menu: %s%s%s\n", action_name(action), action == ZNX_MENU_PLAY ? " " : "",
                       action == ZNX_MENU_PLAY ? znx_menu_rom_path(menu) : "");
                znx_menu_resume(menu);
            }
        }
    }
    frame(menu, 0);
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
