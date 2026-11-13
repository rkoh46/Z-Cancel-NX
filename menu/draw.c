// SPDX-License-Identifier: GPL-3.0-or-later
/* See draw.h. */

#include "draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

/* ---- the OpenGL functions used, looked up through getproc ---- */

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef unsigned int GLbitfield;

#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLES 0x0004
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_SRGB 0x8DB9

#define GL_FUNCTIONS(X) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glDeleteShader, (GLuint)) \
    X(void, glDeleteProgram, (GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glDeleteVertexArrays, (GLsizei, const GLuint*)) \
    X(void, glGenBuffers, (GLsizei, GLuint*)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint*)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void, glGenTextures, (GLsizei, GLuint*)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint*)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glBindFramebuffer, (GLenum, GLuint))

#define DECLARE(ret, name, args) static ret (*p_##name) args;
GL_FUNCTIONS(DECLARE)

/* ---- state ---- */

static const int l_font_px[ZNX_FONT_COUNT] = { 18, 20, 22, 28, 40 };
/* Baseline to baseline in wrapped text, on the 4-pixel grid. */
static const float l_line_step[ZNX_FONT_COUNT] = { 24, 28, 32, 40, 52 };

#define ATLAS 1024
#define FIRST_CHAR 32
#define LAST_CHAR 126
static const unsigned l_extra_chars[] = { 0x00B7, 0x2026, 0x2013, 0x2014, 0x2019, 0x00D7, 0x2192, 0x2190, 0x2212, 0x00A0 };
#define EXTRA_COUNT (sizeof(l_extra_chars) / sizeof(l_extra_chars[0]))
#define GLYPHS (LAST_CHAR - FIRST_CHAR + 1 + EXTRA_COUNT)
/* The symbol font's range: the console's button glyphs. */
#define SYM_FIRST 0xE0A0u
#define SYM_LAST 0xE0FFu
#define SYM_COUNT (SYM_LAST - SYM_FIRST + 1)

struct glyph {
    short x, y, w, h;      /* in the atlas */
    short left, top;       /* bitmap offset from the pen position, top up */
    float advance;
    int present;
};

struct font {
    struct glyph glyphs[GLYPHS];
    struct glyph symbols[SYM_COUNT];
    float line_height, cap_height, tabular_advance;
};

struct vertex {
    float x, y, u, v;
    float lx, ly, hx, hy;   /* position inside the shape relative to its centre, and the shape's half size */
    float radius, mode;     /* mode 0: atlas alpha; 1: rounded box; 2: its outline, u pixels wide */
    uint8_t r, g, b, a;
};

#define MARK_SIZES 2
/* Rasterised at the sizes drawn (the header's 22.5-pixel letter is a 24-pixel square), so the cut stays sharp. */
static const int l_mark_px[MARK_SIZES] = { 24, 160 };

static struct {
    int ready;
    GLuint program, vao, vbo, atlas;
    GLint u_screen, u_atlas;
    struct font fonts[ZNX_FONT_COUNT];
    struct { short x, y, size; } marks[MARK_SIZES];
    struct vertex* vertices;
    int count, capacity;
    int width, height;
    FT_Library library;
    FT_Face face, symbol_face;
    uint8_t* pixels;        /* the atlas while it is built */
    int pen_x, pen_y, row_h;
} g;

/* Set before znx_draw_init, which clears g. */
static const void* l_symbol_data;
static size_t l_symbol_size;
static unsigned l_frames;       /* frames drawn, for the selection outline's pulse */

void znx_draw_set_symbol_font(const void* font_data, size_t font_size)
{
    l_symbol_data = font_data;
    l_symbol_size = font_data != NULL ? font_size : 0;
}

/* ---- shaders ---- */

static const char* l_vertex_shader =
    "#version 330 core\n"
    "uniform vec2 u_screen;\n"
    "in vec2 a_pos; in vec2 a_uv; in vec4 a_local; in vec2 a_shape; in vec4 a_color;\n"
    "out vec2 v_uv; out vec4 v_local; out vec2 v_shape; out vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "    v_uv = a_uv; v_local = a_local; v_shape = a_shape; v_color = a_color;\n"
    "}\n";

