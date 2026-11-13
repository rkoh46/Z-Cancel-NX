// SPDX-License-Identifier: GPL-3.0-or-later
/* The core's video extension (VidExt_*) on EGL and libnx.
 *
 * A mupen64plus video plugin does not create its own window: it asks the core
 * (VidExt_Init, VidExt_GL_SetAttribute, VidExt_SetVideoMode, VidExt_GL_SwapBuffers, ...),
 * and the core does it with SDL unless the front end has handed it its own functions with
 * CoreOverrideVidExt. These are those functions: the Switch's screen (nwindowGetDefault)
 * as an EGL window surface with a desktop OpenGL core-profile context, which is what Mesa's
 * nouveau driver offers on the Switch (OpenGL 4.3) and what GLideN64 asks for (3.3 core).
 * The pattern is devkitPro's switch-examples graphics/opengl.
 *
 * Every function here is called by the plugin on the emulation thread: the context is
 * created, made current, used and destroyed there (RomOpen ... RomClosed, both inside the
 * core's execute loop). Nothing here is called from the main thread.
 *
 * GLideN64 finds its OpenGL functions itself with eglGetProcAddress (it is built with
 * -DEGL), so GL_GetProcAddress below is only there for completeness. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "m64p_types.h"
#include "bench_core.h"
#include "vidext_switch.h"
#include "znx_switch.h"

#define REFRESH_NS 16666667ull

static EGLDisplay l_display = EGL_NO_DISPLAY;
static EGLSurface l_surface = EGL_NO_SURFACE;
static EGLContext l_context = EGL_NO_CONTEXT;
static bool l_initialized;
static int l_width, l_height;

/* What the plugin asked for with GL_SetAttribute. */
static int l_major = 3, l_minor = 3, l_profile = M64P_GL_CONTEXT_PROFILE_CORE;
static int l_swap_interval = 1, l_samples = 0;

static uint64_t l_swap_ns;   /* inside eglSwapBuffers since the last take */
static int l_swaps;
static uint64_t l_paced_at;  /* end of the last swap or paced wait */

static const char* egl_error(void)
{
    static char text[32];
    const EGLint error = eglGetError();
    switch (error) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: break;
    }
    snprintf(text, sizeof(text), "EGL error 0x%x", (unsigned)error);
    return text;
}

