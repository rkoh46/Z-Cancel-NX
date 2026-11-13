// SPDX-License-Identifier: GPL-3.0-or-later
/* The core's video extension without a window, for checking the picture build on a Mac:
 * an OpenGL 4.1 core context from CGL with no drawable, and a framebuffer object that
 * GLideN64 is told is the "default framebuffer" (VidExt_GL_GetDefaultFramebuffer, the same
 * way RMG-K hands it Qt's). Nothing appears on screen; a buffer swap can save the finished
 * frame as a PNG. The Switch build's counterpart is switch/vidext_switch.c (EGL). */

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "m64p_types.h"
#include "bench_core.h"
#include "vidext_cgl.h"

static CGLContextObj l_context;
static GLuint l_fbo, l_color, l_depth;
static int l_width, l_height;
static uint64_t l_swap_ns;
static int l_swaps;
static void (*l_print)(int is_error, const char* line);

#define MAX_SHOTS 16
static struct { int frame; const char* path; int done; } l_shots[MAX_SHOTS];
static int l_shot_count;

static void say(const char* format, const char* a, const char* b)
{
    char line[512];
    snprintf(line, sizeof(line), format, a, b);
    if (l_print != NULL)
        l_print(0, line);
}

void znx_cgl_begin(void (*print)(int is_error, const char* line)) { l_print = print; }

void znx_cgl_shot(int frame, const char* path)
{
    if (l_shot_count < MAX_SHOTS) {
        l_shots[l_shot_count].frame = frame;
        l_shots[l_shot_count].path = path;
        l_shots[l_shot_count].done = 0;
        l_shot_count++;
    }
}

/* ---- a PNG of the framebuffer object: 8-bit RGB, rows top to bottom ---- */

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
    const size_t row = (size_t)l_width * 3;
    unsigned char* pixels = malloc(row * l_height);
    unsigned char* raw = malloc((row + 1) * l_height);
    uLongf packed_size = compressBound((row + 1) * l_height);
    unsigned char* packed = malloc(packed_size);
    unsigned char header[13] = { l_width >> 24, l_width >> 16, l_width >> 8, l_width,
                                 l_height >> 24, l_height >> 16, l_height >> 8, l_height, 8, 2, 0, 0, 0 };
    FILE* f;
    GLint read_fbo = 0;
    long lit = 0;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, l_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, l_width, l_height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
    for (int y = 0; y < l_height; ++y) {   /* OpenGL's rows run bottom to top */
        raw[(row + 1) * y] = 0;
        memcpy(raw + (row + 1) * y + 1, pixels + row * (l_height - 1 - y), row);
    }
    for (size_t i = 0; i < row * l_height; ++i)
        lit += pixels[i] > 16;
    compress2(packed, &packed_size, raw, (row + 1) * l_height, 6);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        chunk(f, "IHDR", header, 13);
        chunk(f, "IDAT", packed, (uint32_t)packed_size);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(pixels); free(raw); free(packed);
    {
        char line[512];
        snprintf(line, sizeof(line), "shot %d %s: %dx%d, %.1f%% of the colour values are above 16%s", znx_bench_frame(), path,
                 l_width, l_height, 100.0 * lit / (row * l_height), f != NULL ? "" : " (CANNOT WRITE THE FILE)");
        if (l_print != NULL)
            l_print(0, line);
    }
    return f != NULL;
}

/* ---- the video extension ---- */

static m64p_error vidext_init_with_render_mode(m64p_render_mode mode)
{
    return mode == M64P_RENDER_OPENGL ? M64ERR_SUCCESS : M64ERR_UNSUPPORTED;
}

static m64p_error vidext_init(void) { return M64ERR_SUCCESS; }

static m64p_error vidext_quit(void)
{
    if (l_context != NULL) {
        glDeleteFramebuffers(1, &l_fbo);
        glDeleteTextures(1, &l_color);
        glDeleteRenderbuffers(1, &l_depth);
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(l_context);
        l_context = NULL;
    }
    return M64ERR_SUCCESS;
}