static const char* l_fragment_shader =
    "#version 330 core\n"
    "uniform sampler2D u_atlas;\n"
    "in vec2 v_uv; in vec4 v_local; in vec2 v_shape; in vec4 v_color;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    float alpha;\n"
    "    if (v_shape.y < 0.5) {\n"
    "        alpha = texture(u_atlas, v_uv).r;\n"
    "    } else {\n"
    "        float r = v_shape.x;\n"
    "        vec2 q = abs(v_local.xy) - v_local.zw + vec2(r);\n"
    "        float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
    "        alpha = clamp(0.5 - d, 0.0, 1.0);\n"
    "        if (v_shape.y > 1.5) alpha *= clamp(0.5 + d + v_uv.x, 0.0, 1.0);\n"
    "    }\n"
    "    o_color = vec4(v_color.rgb, v_color.a * alpha);\n"
    "}\n";

static GLuint compile(GLenum type, const char* source, char* error, size_t error_size)
{
    GLuint shader = p_glCreateShader(type);
    GLint ok = 0;
    p_glShaderSource(shader, 1, &source, NULL);
    p_glCompileShader(shader);
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        p_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        snprintf(error, error_size, "menu: shader does not compile: %s", log);
        p_glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* ---- the mark: tools/make_logo.py's geometry, rasterised here ---- */

/* The letter on the logo's 1000-unit square: width 520, height 600, bar 150, diagonal run 215. */
static const float l_z[10][2] = {
    { 240, 200 }, { 760, 200 }, { 760, 350 }, { 455, 650 }, { 760, 650 },
    { 760, 800 }, { 240, 800 }, { 240, 650 }, { 545, 350 }, { 240, 350 },
};

static int inside_z(float x, float y)
{
    int in = 0;
    for (int i = 0, j = 9; i < 10; j = i++) {
        const float xi = l_z[i][0], yi = l_z[i][1], xj = l_z[j][0], yj = l_z[j][1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            in = !in;
    }
    return in;
}

/* Cut through the middle (gap 30), halves slid 39 units apart, sheared by 0.17 about the centre. */
static int inside_mark(float u, float v)
{
    const float x = u + 0.17f * (v - 500.0f);
    if (v < 485.0f) return inside_z(x + 39.0f, v);
    if (v >= 515.0f) return inside_z(x - 39.0f, v);
    return 0;
}

/* The letter's box is 240..760 x 200..800 of the 1000 units; with the slide and the shear it reaches
 * about 198..800 horizontally. The raster covers the square 180..820 in both directions. */
#define MARK_X0 180.0f
#define MARK_X1 820.0f
#define MARK_Y0 180.0f
#define MARK_Y1 820.0f

static void rasterise_mark(uint8_t* out, int stride, int size)
{
    const int ss = 4;
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px) {
            int hits = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    float u = MARK_X0 + (px + (sx + 0.5f) / ss) / size * (MARK_X1 - MARK_X0);
                    float v = MARK_Y0 + (py + (sy + 0.5f) / ss) / size * (MARK_Y1 - MARK_Y0);
                    hits += inside_mark(u, v);
                }
            out[py * stride + px] = (uint8_t)(hits * 255 / (ss * ss));
        }
}

/* ---- the atlas ---- */

static int atlas_place(int w, int h, int* x, int* y)
{
    if (g.pen_x + w + 1 > ATLAS) {
        g.pen_x = 1;
        g.pen_y += g.row_h + 1;
        g.row_h = 0;
    }
    if (g.pen_y + h + 1 > ATLAS)
        return 0;
    *x = g.pen_x;
    *y = g.pen_y;
    g.pen_x += w + 1;
    if (h > g.row_h) g.row_h = h;
    return 1;
}

static unsigned glyph_code(int index)
{
    return index <= LAST_CHAR - FIRST_CHAR ? (unsigned)(FIRST_CHAR + index) : l_extra_chars[index - (LAST_CHAR - FIRST_CHAR + 1)];
}

static int glyph_index(unsigned code)
{
    if (code >= FIRST_CHAR && code <= LAST_CHAR)
        return (int)(code - FIRST_CHAR);
    for (unsigned i = 0; i < EXTRA_COUNT; ++i)
        if (l_extra_chars[i] == code)
            return (int)(LAST_CHAR - FIRST_CHAR + 1 + i);
    return -1;
}

