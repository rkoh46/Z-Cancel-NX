// SPDX-License-Identifier: GPL-3.0-or-later
/* See overlay_switch.h.
 *
 * Drawn on the emulation thread inside the video extension's buffer swap, after GLideN64 has finished
 * the frame and before it is shown, in GLideN64's own OpenGL context, with the start screen's renderer
 * (menu/draw.c) and font. It only adds pixels to the window's framebuffer: nothing goes near RDRAM or
 * the emulated machine, so it cannot change the game. GLideN64 caches OpenGL state (it skips calls that
 * would set what it believes is already set), so every piece of state draw.c touches is read before and
 * put back after: the cache stays true. The text sits in the black margin left of the 4:3 picture. */

#include "overlay_switch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>

#include "draw.h"
#include "znx_switch.h"

#define FONT_PATH "romfs:/fonts/BarlowSemiCondensed-Medium.ttf"

#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_STENCIL_TEST 0x0B90
#define GL_BLEND 0x0BE2
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_SAMPLER_BINDING 0x8919
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_ARRAY_BUFFER 0x8892
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_2D 0x0DE1

typedef unsigned GLenum;
typedef int GLint;
typedef unsigned char GLboolean;

static struct {
    void (*GetIntegerv)(GLenum, GLint*);
    void (*GetBooleanv)(GLenum, GLboolean*);
    GLboolean (*IsEnabled)(GLenum);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*Viewport)(GLint, GLint, GLint, GLint);
    void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (*BlendEquationSeparate)(GLenum, GLenum);
    void (*UseProgram)(unsigned);
    void (*BindVertexArray)(unsigned);
    void (*BindBuffer)(GLenum, unsigned);
    void (*ActiveTexture)(GLenum);
    void (*BindTexture)(GLenum, unsigned);
    void (*BindSampler)(unsigned, unsigned);
    void (*BindFramebuffer)(GLenum, unsigned);
    void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void (*PixelStorei)(GLenum, GLint);
} gl;

static void* l_font;
static size_t l_font_size;
static int l_ready, l_failed;

static void* getproc(const char* name)
{
    return (void*)eglGetProcAddress(name);
}

static int load(void)
{
    char error[256];
    FILE* f;
    long size;
    if (l_ready || l_failed)
        return l_ready;
    l_failed = 1;
#define GET(name) if ((*(void**)&gl.name = getproc("gl" #name)) == NULL) { znx_log("overlay: no gl" #name); return 0; }
    GET(GetIntegerv) GET(GetBooleanv) GET(IsEnabled) GET(Enable) GET(Disable) GET(Viewport) GET(BlendFuncSeparate)
    GET(BlendEquationSeparate) GET(UseProgram) GET(BindVertexArray) GET(BindBuffer) GET(ActiveTexture)
    GET(BindTexture) GET(BindSampler) GET(BindFramebuffer) GET(ColorMask) GET(PixelStorei)
#undef GET
    f = fopen(FONT_PATH, "rb");
    if (f == NULL) {
        znx_log("overlay: cannot open " FONT_PATH);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    l_font = size > 0 ? malloc((size_t)size) : NULL;
    if (l_font == NULL || fread(l_font, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(l_font);
        l_font = NULL;
        znx_log("overlay: cannot read the font");
        return 0;
    }
    fclose(f);
    l_font_size = (size_t)size;
    {
        /* draw.c's init binds its own objects: keep GLideN64's bindings around it too */
        GLint program = 0, vao = 0, buffer = 0, active = 0, texture = 0, align = 4;
        gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
        gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        const int ok = znx_draw_init(getproc, l_font, l_font_size, error, sizeof(error));
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, align);
        gl.BindTexture(GL_TEXTURE_2D, (unsigned)texture);
        gl.ActiveTexture((GLenum)active);
        gl.BindBuffer(GL_ARRAY_BUFFER, (unsigned)buffer);
        gl.BindVertexArray((unsigned)vao);
        gl.UseProgram((unsigned)program);
        if (!ok) {
            znx_log("overlay: %s", error);
            znx_draw_shutdown();
            return 0;
        }
    }
    l_failed = 0;
    l_ready = 1;
    znx_log("overlay: ready");
    return 1;
}

void znx_overlay_draw(int width, int height, const char* const* labels, const char* const* values, int count)
{
    GLint viewport[4], program, vao, buffer, active, texture, sampler, draw_fb, read_fb, align, row_length;
    GLint src_rgb, dst_rgb, src_alpha, dst_alpha, eq_rgb, eq_alpha;
    GLboolean mask[4];
    GLboolean scissor, depth, cull, stencil, blend, srgb;
    if (!load())
        return;
    gl.GetIntegerv(GL_VIEWPORT, viewport);
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    gl.GetIntegerv(GL_SAMPLER_BINDING, &sampler);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    gl.GetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    gl.GetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
    gl.GetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
    gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb);
    gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_alpha);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    depth = gl.IsEnabled(GL_DEPTH_TEST);
    cull = gl.IsEnabled(GL_CULL_FACE);
    stencil = gl.IsEnabled(GL_STENCIL_TEST);
    blend = gl.IsEnabled(GL_BLEND);
    srgb = gl.IsEnabled(GL_FRAMEBUFFER_SRGB);

    gl.BindSampler(0, 0);
    gl.ColorMask(1, 1, 1, 1);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_FRAMEBUFFER_SRGB);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    znx_draw_begin_over(0, width, height);
    for (int i = 0; i < count; ++i) {
        const float y = 56 + i * 28;
        znx_draw_text(ZNX_FONT_SMALL, 24, y, labels[i], 0x7D7986FFu, 0);
        znx_draw_text(ZNX_FONT_SMALL, 136, y, values[i], 0xEDE9E1FFu, ZNX_TEXT_RIGHT | ZNX_TEXT_TABULAR);
    }
    znx_draw_end();

    /* everything back as GLideN64 left it */
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, align);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned)draw_fb);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned)read_fb);
    gl.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    (scissor ? gl.Enable : gl.Disable)(GL_SCISSOR_TEST);
    (depth ? gl.Enable : gl.Disable)(GL_DEPTH_TEST);
    (cull ? gl.Enable : gl.Disable)(GL_CULL_FACE);
    (stencil ? gl.Enable : gl.Disable)(GL_STENCIL_TEST);
    (blend ? gl.Enable : gl.Disable)(GL_BLEND);
    (srgb ? gl.Enable : gl.Disable)(GL_FRAMEBUFFER_SRGB);
    gl.BlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_alpha, (GLenum)dst_alpha);
    gl.BlendEquationSeparate((GLenum)eq_rgb, (GLenum)eq_alpha);
    gl.BindSampler(0, (unsigned)sampler);
    gl.BindTexture(GL_TEXTURE_2D, (unsigned)texture);
    gl.ActiveTexture((GLenum)active);
    gl.BindBuffer(GL_ARRAY_BUFFER, (unsigned)buffer);
    gl.BindVertexArray((unsigned)vao);
    gl.UseProgram((unsigned)program);
}

void znx_overlay_shutdown(void)
{
    /* with GLideN64's context still current: draw.c's objects go with it */
    if (l_ready)
        znx_draw_shutdown();
    l_ready = 0;
    l_failed = 0;
}
