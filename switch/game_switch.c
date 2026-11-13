// SPDX-License-Identifier: GPL-3.0-or-later
/* Z-Cancel NX for the Nintendo Switch, step 3a of docs/PLAN.md: the picture.
 *
 * The same core, RSP plugin, forced netplay settings and frame driver as znx-bench
 * (bench/bench_core.c), with RMG-K's GLideN64 in place of the null video plugin. GLideN64
 * gets the screen through the core's video extension (vidext_switch.c: EGL, desktop OpenGL)
 * and its settings from game/game_core.c. The buffer swap waits for the display, which
 * keeps the game at 60 Hz; the core's own speed limiter is not used. There is no sound and
 * no controller input yet: input comes from a script, or there is none and the game shows
 * its intro and a demo fight.
 *
 * Files, all under sdmc:/switch/z-cancel-nx/:
 *   rom.z64          the ROM (or pass a path as the NRO's first argument)
 *   inputs.txt       optional input script, the format of bench/inputs/ (frame player buttons_hex)
 *   z-cancel-nx.ini  optional:
 *                      scale=0        0 draws at the screen's 1280x720 (default); 1 or 2 turns
 *                                     GLideN64's frame buffer emulation on at that multiple of
 *                                     the N64's resolution
 *                      rdram_check=1  print "hash <frame> <fnv>" every 600 frames, to compare
 *                                     with znx-bench (3600 -> 1499ebb25c8606c4 without input)
 *                      vsync=1        0 runs as fast as it can
 *                      frames=0       stop after this many frames; 0 runs until Plus
 *                      inputs=<file>  another script; "inputs=" for none
 *                      cpu_mhz=1785   0 leaves the clock alone
 *                      verbose=1      the core's and the plugin's own messages too
 *   latest.log       every line of output, flushed line by line
 *
 * Lines also go to a PC running nxlink (-s). Plus on any controller leaves. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "bench_core.h"
#include "front_switch.h"
#include "game_core.h"
#include "input_switch.h"
#include "vidext_switch.h"
#include "znx_switch.h"

#define PROGRAM "z-cancel-nx"

struct settings {
    struct znx_options options;
    struct znx_game_settings game;
    char inputs_path[512];
    bool inputs_set;
    int rdram_check;
    uint32_t cpu_mhz;
    int sound;                 /* 1: the audio plugin; 0: silent */
    int stick_range;           /* what a fully tilted stick reads on the N64 axis; 0 = 84 */
};

static bool ini_key(const char* key, const char* value, void* user)
{
    struct settings* s = user;
    if (!strcmp(key, "scale")) s->game.scale = atoi(value);
    else if (!strcmp(key, "rdram_check")) s->rdram_check = atoi(value);
    else if (!strcmp(key, "vsync")) s->game.vsync = atoi(value) != 0;
    else if (!strcmp(key, "frames")) s->options.frames = atoi(value);
    else if (!strcmp(key, "inputs")) {
        s->inputs_set = true;
        if (*value == '\0') s->inputs_path[0] = '\0';
        else if (strchr(value, ':') != NULL) snprintf(s->inputs_path, sizeof(s->inputs_path), "%s", value);
        else snprintf(s->inputs_path, sizeof(s->inputs_path), ZNX_BASE_DIR "/%s", value);
    }
    else if (!strcmp(key, "cpu_mhz")) s->cpu_mhz = (uint32_t)atoi(value);
    else if (!strcmp(key, "sound")) s->sound = atoi(value) != 0;
    else if (!strcmp(key, "stick_range")) s->stick_range = atoi(value);
    else if (!strcmp(key, "verbose")) s->options.verbose = atoi(value);
    else return false;
    return true;
}

/* On the emulation thread, after every frame. */
static void frame_done(int frames, uint64_t frame_ns)
{
    znx_game_frame_done(frames, frame_ns);
    if (frames % 600 == 0) {
        /* The audio plugin's own counters, through the same table its API is looked up in. */
        static void (*stats)(unsigned*, unsigned*, unsigned*);
        static bool looked;
        if (!looked) {
            void* audio = znx_static_lib("audio");
            stats = audio != NULL ? (void (*)(unsigned*, unsigned*, unsigned*))osal_dynlib_getproc(audio, "znx_audio_stats") : NULL;
            looked = true;
        }
        if (stats != NULL) {
            unsigned underruns = 0, overruns = 0, fill = 0;
            stats(&underruns, &overruns, &fill);
            znx_log("sound %d: %u ms waiting, ran dry %u times and dropped %u blocks so far", frames, fill, underruns, overruns);
        }
    }
    /* The plugin swaps when the game has drawn something. A frame without a swap (a scene
     * loading) would otherwise not wait for the display at all. */
    if (znx_game_last_frame_swaps() == 0)
        znx_vidext_pace_without_swap();
}