/* One glyph into the atlas. 0 only when the atlas is full; a glyph the face lacks stays absent. */
static int load_glyph(FT_Face face, unsigned code, struct glyph* gl)
{
    FT_UInt id = FT_Get_Char_Index(face, code);
    FT_Bitmap* bitmap;
    int x, y;
    memset(gl, 0, sizeof(*gl));
    if (id == 0 || FT_Load_Glyph(face, id, FT_LOAD_TARGET_LIGHT) != 0 ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LIGHT) != 0)
        return 1;
    bitmap = &face->glyph->bitmap;
    if (!atlas_place((int)bitmap->width, (int)bitmap->rows, &x, &y))
        return 0;
    for (unsigned r = 0; r < bitmap->rows; ++r)
        memcpy(g.pixels + (y + r) * ATLAS + x, bitmap->buffer + r * bitmap->pitch, bitmap->width);
    gl->x = (short)x; gl->y = (short)y;
    gl->w = (short)bitmap->width; gl->h = (short)bitmap->rows;
    gl->left = (short)face->glyph->bitmap_left;
    gl->top = (short)face->glyph->bitmap_top;
    gl->advance = face->glyph->advance.x / 64.0f;
    gl->present = 1;
    return 1;
}

static int build_font(int index, char* error, size_t error_size)
{
    struct font* font = &g.fonts[index];
    FT_Set_Pixel_Sizes(g.face, 0, (FT_UInt)l_font_px[index]);
    font->line_height = l_line_step[index];
    font->tabular_advance = 0;
    for (int i = 0; i < (int)GLYPHS; ++i)
        if (!load_glyph(g.face, glyph_code(i), &font->glyphs[i])) {
            snprintf(error, error_size, "menu: the glyph atlas is full");
            return 0;
        }
    /* The button glyphs, at the two sizes they are drawn in (the footer's hints, the glyph check). */
    if (g.symbol_face != NULL && (index == ZNX_FONT_ROW || index == ZNX_FONT_BODY)) {
        FT_Set_Pixel_Sizes(g.symbol_face, 0, (FT_UInt)l_font_px[index]);
        for (unsigned i = 0; i < SYM_COUNT; ++i)
            if (!load_glyph(g.symbol_face, SYM_FIRST + i, &font->symbols[i])) {
                snprintf(error, error_size, "menu: the glyph atlas is full");
                return 0;
            }
    }
    {
        /* Tabular figures: the widest of 0-9. Letters keep their own widths (padding every digit to a
         * letter's width left visible gaps between digits). */
        static const char tab[] = "0123456789";
        for (const char* c = tab; *c; ++c) {
            const struct glyph* gl = &font->glyphs[*c - FIRST_CHAR];
            if (gl->advance > font->tabular_advance) font->tabular_advance = gl->advance;
        }
        font->cap_height = font->glyphs['H' - FIRST_CHAR].top;
    }
    return 1;
}

