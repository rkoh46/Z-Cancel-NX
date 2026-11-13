// SPDX-License-Identifier: GPL-3.0-or-later
/* See menu_switch.h. The menu's own OpenGL 3.3 core context on the shared window surface, the
 * font from the NRO's romfs, every controller (handheld, Joy-Con, Pro, GameCube adapter) read as
 * one, and the buffer swap pacing the loop. */

#include "menu_switch.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include <EGL/egl.h>

#include "draw.h"
#include "screen_switch.h"
#include "znx_switch.h"

#define FONT_PATH "romfs:/fonts/BarlowSemiCondensed-Medium.ttf"

static void* l_font;
static size_t l_font_size;
static bool l_failed;

bool znx_menu_switch_failed(void) { return l_failed; }

static void* getproc(const char* name)
{
    return (void*)eglGetProcAddress(name);
}

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
    swkbdConfigSetStringLenMax(&keyboard, kind == 0 ? 31 : kind == 1 ? 12 : 21);
    swkbdConfigSetBlurBackground(&keyboard, true);
    out[0] = '\0';
    rc = swkbdShow(&keyboard, out, out_size);
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
    char error[256];
    enum znx_menu_action action = ZNX_MENU_NONE;

    l_failed = true;   /* until the first frame is shown */
    if (!load_font() || !znx_screen_open())
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
    if (!znx_draw_init(getproc, l_font, l_font_size, error, sizeof(error))) {
        znx_log("%s", error);
        znx_draw_shutdown();
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

    while (action == ZNX_MENU_NONE) {
        if (!appletMainLoop()) {
            znx_log("menu: the system asks the program to close");
            action = ZNX_MENU_QUIT;
            break;
        }
        padUpdate(&pad);
        action = znx_menu_update(menu, buttons(padGetButtons(&pad)));
        znx_menu_draw(menu, 0, 1280, 720);
        eglSwapBuffers(display, surface);   /* swap interval 1: waits for the display */
    }

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
