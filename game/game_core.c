// SPDX-License-Identifier: GPL-3.0-or-later
/* See game_core.h. */

#include "game_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct znx_game_settings l_settings;
static struct znx_game_video l_video;
static void (*l_print)(int is_error, const char* line);

/* Since the last report. */
static int l_count, l_no_swap, l_late;
static uint64_t l_emu_ns, l_swap_ns, l_frame_ns;
static uint64_t l_emu_worst, l_swap_worst, l_frame_worst;
static int l_emu_worst_frame, l_last_swaps;
static uint64_t l_cpu_last, l_cpu_ns, l_cpu_worst;
static int l_cpu_late, l_cpu_worst_frame;

static void out(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    l_print(0, line);
}

void znx_game_begin(const struct znx_game_settings* settings, const struct znx_game_video* video,
                    void (*print)(int is_error, const char* line))
{
    l_settings = *settings;
    l_video = *video;
    l_print = print;
    l_count = l_no_swap = l_late = 0;
    l_emu_ns = l_swap_ns = l_frame_ns = 0;
    l_emu_worst = l_swap_worst = l_frame_worst = 0;
    l_emu_worst_frame = 0;
}

/* GLideN64's settings, written into the core's configuration after the plugin has declared
 * them with its defaults (mupenplus/Config_mupenplus.cpp, Config_SetDefault) and before it
 * reads them (Config_LoadConfig, at RomOpen).
 *
 * RMG-K does not force any of this for a netplay session. It builds GLideN64 with its Qt
 * settings dialog, which keeps them in GLideN64.ini; a fresh install has no such file and
 * gets Config::resetToDefaults (Config.cpp): frame buffer emulation OFF. With it off the
 * plugin never writes into RDRAM for this game (FrameBufferList::saveBuffer returns before
 * there is a current buffer, FrameBuffer.cpp:793, and everything that copies to RDRAM hangs
 * off that buffer). So the picture cannot change the emulated machine, here or on the PC,
 * unless the PC player turned frame buffer emulation on.
 *
 * Every option that can put data into RDRAM is switched off explicitly all the same. */
struct setting { const char* section; const char* name; int is_bool; int value; };

int znx_game_video_config(const struct znx_config* config)
{
    const int fb = l_settings.scale > 0;
    const struct setting settings[] = {
        { "Video-General",  "Fullscreen",                       1, 0 },
        { "Video-General",  "ScreenWidth",                      0, l_settings.width },
        { "Video-General",  "ScreenHeight",                     0, l_settings.height },
        { "Video-General",  "VerticalSync",                     1, l_settings.vsync },
        /* All OpenGL calls on the emulation thread. */
        { "Video-GLideN64", "ThreadedVideo",                    1, 0 },
        { "Video-GLideN64", "MultiSampling",                    0, 0 },
        { "Video-GLideN64", "FXAA",                             1, 0 },
        { "Video-GLideN64", "AspectRatio",                      0, 1 },   /* 4:3, bars left and right */
        { "Video-GLideN64", "BufferSwapMode",                   0, 0 },   /* swap on the vertical interrupt */
        { "Video-GLideN64", "UseNativeResolutionFactor",        0, fb ? l_settings.scale : 0 },
        { "Video-GLideN64", "bilinearMode",                     1, 1 },   /* the plugin's default filter */
        { "Video-GLideN64", "anisotropy",                       0, 0 },
        /* What writes into RDRAM, or reads the picture back for it. */
        { "Video-GLideN64", "EnableFBEmulation",                1, fb },
        { "Video-GLideN64", "EnableCopyColorToRDRAM",           0, 0 },
        { "Video-GLideN64", "EnableCopyDepthToRDRAM",           0, 0 },
        { "Video-GLideN64", "EnableCopyAuxiliaryToRDRAM",       1, 0 },
        { "Video-GLideN64", "EnableCopyColorFromRDRAM",         1, 0 },
        { "Video-GLideN64", "EnableCopyDepthToMainDepthBuffer", 1, 0 },
        { "Video-GLideN64", "EnableN64DepthCompare",            0, 0 },
        { "Video-GLideN64", "ForceDepthBufferClear",            1, 0 },
        { "Video-GLideN64", "DisableFBInfo",                    1, 1 },
        /* No per-game overrides (GLideN64.custom.ini has none for this game and is not shipped). */
        { "Video-GLideN64", "EnableCustomSettings",             1, 0 },
        /* No files: shader cache, texture packs, texture cache, dumps. */
        { "Video-GLideN64", "EnableShadersStorage",             1, 0 },
        { "Video-GLideN64", "txFilterMode",                     0, 0 },
        { "Video-GLideN64", "txEnhancementMode",                0, 0 },
        { "Video-GLideN64", "txHiresEnable",                    1, 0 },
        { "Video-GLideN64", "txSaveCache",                      1, 0 },
        { "Video-GLideN64", "txDump",                           1, 0 },
        { "Video-GLideN64", "ShowFPS",                          1, 0 },
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        const struct setting* s = &settings[i];
        if ((s->is_bool ? config->set_bool : config->set_int)(s->section, s->name, s->value) != 0)
            return 1;
    }
    out("video settings: %dx%d, vsync %s, %s; nothing is copied to or from RDRAM",
        l_settings.width, l_settings.height, l_settings.vsync ? "on" : "off",
        fb ? "frame buffer emulation ON (the plugin clears buffers in RDRAM itself)"
           : "frame buffer emulation off (GLideN64 never writes RDRAM)");
    if (fb)
        out("video settings: drawing at %dx the N64's resolution", l_settings.scale);
    return 0;
}