int znx_draw_init(void* (*getproc)(const char*), const void* font_data, size_t font_size, char* error, size_t error_size)
{
    GLuint vs, fs;
    GLint ok = 0;

    memset(&g, 0, sizeof(g));
#define LOAD(ret, name, args) \
    p_##name = (ret (*) args)getproc(#name); \
    if (p_##name == NULL) { snprintf(error, error_size, "menu: no OpenGL function " #name); return 0; }
    GL_FUNCTIONS(LOAD)
#undef LOAD

    if (FT_Init_FreeType(&g.library) != 0) {
        snprintf(error, error_size, "menu: FreeType does not start");
        return 0;
    }
    if (FT_New_Memory_Face(g.library, font_data, (FT_Long)font_size, 0, &g.face) != 0) {
        snprintf(error, error_size, "menu: the font cannot be read");
        FT_Done_FreeType(g.library);
        return 0;
    }
    /* A symbol font that cannot be read is not an error: the stand-ins are drawn instead. */
    if (l_symbol_data != NULL && FT_New_Memory_Face(g.library, l_symbol_data, (FT_Long)l_symbol_size, 0, &g.symbol_face) != 0)
        g.symbol_face = NULL;
    g.pixels = calloc(ATLAS, ATLAS);
    if (g.pixels == NULL) {
        snprintf(error, error_size, "menu: no memory for the glyph atlas");
        return 0;
    }
    g.pen_x = g.pen_y = 1;
    for (int i = 0; i < ZNX_FONT_COUNT; ++i)
        if (!build_font(i, error, error_size))
            return 0;
    for (int i = 0; i < MARK_SIZES; ++i) {
        int x, y;
        if (!atlas_place(l_mark_px[i], l_mark_px[i], &x, &y)) {
            snprintf(error, error_size, "menu: the glyph atlas is full");
            return 0;
        }
        rasterise_mark(g.pixels + y * ATLAS + x, ATLAS, l_mark_px[i]);
        g.marks[i].x = (short)x; g.marks[i].y = (short)y; g.marks[i].size = (short)l_mark_px[i];
    }
    FT_Done_Face(g.face);
    if (g.symbol_face != NULL) FT_Done_Face(g.symbol_face);
    FT_Done_FreeType(g.library);
    g.face = NULL;
    g.symbol_face = NULL;
    g.library = NULL;

    p_glGenTextures(1, &g.atlas);
    p_glBindTexture(GL_TEXTURE_2D, g.atlas);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS, ATLAS, 0, GL_RED, GL_UNSIGNED_BYTE, g.pixels);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(g.pixels);
    g.pixels = NULL;

    vs = compile(GL_VERTEX_SHADER, l_vertex_shader, error, error_size);
    fs = vs ? compile(GL_FRAGMENT_SHADER, l_fragment_shader, error, error_size) : 0;
    if (!vs || !fs)
        return 0;
    g.program = p_glCreateProgram();
    p_glAttachShader(g.program, vs);
    p_glAttachShader(g.program, fs);
    p_glBindAttribLocation(g.program, 0, "a_pos");
    p_glBindAttribLocation(g.program, 1, "a_uv");
    p_glBindAttribLocation(g.program, 2, "a_local");
    p_glBindAttribLocation(g.program, 3, "a_shape");
    p_glBindAttribLocation(g.program, 4, "a_color");
    p_glLinkProgram(g.program);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    p_glGetProgramiv(g.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        p_glGetProgramInfoLog(g.program, sizeof(log), NULL, log);
        snprintf(error, error_size, "menu: shaders do not link: %s", log);
        return 0;
    }
    g.u_screen = p_glGetUniformLocation(g.program, "u_screen");
    g.u_atlas = p_glGetUniformLocation(g.program, "u_atlas");

    p_glGenVertexArrays(1, &g.vao);
    p_glBindVertexArray(g.vao);
    p_glGenBuffers(1, &g.vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(struct vertex), (void*)offsetof(struct vertex, x));
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(struct vertex), (void*)offsetof(struct vertex, u));
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(struct vertex), (void*)offsetof(struct vertex, lx));
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(3, 2, GL_FLOAT, 0, sizeof(struct vertex), (void*)offsetof(struct vertex, radius));
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, 1, sizeof(struct vertex), (void*)offsetof(struct vertex, r));
    p_glBindVertexArray(0);

    g.capacity = 6 * 4096;
    g.vertices = malloc(sizeof(struct vertex) * (size_t)g.capacity);
    if (g.vertices == NULL) {
        snprintf(error, error_size, "menu: no memory for vertices");
        return 0;
    }
    g.ready = 1;
    return 1;
}

void znx_draw_shutdown(void)
{
    if (g.program) p_glDeleteProgram(g.program);
    if (g.vbo) p_glDeleteBuffers(1, &g.vbo);
    if (g.vao) p_glDeleteVertexArrays(1, &g.vao);
    if (g.atlas) p_glDeleteTextures(1, &g.atlas);
    if (g.face) FT_Done_Face(g.face);
    if (g.symbol_face) FT_Done_Face(g.symbol_face);
    if (g.library) FT_Done_FreeType(g.library);
    free(g.vertices);
    free(g.pixels);
    memset(&g, 0, sizeof(g));
}

/* ---- drawing ---- */

static void flush(void)
{
    if (g.count == 0)
        return;
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(struct vertex) * (size_t)g.count), g.vertices, GL_STREAM_DRAW);
    p_glDrawArrays(GL_TRIANGLES, 0, g.count);
    g.count = 0;
}