static void destroy_context(void)
{
    if (l_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(l_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (l_context != EGL_NO_CONTEXT)
            eglDestroyContext(l_display, l_context);
        if (l_surface != EGL_NO_SURFACE)
            eglDestroySurface(l_display, l_surface);
    }
    l_context = EGL_NO_CONTEXT;
    l_surface = EGL_NO_SURFACE;
}

static m64p_error vidext_init_with_render_mode(m64p_render_mode mode)
{
    if (mode != M64P_RENDER_OPENGL)
        return M64ERR_UNSUPPORTED;
    if (l_initialized)
        return M64ERR_SUCCESS;
    l_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (l_display == EGL_NO_DISPLAY || !eglInitialize(l_display, NULL, NULL)) {
        znx_log("video: cannot open the display: %s", egl_error());
        l_display = EGL_NO_DISPLAY;
        return M64ERR_SYSTEM_FAIL;
    }
    /* Desktop OpenGL, not OpenGL ES. */
    if (!eglBindAPI(EGL_OPENGL_API)) {
        znx_log("video: this EGL has no desktop OpenGL: %s", egl_error());
        eglTerminate(l_display);
        l_display = EGL_NO_DISPLAY;
        return M64ERR_SYSTEM_FAIL;
    }
    l_initialized = true;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_init(void)
{
    return vidext_init_with_render_mode(M64P_RENDER_OPENGL);
}

static m64p_error vidext_quit(void)
{
    if (!l_initialized)
        return M64ERR_NOT_INIT;
    destroy_context();
    eglTerminate(l_display);
    l_display = EGL_NO_DISPLAY;
    l_initialized = false;
    znx_log("video: closed");
    return M64ERR_SUCCESS;
}

static m64p_error vidext_list_modes(m64p_2d_size* sizes, int* count)
{
    if (sizes == NULL || count == NULL || *count < 1)
        return M64ERR_INPUT_ASSERT;
    sizes[0].uiWidth = 1280;
    sizes[0].uiHeight = 720;
    *count = 1;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_list_rates(m64p_2d_size size, int* count, int* rates)
{
    (void)size;
    if (rates == NULL || count == NULL || *count < 1)
        return M64ERR_INPUT_ASSERT;
    rates[0] = 60;
    *count = 1;
    return M64ERR_SUCCESS;
}

static EGLContext create_context(EGLConfig config, int major, int minor)
{
    const EGLint attributes[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
        l_profile == M64P_GL_CONTEXT_PROFILE_COMPATIBILITY ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR
                                                           : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, major,
        EGL_CONTEXT_MINOR_VERSION_KHR, minor,
        EGL_NONE
    };
    return eglCreateContext(l_display, config, EGL_NO_CONTEXT, attributes);
}

static m64p_error vidext_set_mode(int width, int height, int bits_per_pixel, int screen_mode, int flags)
{
    (void)bits_per_pixel; (void)screen_mode; (void)flags;
    NWindow* window = nwindowGetDefault();
    EGLConfig config;
    EGLint config_count = 0;
    Result rc;
    const EGLint config_attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        /* 24 bits of depth whatever the plugin asks for (GLideN64 asks for 16 and, without
         * frame buffer emulation, draws straight into this buffer). */
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    if (!l_initialized)
        return M64ERR_NOT_INIT;
    if (l_profile == M64P_GL_CONTEXT_PROFILE_ES) {
        znx_log("video: the plugin asked for OpenGL ES; this program gives desktop OpenGL only");
        return M64ERR_UNSUPPORTED;
    }
    if (l_samples > 1)
        znx_log("video: %dx multisampling was asked for and is not given", l_samples);
    destroy_context(); /* a second call replaces the first window */

    /* The size of the buffers the screen is fed from; the system scales them to the panel
     * or the television. */
    rc = nwindowSetDimensions(window, (u32)width, (u32)height);
    if (R_FAILED(rc))
        znx_log("video: the screen refuses %dx%d (0x%08x); its own size stays", width, height, rc);
    if (!eglChooseConfig(l_display, config_attributes, &config, 1, &config_count) || config_count < 1) {
        znx_log("video: no matching framebuffer format: %s", egl_error());
        return M64ERR_SYSTEM_FAIL;
    }
    l_surface = eglCreateWindowSurface(l_display, config, window, NULL);
    if (l_surface == EGL_NO_SURFACE) {
        znx_log("video: cannot create the window surface: %s", egl_error());
        return M64ERR_SYSTEM_FAIL;
    }
    /* What the plugin asked for; Mesa answers with the newest version that is compatible
     * with it. If it refuses, the version the Switch examples ask for. */
    l_context = create_context(config, l_major, l_minor);
    if (l_context == EGL_NO_CONTEXT) {
        znx_log("video: no OpenGL %d.%d context (%s); trying 4.3", l_major, l_minor, egl_error());
        l_context = create_context(config, 4, 3);
    }
    if (l_context == EGL_NO_CONTEXT) {
        znx_log("video: cannot create an OpenGL context: %s", egl_error());
        destroy_context();
        return M64ERR_SYSTEM_FAIL;
    }
    if (!eglMakeCurrent(l_display, l_surface, l_surface, l_context)) {
        znx_log("video: cannot make the OpenGL context current: %s", egl_error());
        destroy_context();
        return M64ERR_SYSTEM_FAIL;
    }
    if (!eglSwapInterval(l_display, l_swap_interval))
        znx_log("video: swap interval %d refused: %s", l_swap_interval, egl_error());
    l_width = width;
    l_height = height;

    znx_log("video: %dx%d, swap interval %d, asked for OpenGL %d.%d %s", width, height, l_swap_interval,
            l_major, l_minor, l_profile == M64P_GL_CONTEXT_PROFILE_COMPATIBILITY ? "compatibility" : "core");
    {
        /* The few OpenGL calls made here go through the same lookup the plugin uses. */
        const unsigned char* (*get_string)(unsigned) = (const unsigned char* (*)(unsigned))eglGetProcAddress("glGetString");
        void (*viewport)(int, int, int, int) = (void (*)(int, int, int, int))eglGetProcAddress("glViewport");
        void (*clear_color)(float, float, float, float) = (void (*)(float, float, float, float))eglGetProcAddress("glClearColor");
        void (*clear)(unsigned) = (void (*)(unsigned))eglGetProcAddress("glClear");
        if (get_string == NULL || viewport == NULL || clear_color == NULL || clear == NULL) {
            znx_log("video: eglGetProcAddress does not know the basic OpenGL functions");
            destroy_context();
            return M64ERR_SYSTEM_FAIL;
        }
        znx_log("video: GL_VENDOR %s", (const char*)get_string(0x1F00));
        znx_log("video: GL_RENDERER %s", (const char*)get_string(0x1F01));
        znx_log("video: GL_VERSION %s", (const char*)get_string(0x1F02));
        znx_log("video: GLSL %s", (const char*)get_string(0x8B8C));
        {
            /* GL_NUM_PROGRAM_BINARY_FORMATS: whether compiled shaders can be saved and loaded back. */
            void (*get_integerv)(unsigned, int*) = (void (*)(unsigned, int*))eglGetProcAddress("glGetIntegerv");
            int formats = -1;
            if (get_integerv != NULL) get_integerv(0x87FE, &formats);
            znx_log("video: %d program binary formats (%s)", formats,
                    formats > 0 ? "compiled shaders are saved to the SD card"
                                : "the shader list is saved and compiled at start");
        }
        /* Black in every buffer of the swap chain (four swaps cover the most it can have):
         * without frame buffer emulation the plugin only ever draws the 4:3 middle. */
        for (int i = 0; i < 4; ++i) {
            viewport(0, 0, width, height);
            clear_color(0.0f, 0.0f, 0.0f, 1.0f);
            clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
            eglSwapBuffers(l_display, l_surface);
        }
    }
    l_swap_ns = 0;
    l_swaps = 0;
    l_paced_at = znx_now_ns();
    return M64ERR_SUCCESS;
}

static m64p_error vidext_set_mode_with_rate(int width, int height, int rate, int bits_per_pixel, int screen_mode, int flags)
{
    (void)rate;
    return vidext_set_mode(width, height, bits_per_pixel, screen_mode, flags);
}

static m64p_function vidext_gl_get_proc(const char* name)
{
    return (m64p_function)eglGetProcAddress(name);
}

static m64p_error vidext_gl_set_attribute(m64p_GLattr attribute, int value)
{
    switch (attribute) {
    case M64P_GL_CONTEXT_MAJOR_VERSION: l_major = value; break;
    case M64P_GL_CONTEXT_MINOR_VERSION: l_minor = value; break;
    case M64P_GL_CONTEXT_PROFILE_MASK: l_profile = value; break;
    case M64P_GL_MULTISAMPLESAMPLES: l_samples = value; break;
    case M64P_GL_SWAP_CONTROL:
        l_swap_interval = value;
        if (l_context != EGL_NO_CONTEXT)
            eglSwapInterval(l_display, value);
        break;
    default: break; /* double buffering and 8 bits per channel are what there is */
    }
    return M64ERR_SUCCESS;
}

static m64p_error vidext_gl_get_attribute(m64p_GLattr attribute, int* value)
{
    if (value == NULL)
        return M64ERR_INPUT_ASSERT;
    switch (attribute) {
    case M64P_GL_DOUBLEBUFFER: *value = 1; break;
    case M64P_GL_BUFFER_SIZE: *value = 32; break;
    case M64P_GL_DEPTH_SIZE: *value = 24; break;
    case M64P_GL_RED_SIZE: case M64P_GL_GREEN_SIZE: case M64P_GL_BLUE_SIZE: case M64P_GL_ALPHA_SIZE: *value = 8; break;
    case M64P_GL_SWAP_CONTROL: *value = l_swap_interval; break;
    case M64P_GL_MULTISAMPLEBUFFERS: *value = 0; break;
    case M64P_GL_MULTISAMPLESAMPLES: *value = 0; break;
    case M64P_GL_CONTEXT_MAJOR_VERSION: *value = l_major; break;
    case M64P_GL_CONTEXT_MINOR_VERSION: *value = l_minor; break;
    case M64P_GL_CONTEXT_PROFILE_MASK: *value = l_profile; break;
    default: return M64ERR_INPUT_INVALID;
    }
    return M64ERR_SUCCESS;
}

/* With swap interval 1 this returns after the next display refresh has taken the frame:
 * the wait that keeps the game at 60 Hz. */
static m64p_error vidext_gl_swap_buffers(void)
{
    uint64_t begin, end;
    if (l_surface == EGL_NO_SURFACE)
        return M64ERR_NOT_INIT;
    begin = znx_now_ns();
    if (!eglSwapBuffers(l_display, l_surface)) {
        static int reported;
        if (reported++ < 5)
            znx_log("video: buffer swap failed: %s", egl_error());
        return M64ERR_SYSTEM_FAIL;
    }
    end = znx_now_ns();
    l_swap_ns += end - begin;
    l_swaps++;
    l_paced_at = end;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_set_caption(const char* title)
{
    znx_log("video: plugin \"%s\"", title != NULL ? title : "");
    return M64ERR_SUCCESS;
}

static m64p_error vidext_toggle_fullscreen(void)
{
    return M64ERR_SUCCESS; /* the screen is always full */
}

static m64p_error vidext_resize_window(int width, int height)
{
    (void)width; (void)height;
    return M64ERR_SUCCESS;
}

static uint32_t vidext_gl_get_default_framebuffer(void)
{
    return 0; /* the window surface */
}

static m64p_error vidext_vk_get_surface(void** surface, void* instance)
{
    (void)surface; (void)instance;
    return M64ERR_UNSUPPORTED;
}

static m64p_error vidext_vk_get_instance_extensions(const char** extensions[], uint32_t* count)
{
    (void)extensions; (void)count;
    return M64ERR_UNSUPPORTED;
}

static m64p_video_extension_functions l_functions = {
    17,
    vidext_init,
    vidext_quit,
    vidext_list_modes,
    vidext_list_rates,
    vidext_set_mode,
    vidext_set_mode_with_rate,
    vidext_gl_get_proc,
    vidext_gl_set_attribute,
    vidext_gl_get_attribute,
    vidext_gl_swap_buffers,
    vidext_set_caption,
    vidext_toggle_fullscreen,
    vidext_resize_window,
    vidext_gl_get_default_framebuffer,
    vidext_init_with_render_mode,
    vidext_vk_get_surface,
    vidext_vk_get_instance_extensions,
};

void* znx_vidext_functions(void)
{
    return &l_functions;
}

uint64_t znx_vidext_take_swap_ns(void)
{
    uint64_t ns = l_swap_ns;
    l_swap_ns = 0;
    return ns;
}

int znx_vidext_take_swaps(void)
{
    int swaps = l_swaps;
    l_swaps = 0;
    return swaps;
}

void znx_vidext_pace_without_swap(void)
{
    uint64_t now = znx_now_ns();
    uint64_t due = l_paced_at + REFRESH_NS;
    if (l_swap_interval > 0 && due > now && due - now <= REFRESH_NS)
        svcSleepThread((s64)(due - now));
    now = znx_now_ns();
    /* Behind by more than a refresh: start counting from now instead of catching up. */
    l_paced_at = now > due + REFRESH_NS ? now : due;
}
