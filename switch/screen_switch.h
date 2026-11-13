// SPDX-License-Identifier: GPL-3.0-or-later
/* screen_switch.c: one EGL display and window surface for the whole program, shared by the start
 * screen (menu_switch.c, main thread) and the game (vidext_switch.c, emulation thread). */
#ifndef ZNX_SCREEN_SWITCH_H
#define ZNX_SCREEN_SWITCH_H

#include <stdbool.h>

#include <EGL/egl.h>

/* Opens it if it is not open: EGL on nwindowGetDefault(), 1280x720, desktop OpenGL, RGBA8 with
 * 24-bit depth and 8-bit stencil (what GLideN64 draws into). False, with a line in the log, if not. */
bool znx_screen_open(void);
bool znx_screen_is_open(void);
EGLDisplay znx_screen_display(void);
EGLConfig znx_screen_config(void);
EGLSurface znx_screen_surface(void);
/* A desktop OpenGL core context of that version for the shared config, or EGL_NO_CONTEXT. */
EGLContext znx_screen_context(int major, int minor, bool compatibility);
/* At the end of the program, with no context current anywhere. */
void znx_screen_close(void);
const char* znx_screen_egl_error(void);

#endif