void znx_draw_begin(unsigned framebuffer, int width, int height, uint32_t clear)
{
    g.width = width;
    g.height = height;
    l_frames++;
    p_glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    p_glViewport(0, 0, width, height);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    p_glClearColor((clear >> 24) / 255.0f, ((clear >> 16) & 255) / 255.0f, ((clear >> 8) & 255) / 255.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(g.program);
    /* Everything is laid out for 1280x720 and scaled to the framebuffer. */
    p_glUniform2f(g.u_screen, 1280.0f, 720.0f);
    p_glUniform1i(g.u_atlas, 0);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, g.atlas);
    p_glBindVertexArray(g.vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    g.count = 0;
}

void znx_draw_begin_over(unsigned framebuffer, int width, int height)
{
    /* znx_draw_begin without the clear: drawing over a finished picture (the match overlay) */
    g.width = width;
    g.height = height;
    p_glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    p_glViewport(0, 0, width, height);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(g.program);
    p_glUniform2f(g.u_screen, 1280.0f, 720.0f);
    p_glUniform1i(g.u_atlas, 0);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, g.atlas);
    p_glBindVertexArray(g.vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    g.count = 0;
}

void znx_draw_end(void)
{
    flush();
    p_glBindVertexArray(0);
    p_glUseProgram(0);
}

static struct vertex* reserve(int n)
{
    if (g.count + n > g.capacity)
        flush();
    struct vertex* v = g.vertices + g.count;
    g.count += n;
    return v;
}

static void set_color(struct vertex* v, uint32_t color)
{
    v->r = (uint8_t)(color >> 24);
    v->g = (uint8_t)(color >> 16);
    v->b = (uint8_t)(color >> 8);
    v->a = (uint8_t)color;
}

/* Two triangles; corners given in the order top-left, top-right, bottom-right, bottom-left. */
static void quad(const float xy[4][2], const float uv[4][2], const float local[4][2], float hx, float hy,
                 float radius, float mode, uint32_t color)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    struct vertex* v = reserve(6);
    for (int i = 0; i < 6; ++i) {
        const int c = order[i];
        v[i].x = xy[c][0]; v[i].y = xy[c][1];
        v[i].u = uv[c][0]; v[i].v = uv[c][1];
        v[i].lx = local[c][0]; v[i].ly = local[c][1];
        v[i].hx = hx; v[i].hy = hy;
        v[i].radius = radius; v[i].mode = mode;
        set_color(&v[i], color);
    }
}

