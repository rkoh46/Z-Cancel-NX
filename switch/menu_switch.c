// SPDX-License-Identifier: GPL-3.0-or-later
/* See menu_switch.h. The menu's own OpenGL 3.3 core context on the shared window surface, the
 * console's system font and button glyphs (sysfont_switch.c; the font in the NRO's romfs when the
 * console does not give them), every controller (handheld, Joy-Con, Pro, GameCube adapter) read as
 * one, and the buffer swap pacing the loop. */

#include "menu_switch.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include <EGL/egl.h>

#include "draw.h"
#include "front_switch.h"
#include "stall.h"
#include "screen_switch.h"
#include "sysfont_switch.h"
#include "znx_switch.h"

#define FONT_PATH "romfs:/fonts/BarlowSemiCondensed-Medium.ttf"

static void* l_font;
static size_t l_font_size;
static bool l_failed;

bool znx_menu_switch_failed(void) { return l_failed; }

/* ---- the watchdog: a menu or lobby screen that stops ----
 * The game has its own (front_switch.c); this one covers the start screen and the online screens, which
 * share this loop. The loop beats at every step of a frame; a thread on another core polls twice a second,
 * logs where the loop is stuck after 5 s, and leaves the program if a buffer swap has not returned in 15 s
 * (a stopped GPU, which otherwise leaves the console frozen and black until it is reset). */
static volatile unsigned l_beat;
static volatile int l_phase = ZNX_STALL_INPUT;
static volatile bool l_watching;
static Thread l_watch_thread;
static bool l_watch_started;

static void beat(enum znx_stall_phase phase)
{
    l_phase = phase;
    l_beat = l_beat + 1;
}

static void watch_thread(void* arg)
{
    struct znx_stall stall = { 0, 0 };
    (void)arg;
    for (;;) {
        svcSleepThread(500000000ll);
        if (!l_watching) {
            stall.polls = 0;
            continue;
        }
        enum znx_stall_phase phase = (enum znx_stall_phase)l_phase;
        switch (znx_stall_poll(&stall, l_beat, phase)) {
        case ZNX_STALL_REPORT:
            znx_log("menu: WATCHDOG: the screen has not moved for %d s, stuck in %s", stall.polls / 2,
                    znx_stall_phase_name(phase));
            break;
        case ZNX_STALL_GIVE_UP:
            znx_log("menu: WATCHDOG: %s for %d s; closing so the console can recover",
                    znx_stall_phase_name(phase), stall.polls / 2);
            znx_front_hard_exit();
        default:
            break;
        }
    }
}

static void watch_begin(void)
{
    if (!l_watch_started) {
        /* core 1, just above the main thread's priority so a busy loop cannot starve it */
        Result rc = threadCreate(&l_watch_thread, watch_thread, NULL, NULL, 0x4000, 0x2B, 1);
        if (R_SUCCEEDED(rc) && R_FAILED(rc = threadStart(&l_watch_thread)))
            threadClose(&l_watch_thread);
        if (R_FAILED(rc)) {
            znx_log("menu: the watchdog does not start: 0x%08x", rc);
            return;
        }
        l_watch_started = true;
    }
    beat(ZNX_STALL_INPUT);
    l_watching = true;
}

static void* getproc(const char* name)
{
    return (void*)eglGetProcAddress(name);
}

