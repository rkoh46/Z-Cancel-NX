// SPDX-License-Identifier: GPL-3.0-or-later
/* See screen_switch.h.
 *
 * Why one surface for the whole program: the start screen and the game take turns on the same
 * NWindow. Destroying a window surface and creating another on it, once per game, is a path the
 * Switch's Mesa EGL platform is not known to be exercised on (devkitPro's examples create one and
 * keep it); keeping the display and the surface for the life of the program and only creating and
 * destroying OpenGL contexts avoids it. A context is made current on one thread at a time: the
 * menu releases the surface (eglMakeCurrent with no surface) before the emulation thread starts,
 * and the video extension releases it when GLideN64 closes its window (RomClosed -> VidExt_Quit),
 * before the menu takes it back. */

#include "screen_switch.h"

#include <stdio.h>

#include <switch.h>

#include <EGL/eglext.h>

#include "znx_switch.h"

static EGLDisplay l_display = EGL_NO_DISPLAY;
static EGLConfig l_config;
static EGLSurface l_surface = EGL_NO_SURFACE;

const char* znx_screen_egl_error(void)
{
    static char text[32];
    snprintf(text, sizeof(text), "EGL error 0x%x", (unsigned)eglGetError());
    return text;
}

bool znx_screen_is_open(void) { return l_surface != EGL_NO_SURFACE; }
EGLDisplay znx_screen_display(void) { return l_display; }
EGLConfig znx_screen_config(void) { return l_config; }
EGLSurface znx_screen_surface(void) { return l_surface; }

bool znx_screen_open(void)
{
    NWindow* window = nwindowGetDefault();
    EGLint count = 0;
    Result rc;
    const EGLint attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    if (l_surface != EGL_NO_SURFACE)
        return true;
    l_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (l_display == EGL_NO_DISPLAY || !eglInitialize(l_display, NULL, NULL)) {
        znx_log("screen: cannot open the display: %s", znx_screen_egl_error());
        l_display = EGL_NO_DISPLAY;
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        znx_log("screen: this EGL has no desktop OpenGL: %s", znx_screen_egl_error());
        znx_screen_close();
        return false;
    }
    /* The buffers the screen is fed from; the system scales them to the panel or the TV. */
    rc = nwindowSetDimensions(window, 1280, 720);
    if (R_FAILED(rc))
        znx_log("screen: the window refuses 1280x720 (0x%08x); its own size stays", rc);
    if (!eglChooseConfig(l_display, attributes, &l_config, 1, &count) || count < 1) {
        znx_log("screen: no matching framebuffer format: %s", znx_screen_egl_error());
        znx_screen_close();
        return false;
    }
    l_surface = eglCreateWindowSurface(l_display, l_config, window, NULL);
    if (l_surface == EGL_NO_SURFACE) {
        znx_log("screen: cannot create the window surface: %s", znx_screen_egl_error());
        znx_screen_close();
        return false;
    }
    znx_log("screen: 1280x720 window surface, shared by the start screen and the game");
    return true;
}

EGLContext znx_screen_context(int major, int minor, bool compatibility)
{
    const EGLint attributes[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        compatibility ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, major,
        EGL_CONTEXT_MINOR_VERSION_KHR, minor,
        EGL_NONE
    };
    if (l_display == EGL_NO_DISPLAY)
        return EGL_NO_CONTEXT;
    eglBindAPI(EGL_OPENGL_API);   /* per thread: the caller's thread may not have bound it */
    return eglCreateContext(l_display, l_config, EGL_NO_CONTEXT, attributes);
}

void znx_screen_close(void)
{
    if (l_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(l_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (l_surface != EGL_NO_SURFACE)
            eglDestroySurface(l_display, l_surface);
        eglTerminate(l_display);
    }
    l_surface = EGL_NO_SURFACE;
    l_display = EGL_NO_DISPLAY;
}