/* Running time of the calling thread on any core (13.0.0+), in ns: system ticks are 19.2 MHz. */
static uint64_t thread_cpu_ns(void)
{
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, (u64)-1)))
        return 0;
    return armTicksToNs(ticks);
}

/* The four controllers for this frame. Holding the quit combination (input_switch.h) ends the run. */
static void live_inputs(uint32_t buttons[4])
{
    static bool quitting;
    znx_input_poll();
    for (int p = 0; p < 4; ++p)
        buttons[p] = znx_input_buttons(p);
    if (!quitting && znx_input_quit_requested()) {
        quitting = true;
        znx_log(PROGRAM ": quit combination held at frame %d", znx_bench_frame());
        znx_bench_stop();
    }
}

int main(int argc, char** argv)
{
    static struct settings settings;
    static struct znx_host host;
    static const struct znx_game_video video = { znx_vidext_take_swap_ns, znx_vidext_take_swaps, thread_cpu_ns };
    bool have_romfs;
    int exit_code = 1;

    znx_front_begin(PROGRAM, "latest.log", false);
    have_romfs = znx_front_romfs();

    znx_options_defaults(&settings.options);
    settings.options.frames = 0;          /* until Plus */
    settings.options.hash_every = 0;
    settings.options.progress_every = 0;
    settings.options.window_count = 0;
    settings.game.width = 1280;
    settings.game.height = 720;
    settings.game.vsync = 1;
    settings.game.scale = 0;
    settings.game.report_every = 600;
    settings.cpu_mhz = 1785; /* the highest stock CPU clock of the first Switch model */
    settings.sound = 1;
    znx_front_read_ini(PROGRAM ".ini", ini_key, &settings);
    if (settings.options.frames < 0)
        settings.options.frames = 0;
    if (settings.game.scale < 0 || settings.game.scale > 4)
        settings.game.scale = 0;
    if (settings.rdram_check)
        settings.options.hash_every = 600;
    if (!settings.inputs_set && znx_file_exists(ZNX_BASE_DIR "/inputs.txt"))
        snprintf(settings.inputs_path, sizeof(settings.inputs_path), ZNX_BASE_DIR "/inputs.txt");
    settings.options.inputs_path = settings.inputs_path[0] ? settings.inputs_path : NULL;
    znx_log("settings: scale %d, vsync %d, rdram_check %d, frames %d, inputs %s, cpu_mhz %u",
            settings.game.scale, settings.game.vsync, settings.rdram_check, settings.options.frames,
            settings.options.inputs_path ? settings.options.inputs_path : "none", (unsigned)settings.cpu_mhz);
    settings.options.rom_path = znx_front_find_rom(argc, argv);

    znx_front_power_begin(settings.cpu_mhz);
    znx_game_begin(&settings.game, &video, znx_front_print);

    host.name = PROGRAM;
    host.core = znx_static_lib("core");
    host.video = znx_static_lib("gliden64");
    host.rsp = znx_static_lib("rsp_hle");
    host.sym = osal_dynlib_getproc;
    host.print = znx_front_print;
    host.data_dir = "romfs:/data";
    host.user_dir = ZNX_BASE_DIR "/profile";
    host.vidext = znx_vidext_functions();
    host.video_config = znx_game_video_config;
    host.frame_done = frame_done;
    host.audio = settings.sound ? znx_static_lib("audio") : NULL;
    if (settings.options.inputs_path == NULL) {
        /* Played with controllers: Plus is the N64's Start, so it no longer stops the run. */
        znx_input_init(settings.stick_range);
        host.live_inputs = live_inputs;
        znx_front_plus_stops(false);
        znx_log("input: controllers (hold Minus, or L + R + Z + Start on a GameCube controller, to quit); full stick tilt reads %d",
                settings.stick_range > 0 ? settings.stick_range : 84);
    }

    if (settings.options.rom_path == NULL)
        ;
    else if (host.core == NULL || host.video == NULL || host.rsp == NULL || !have_romfs)
        znx_log(PROGRAM ": this build is incomplete (a linked library or the romfs is missing)");
    else
        exit_code = znx_front_run(&host, &settings.options, NULL);

    /* Without a PC listening, a run that ended by itself (an error, or frames=N) leaves its
     * last lines on the screen; Plus during the picture goes straight back to the menu. */
    znx_front_end(exit_code, exit_code != 0 || settings.options.frames > 0);
    return 0;
}