/* The bundled font, the fallback. */
static int load_font(void)
{
    FILE* f;
    long size;
    if (l_font != NULL)
        return 1;
    f = fopen(FONT_PATH, "rb");
    if (f == NULL) {
        znx_log("menu: cannot open " FONT_PATH);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    l_font = size > 0 ? malloc((size_t)size) : NULL;
    if (l_font == NULL || fread(l_font, 1, (size_t)size, f) != (size_t)size) {
        znx_log("menu: cannot read " FONT_PATH);
        free(l_font);
        l_font = NULL;
        fclose(f);
        return 0;
    }
    fclose(f);
    l_font_size = (size_t)size;
    return 1;
}

/* The renderer with the console's system font and button glyphs; with the bundled font if the console
 * does not give its own or FreeType cannot read it. */
static int start_draw(void)
{
    const void* standard;
    const void* extended;
    size_t standard_size, extended_size;
    char error[256];
    static int said;
    if (znx_sysfont_get(&standard, &standard_size, &extended, &extended_size)) {
        znx_draw_set_symbol_font(extended, extended_size);
        if (znx_draw_init(getproc, standard, standard_size, error, sizeof(error))) {
            if (!said)
                znx_log("menu: drawing with the system font, button glyphs %s", extended != NULL ? "from the system" : "drawn as stand-ins");
            said = 1;
            return 1;
        }
        znx_log("menu: the system font cannot be used (%s), using the bundled font", error);
        znx_draw_shutdown();
        znx_sysfont_reject();
    }
    if (!load_font())
        return 0;
    znx_draw_set_symbol_font(extended, extended_size);
    if (!znx_draw_init(getproc, l_font, l_font_size, error, sizeof(error))) {
        znx_log("%s", error);
        znx_draw_shutdown();
        return 0;
    }
    if (!said)
        znx_log("menu: drawing with the bundled font, button glyphs %s", extended != NULL ? "from the system" : "drawn as stand-ins");
    said = 1;
    return 1;
}

static uint32_t buttons(u64 held)
{
    uint32_t b = 0;
    if (held & HidNpadButton_AnyUp) b |= ZNX_BTN_UP;
    if (held & HidNpadButton_AnyDown) b |= ZNX_BTN_DOWN;
    if (held & HidNpadButton_AnyLeft) b |= ZNX_BTN_LEFT;
    if (held & HidNpadButton_AnyRight) b |= ZNX_BTN_RIGHT;
    if (held & HidNpadButton_A) b |= ZNX_BTN_A;
    if (held & HidNpadButton_B) b |= ZNX_BTN_B;
    if (held & HidNpadButton_X) b |= ZNX_BTN_X;
    if (held & HidNpadButton_Y) b |= ZNX_BTN_Y;
    if (held & HidNpadButton_Plus) b |= ZNX_BTN_PLUS;
    if (held & HidNpadButton_Minus) b |= ZNX_BTN_MINUS;
    if (held & HidNpadButton_L) b |= ZNX_BTN_L;
    if (held & HidNpadButton_R) b |= ZNX_BTN_R;
    if (held & HidNpadButton_ZL) b |= ZNX_BTN_ZL;
    if (held & HidNpadButton_ZR) b |= ZNX_BTN_ZR;
    return b;
}

int znx_switch_keyboard(int kind, const char* header, const char* guide, const char* initial, char* out, size_t out_size)
{
    /* The system keyboard applet (libnx swkbd). It takes over the screen until the player is done; the
     * menu's context stays current meanwhile and the next frame draws over whatever the applet left. */
    SwkbdConfig keyboard;
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc)) {
        znx_log("menu: the keyboard does not open: 0x%08x", rc);
        return 0;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    if (kind == 2) {
        /* an address: digits, with "." and ":" on the two extra keys */
        swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
        swkbdConfigSetLeftOptionalSymbolKey(&keyboard, ".");
        swkbdConfigSetRightOptionalSymbolKey(&keyboard, ":");
    } else {
        swkbdConfigSetType(&keyboard, SwkbdType_Normal);
    }
    swkbdConfigSetHeaderText(&keyboard, header);
    swkbdConfigSetGuideText(&keyboard, guide);
    swkbdConfigSetInitialText(&keyboard, initial != NULL ? initial : "");
    /* a name, a connect code, an address, a chat line, a room name (RMG-K's limit: 48), a room password */
    swkbdConfigSetStringLenMax(&keyboard, kind == 0 ? 31 : kind == 1 ? 12 : kind == 3 ? 120 : kind == 4 ? 48 : kind == 5 ? 32 : 21);
    swkbdConfigSetBlurBackground(&keyboard, true);
    out[0] = '\0';
    beat(ZNX_STALL_KEYBOARD);
    rc = swkbdShow(&keyboard, out, out_size);
    beat(ZNX_STALL_UPDATE);
    swkbdClose(&keyboard);
    if (R_FAILED(rc)) {
        out[0] = '\0';
        return 0;   /* cancelled (0x5d59) or failed */
    }
    znx_log("menu: keyboard \"%s\": %u characters", header, (unsigned)strlen(out));
    return out[0] != '\0';
}

enum znx_menu_action znx_menu_switch_run(struct znx_menu* menu)
{
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    PadState pad;
    enum znx_menu_action action = ZNX_MENU_NONE;

    l_failed = true;   /* until the first frame is shown */
    if (!znx_screen_open())
        return ZNX_MENU_QUIT;
    display = znx_screen_display();
    surface = znx_screen_surface();
    context = znx_screen_context(3, 3, false);
    if (context == EGL_NO_CONTEXT) {
        znx_log("menu: no OpenGL 3.3 context: %s", znx_screen_egl_error());
        return ZNX_MENU_QUIT;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        /* EGL_BAD_ACCESS here would mean the surface is still current on the emulation thread. */
        znx_log("menu: cannot make the OpenGL context current: %s", znx_screen_egl_error());
        eglDestroyContext(display, context);
        return ZNX_MENU_QUIT;
    }
    eglSwapInterval(display, 1);
    if (!start_draw()) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        return ZNX_MENU_QUIT;
    }

    /* Every controller as one: up to eight, the console itself in handheld mode, GameCube
     * controllers on the adapter too. */
    padConfigureInput(8, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadGc);
    padInitializeAny(&pad);
    znx_menu_resume(menu);
    znx_log("menu: shown");
    l_failed = false;
    watch_begin();

    while (action == ZNX_MENU_NONE) {
        beat(ZNX_STALL_APPLET);
        if (!appletMainLoop()) {
            znx_log("menu: the system asks the program to close");
            action = ZNX_MENU_QUIT;
            break;
        }
        beat(ZNX_STALL_INPUT);
        padUpdate(&pad);
        beat(ZNX_STALL_UPDATE);
        action = znx_menu_update(menu, buttons(padGetButtons(&pad)));
        beat(ZNX_STALL_DRAW);
        znx_menu_draw(menu, 0, 1280, 720);
        beat(ZNX_STALL_SWAP);
        eglSwapBuffers(display, surface);   /* swap interval 1: waits for the display */
    }
    l_watching = false;

    /* A black frame, so the game's first frames do not show the menu behind its 4:3 picture. */
    {
        void (*clear_color)(float, float, float, float) = (void (*)(float, float, float, float))getproc("glClearColor");
        void (*clear)(unsigned) = (void (*)(unsigned))getproc("glClear");
        if (clear_color != NULL && clear != NULL) {
            for (int i = 0; i < 3; ++i) {
                clear_color(0.0f, 0.0f, 0.0f, 1.0f);
                clear(0x4000);
                eglSwapBuffers(display, surface);
            }
        }
    }
    znx_draw_shutdown();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    znx_log("menu: %s", action == ZNX_MENU_PLAY ? "play" : action == ZNX_MENU_PLAY_ONLINE ? "play online" : "quit");
    return action;
}