void znx_draw_rect(float x, float y, float w, float h, float radius, uint32_t color)
{
    /* One pixel of margin so the edge's antialiasing is not cut off. */
    const float m = 1.0f, hx = w / 2, hy = h / 2;
    const float xy[4][2] = { { x - m, y - m }, { x + w + m, y - m }, { x + w + m, y + h + m }, { x - m, y + h + m } };
    const float uv[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    const float local[4][2] = { { -hx - m, -hy - m }, { hx + m, -hy - m }, { hx + m, hy + m }, { -hx - m, hy + m } };
    if (radius > hx) radius = hx;
    if (radius > hy) radius = hy;
    quad(xy, uv, local, hx, hy, radius, 1.0f, color);
}

void znx_draw_ring(float x, float y, float w, float h, float radius, float thickness, uint32_t color)
{
    const float m = 1.0f, hx = w / 2, hy = h / 2;
    const float xy[4][2] = { { x - m, y - m }, { x + w + m, y - m }, { x + w + m, y + h + m }, { x - m, y + h + m } };
    const float uv[4][2] = { { thickness, 0 }, { thickness, 0 }, { thickness, 0 }, { thickness, 0 } };
    const float local[4][2] = { { -hx - m, -hy - m }, { hx + m, -hy - m }, { hx + m, hy + m }, { -hx - m, hy + m } };
    if (radius > hx) radius = hx;
    if (radius > hy) radius = hy;
    quad(xy, uv, local, hx, hy, radius, 2.0f, color);
}

static void atlas_quad(float x, float y, float w, float h, int ax, int ay, int aw, int ah, uint32_t color)
{
    const float u0 = (float)ax / ATLAS, v0 = (float)ay / ATLAS, u1 = (float)(ax + aw) / ATLAS, v1 = (float)(ay + ah) / ATLAS;
    const float xy[4][2] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    const float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    const float local[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    quad(xy, uv, local, 0, 0, 0, 0.0f, color);
}

void znx_draw_mark(float x, float y, float height, uint32_t color)
{
    /* The square covers 640 units for a 600-unit letter. */
    const float size_y = height * (MARK_Y1 - MARK_Y0) / 600.0f;
    const float size_x = size_y * (MARK_X1 - MARK_X0) / (MARK_Y1 - MARK_Y0);
    int pick = MARK_SIZES - 1;
    for (int i = MARK_SIZES - 1; i >= 0; --i)
        if (g.marks[i].size + 0.5f >= size_y) pick = i;
    /* The atlas square is square in units, so its pixels are drawn square too; at a raster's own size the
     * quad lands on whole pixels. */
    (void)size_x;
    {
        const float s = size_y + 0.5f >= g.marks[pick].size && size_y - 0.5f <= g.marks[pick].size ? g.marks[pick].size : size_y;
        const float left = floorf(x - s * (240.0f - MARK_X0) / (MARK_X1 - MARK_X0) + 0.5f);
        const float top = floorf(y - s * (200.0f - MARK_Y0) / (MARK_Y1 - MARK_Y0) + 0.5f);
        atlas_quad(left, top, s, s, g.marks[pick].x, g.marks[pick].y, g.marks[pick].size, g.marks[pick].size, color);
    }
}

static unsigned next_code(const char** text)
{
    const unsigned char* s = (const unsigned char*)*text;
    unsigned code;
    int extra;
    if (*s < 0x80) { code = *s; extra = 0; }
    else if ((*s & 0xE0) == 0xC0) { code = *s & 0x1F; extra = 1; }
    else if ((*s & 0xF0) == 0xE0) { code = *s & 0x0F; extra = 2; }
    else { code = *s & 0x07; extra = 3; }
    s++;
    while (extra-- > 0 && (*s & 0xC0) == 0x80)
        code = (code << 6) | (*s++ & 0x3F);
    *text = (const char*)s;
    return code;
}

static int tabular_char(unsigned code)
{
    return code >= '0' && code <= '9';
}

static const struct glyph* find_glyph(const struct font* font, unsigned code)
{
    int index;
    if (code >= SYM_FIRST && code <= SYM_LAST)
        return font->symbols[code - SYM_FIRST].present ? &font->symbols[code - SYM_FIRST] : NULL;
    index = glyph_index(code);
    if (index >= 0 && !font->glyphs[index].present) {
        /* a font without this punctuation: the nearest plain character, so nothing vanishes from a sentence */
        static const unsigned stand_in[][2] = {
            { 0x2212, 0x2013 }, { 0x2013, '-' }, { 0x2014, '-' }, { 0x00A0, ' ' }, { 0x2019, '\'' }, { 0x00D7, 'x' },
            { 0x00B7, '-' }, { 0x2192, '>' }, { 0x2190, '<' }, { 0x2026, '.' },
        };
        for (unsigned i = 0; i < sizeof(stand_in) / sizeof(stand_in[0]); ++i)
            if (stand_in[i][0] == code)
                return find_glyph(font, stand_in[i][1]);
    }
    return index >= 0 ? &font->glyphs[index] : NULL;
}

static float advance(const struct font* font, unsigned code, int flags)
{
    const struct glyph* gl = find_glyph(font, code);
    if (gl == NULL)
        return 0;
    if ((flags & ZNX_TEXT_TABULAR) && tabular_char(code))
        return font->tabular_advance;
    return gl->advance;
}

int znx_draw_has_symbol(unsigned code)
{
    return g.ready && code >= SYM_FIRST && code <= SYM_LAST && g.fonts[ZNX_FONT_ROW].symbols[code - SYM_FIRST].present;
}

float znx_text_width(int font_id, const char* text, int flags)
{
    const struct font* font = &g.fonts[font_id];
    float width = 0;
    while (*text)
        width += advance(font, next_code(&text), flags);
    return width;
}

float znx_draw_text(int font_id, float x, float y, const char* text, uint32_t color, int flags)
{
    const struct font* font = &g.fonts[font_id];
    const float width = znx_text_width(font_id, text, flags);
    float pen;
    if (flags & ZNX_TEXT_RIGHT) x -= width;
    else if (flags & ZNX_TEXT_CENTER) x -= width / 2;
    pen = x;
    y = floorf(y + 0.5f);
    while (*text) {
        unsigned code = next_code(&text);
        const struct glyph* gl = find_glyph(font, code);
        if (gl == NULL)
            continue;
        float step = advance(font, code, flags);
        float offset = (flags & ZNX_TEXT_TABULAR) && tabular_char(code) ? (step - gl->advance) / 2 : 0;
        if (gl->present && gl->w > 0)
            atlas_quad(floorf(pen + offset + 0.5f) + gl->left, y - gl->top, gl->w, gl->h, gl->x, gl->y, gl->w, gl->h, color);
        pen += step;
    }
    return width;
}

float znx_line_height(int font) { return g.fonts[font].line_height; }
float znx_cap_height(int font) { return g.fonts[font].cap_height; }

int znx_draw_wrapped(int font, float x, float y, float width, const char* text, uint32_t color)
{
    char line[512];
    int lines = 0;
    const float step = g.fonts[font].line_height;
    while (*text) {
        /* The longest run of whole words that fits. */
        size_t best = 0, length = 0;
        const char* p = text;
        while (*p == ' ') p++;
        text = p;
        for (;;) {
            size_t end = length;
            while (text[end] && text[end] != ' ' && text[end] != '\n') end++;
            if (end >= sizeof(line)) break;
            memcpy(line, text, end);
            line[end] = '\0';
            if (znx_text_width(font, line, 0) > width && best > 0) break;
            best = end;
            if (text[end] != ' ') break;
            length = end + 1;
        }
        if (best == 0) best = strcspn(text, " \n");
        if (best >= sizeof(line)) best = sizeof(line) - 1;
        memcpy(line, text, best);
        line[best] = '\0';
        /* a single word wider than the line (a path, a file name) is broken where it stops fitting */
        while (best > 1 && strchr(line, ' ') == NULL && znx_text_width(font, line, 0) > width) {
            do best--; while (best > 1 && ((unsigned char)text[best] & 0xC0) == 0x80);
            line[best] = '\0';
        }
        if (color != 0)
            znx_draw_text(font, x, y + lines * step, line, color, 0);
        lines++;
        text += best;
        if (*text == '\n') text++;
    }
    return lines;
}

void znx_text_fit(int font, const char* text, float width, char* out, size_t out_size)
{
    znx_text_fit_flags(font, text, width, out, out_size, 0);
}

void znx_text_fit_flags(int font, const char* text, float width, char* out, size_t out_size, int flags)
{
    size_t length = strlen(text);
    snprintf(out, out_size, "%s", text);
    if (znx_text_width(font, out, flags) <= width)
        return;
    /* Keep the start and the end (the end of a file name says what kind of file it is). */
    for (size_t cut = 1; cut < length; ++cut) {
        size_t keep = length - cut, head = (keep + 1) / 2, tail = keep - head;
        if (head + tail + 4 > out_size) continue;
        if ((text[head] & 0xC0) == 0x80 || (tail > 0 && (text[length - tail] & 0xC0) == 0x80)) continue;
        memcpy(out, text, head);
        memcpy(out + head, "\xE2\x80\xA6", 3);
        memcpy(out + head + 3, text + length - tail, tail);
        out[head + 3 + tail] = '\0';
        if (znx_text_width(font, out, flags) <= width)
            return;
    }
}

/* ---- controller buttons in text ---- */

/* The Nintendo Extended font's private-use code points, as the open Switch libraries use them (Borealis's
 * hints, nx-hbmenu): the filled set from U+E0E0. debug_glyphs=1 shows them all for checking on a console. */
static const struct { const char* key; unsigned codes[2]; const char* letter; } l_buttons[] = {
    { "A", { 0xE0E0, 0 }, "A" }, { "B", { 0xE0E1, 0 }, "B" }, { "X", { 0xE0E2, 0 }, "X" }, { "Y", { 0xE0E3, 0 }, "Y" },
    { "L", { 0xE0E4, 0 }, "L" }, { "R", { 0xE0E5, 0 }, "R" }, { "ZL", { 0xE0E6, 0 }, "ZL" }, { "ZR", { 0xE0E7, 0 }, "ZR" },
    { "+", { 0xE0EF, 0 }, "+" }, { "-", { 0xE0F0, 0 }, "\xE2\x88\x92" },
    { "LR", { 0xE0ED, 0xE0EE }, NULL }, { "DPAD", { 0xE0EA, 0 }, NULL }, { "LS", { 0xE0C1, 0 }, "L" },
};

static int find_button(const char* key)
{
    for (int i = 0; i < (int)(sizeof(l_buttons) / sizeof(l_buttons[0])); ++i)
        if (strcmp(l_buttons[i].key, key) == 0)
            return i;
    return -1;
}

int znx_button_codes(const char* key, unsigned codes[2])
{
    const int i = find_button(key);
    codes[0] = codes[1] = 0;
    if (i < 0)
        return 0;
    codes[0] = l_buttons[i].codes[0];
    codes[1] = l_buttons[i].codes[1];
    return codes[1] ? 2 : 1;
}

static void put_code(char* out, unsigned code)
{
    out[0] = (char)(0xE0 | (code >> 12));
    out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[2] = (char)(0x80 | (code & 0x3F));
    out[3] = '\0';
}

float znx_draw_button(int font, float x, float baseline, const char* key, uint32_t color, uint32_t knock, int draw)
{
    const int i = find_button(key);
    const float cap = g.fonts[font].cap_height;
    const float d = floorf(cap * 1.5f + 0.5f);          /* a 22-pixel line: a 24-pixel disc */
    const float cy = floorf(baseline - cap / 2 + 0.5f);
    if (i >= 0) {
        const struct font* f = &g.fonts[font];
        const unsigned a = l_buttons[i].codes[0], b = l_buttons[i].codes[1];
        if (find_glyph(f, a) != NULL && (b == 0 || find_glyph(f, b) != NULL)) {
            char text[8];
            float w;
            put_code(text, a);
            if (b != 0) put_code(text + 3, b);
            w = znx_text_width(font, text, 0);
            if (draw) znx_draw_text(font, x, baseline, text, color, 0);
            return w;
        }
    }
    if (i >= 0 && l_buttons[i].letter == NULL) {
        /* the D-pad: a cross; for "LR" the up and down arms are quieter */
        const float arm = floorf(d / 3 + 0.5f), cx = x + d / 2;
        if (draw) {
            const uint32_t quiet = strcmp(key, "LR") == 0 ? (color & 0xFFFFFF00u) | 0x66u : color;
            znx_draw_rect(cx - arm / 2, cy - d / 2, arm, d, 2, quiet);
            znx_draw_rect(x, cy - arm / 2, d, arm, 2, color);
        }
        return d;
    }
    {
        /* a disc with the letter; two letters make it a pill */
        const char* letter = i >= 0 ? l_buttons[i].letter : key;
        const float tw = znx_text_width(ZNX_FONT_SMALL, letter, 0);
        const float w = tw + 8 > d ? floorf(tw + 12.5f) : d;
        if (draw) {
            znx_draw_rect(x, cy - d / 2, w, d, d / 2, color);
            znx_draw_text(ZNX_FONT_SMALL, x + w / 2, cy + g.fonts[ZNX_FONT_SMALL].cap_height / 2, letter, knock, ZNX_TEXT_CENTER);
        }
        return w;
    }
}

/* ---- the theme ---- */

/* The colours of the console's two themes as the open Switch libraries approximate them (Borealis's theme
 * values, MIT; see THIRD_PARTY_NOTICES.md). */
static const struct znx_theme l_themes[2] = {
    {   /* Basic White */
        0xEBEBEBFFu, 0xF0F0F0FFu, 0xFCFFF8FFu,
        0x2D2D2DFFu, 0x828282FFu, 0xA8A8A8FFu,
        0x2D2D2DFFu, 0xCFCFCFFFu,
        0x3250F0FFu,
        0x0DB6D5FFu, 0x50EFD9FFu,
        0x1E9E5AFFu, 0xD9412FFFu,
    },
    {   /* Basic Black */
        0x2D2D2DFFu, 0x323232FFu, 0x1F2227FFu,
        0xFFFFFFFFu, 0xA3A3A3FFu, 0x787878FFu,
        0xFFFFFFFFu, 0x4E4E4EFFu,
        0x00FFCCFFu,
        0x198AC6FFu, 0x89F1F2FFu,
        0x6ED478FFu, 0xFF6A5AFFu,
    },
};
static int l_dark;
static float l_freeze = -1.0f;

void znx_theme_set(int dark) { l_dark = dark != 0; }
int znx_theme_is_dark(void) { return l_dark; }
const struct znx_theme* znx_theme(void) { return &l_themes[l_dark]; }
void znx_theme_freeze(float phase) { l_freeze = phase; }

uint32_t znx_theme_highlight(void)
{
    const struct znx_theme* t = &l_themes[l_dark];
    /* a sine between the two colours, one cycle in 72 frames */
    const float k = l_freeze >= 0 ? l_freeze : 0.5f - 0.5f * cosf((float)(l_frames % 72) / 72.0f * 6.2831853f);
    uint32_t out = 0;
    for (int shift = 24; shift >= 8; shift -= 8) {
        const float a = (float)((t->highlight_a >> shift) & 255), b = (float)((t->highlight_b >> shift) & 255);
        out |= (uint32_t)(a + (b - a) * k + 0.5f) << shift;
    }
    return out | 0xFFu;
}