static m64p_error vidext_list_modes(m64p_2d_size* sizes, int* count)
{
    sizes[0].uiWidth = 1280;
    sizes[0].uiHeight = 720;
    *count = 1;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_list_rates(m64p_2d_size size, int* count, int* rates)
{
    (void)size;
    rates[0] = 60;
    *count = 1;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_set_mode(int width, int height, int bits_per_pixel, int screen_mode, int flags)
{
    (void)bits_per_pixel; (void)screen_mode; (void)flags;
    const CGLPixelFormatAttribute attributes[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj format = NULL;
    GLint formats = 0;
    if (l_context != NULL)
        vidext_quit();
    if (CGLChoosePixelFormat(attributes, &format, &formats) != kCGLNoError || format == NULL ||
        CGLCreateContext(format, NULL, &l_context) != kCGLNoError) {
        say("video: cannot create an OpenGL 4.1 core context%s%s", "", "");
        return M64ERR_SYSTEM_FAIL;
    }
    CGLDestroyPixelFormat(format);
    CGLSetCurrentContext(l_context);
    l_width = width;
    l_height = height;

    glGenFramebuffers(1, &l_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, l_fbo);
    glGenTextures(1, &l_color);
    glBindTexture(GL_TEXTURE_2D, l_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l_color, 0);
    glGenRenderbuffers(1, &l_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, l_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, l_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        say("video: the offscreen framebuffer is incomplete%s%s", "", "");
        return M64ERR_SYSTEM_FAIL;
    }
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    say("video: GL_RENDERER %s, GL_VERSION %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    return M64ERR_SUCCESS;
}

static m64p_error vidext_set_mode_with_rate(int width, int height, int rate, int bits_per_pixel, int screen_mode, int flags)
{
    (void)rate;
    return vidext_set_mode(width, height, bits_per_pixel, screen_mode, flags);
}

static m64p_function vidext_gl_get_proc(const char* name)
{
    return (m64p_function)dlsym(RTLD_DEFAULT, name);
}

static m64p_error vidext_gl_set_attribute(m64p_GLattr attribute, int value)
{
    (void)attribute; (void)value;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_gl_get_attribute(m64p_GLattr attribute, int* value)
{
    (void)attribute;
    *value = 0;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_gl_swap_buffers(void)
{
    uint64_t begin = znx_now_ns();
    /* Frames completed so far == the index of the frame whose picture this is. */
    for (int i = 0; i < l_shot_count; ++i) {
        if (!l_shots[i].done && znx_bench_frame() >= l_shots[i].frame) {
            l_shots[i].done = 1;
            save_png(l_shots[i].path);
        }
    }
    glFlush();
    l_swap_ns += znx_now_ns() - begin;
    l_swaps++;
    return M64ERR_SUCCESS;
}

static m64p_error vidext_set_caption(const char* title)
{
    say("video: plugin \"%s\"%s", title, "");
    return M64ERR_SUCCESS;
}

static m64p_error vidext_toggle_fullscreen(void) { return M64ERR_SUCCESS; }
static m64p_error vidext_resize_window(int width, int height) { (void)width; (void)height; return M64ERR_SUCCESS; }
static uint32_t vidext_gl_get_default_framebuffer(void) { return l_fbo; }
static m64p_error vidext_vk_get_surface(void** surface, void* instance) { (void)surface; (void)instance; return M64ERR_UNSUPPORTED; }
static m64p_error vidext_vk_get_instance_extensions(const char** extensions[], uint32_t* count) { (void)extensions; (void)count; return M64ERR_UNSUPPORTED; }

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

void* znx_cgl_functions(void) { return &l_functions; }

uint64_t znx_cgl_take_swap_ns(void)
{
    uint64_t ns = l_swap_ns;
    l_swap_ns = 0;
    return ns;
}

int znx_cgl_take_swaps(void)
{
    int swaps = l_swaps;
    l_swaps = 0;
    return swaps;
}