int znx_game_last_frame_swaps(void) { return l_last_swaps; }

void znx_game_frame_done(int frames, uint64_t frame_ns)
{
    /* The swap happens inside the frame (the plugin swaps on the vertical interrupt), so the
     * frame's own work is what is left: core, RSP, GLideN64 and the GL driver's CPU side. */
    uint64_t swap_ns = l_video.take_swap_ns != NULL ? l_video.take_swap_ns() : 0;
    uint64_t emu_ns = frame_ns > swap_ns ? frame_ns - swap_ns : 0;
    l_last_swaps = l_video.take_swaps != NULL ? l_video.take_swaps() : 0;
    if (l_video.thread_cpu_ns != NULL) {
        const uint64_t now = l_video.thread_cpu_ns();
        const uint64_t cpu = l_cpu_last != 0 && now > l_cpu_last ? now - l_cpu_last : 0;
        l_cpu_last = now;
        l_cpu_ns += cpu;
        if (cpu > 16666667ull) l_cpu_late++;
        if (cpu > l_cpu_worst) { l_cpu_worst = cpu; l_cpu_worst_frame = frames - 1; }
    }

    l_count++;
    if (l_last_swaps == 0)
        l_no_swap++;
    if (emu_ns > 16666667ull)
        l_late++;
    l_emu_ns += emu_ns;
    l_swap_ns += swap_ns;
    l_frame_ns += frame_ns;
    if (emu_ns > l_emu_worst) {
        l_emu_worst = emu_ns;
        l_emu_worst_frame = frames - 1;
    }
    if (swap_ns > l_swap_worst)
        l_swap_worst = swap_ns;
    if (frame_ns > l_frame_worst)
        l_frame_worst = frame_ns;

    if (l_settings.report_every > 0 && frames % l_settings.report_every == 0) {
        out("shown %d: emulation %.2f ms/frame (worst %.2f at frame %d, %d over 16.7), "
            "swap wait %.2f ms (worst %.2f), whole frame %.2f ms (worst %.2f), %.1f fps, %d of %d frames without a swap",
            frames, l_emu_ns / 1e6 / l_count, l_emu_worst / 1e6, l_emu_worst_frame, l_late,
            l_swap_ns / 1e6 / l_count, l_swap_worst / 1e6,
            l_frame_ns / 1e6 / l_count, l_frame_worst / 1e6,
            l_frame_ns > 0 ? l_count * 1e9 / l_frame_ns : 0.0, l_no_swap, l_count);
        if (l_video.thread_cpu_ns != NULL)
            out("shown %d: the emulation thread ran %.2f ms/frame (worst %.2f at frame %d, %d frames over 16.7)",
                frames, l_cpu_ns / 1e6 / l_count, l_cpu_worst / 1e6, l_cpu_worst_frame, l_cpu_late);
        l_count = l_no_swap = l_late = 0;
        l_emu_ns = l_swap_ns = l_frame_ns = 0;
        l_emu_worst = l_swap_worst = l_frame_worst = 0;
        l_cpu_ns = l_cpu_worst = 0; l_cpu_late = 0;
    }
}
