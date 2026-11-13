// SPDX-License-Identifier: GPL-3.0-or-later
/* The part of znx-bench that is the same on every platform: core set-up with RMG-K's
 * forced rollback settings, the frame loop, RDRAM hashes, the rollback test and the
 * timing report. The platform (bench/znx-bench.c on the Mac, switch/main_switch.c on the
 * Switch) finds the libraries, the ROM and the options, and says where lines go. */
#ifndef ZNX_BENCH_CORE_H
#define ZNX_BENCH_CORE_H

#include <stdint.h>

#define ZNX_MAX_WINDOWS 4

struct znx_window {
    const char* name; /* printed in the "window" line */
    int from, to;     /* frames from <= f < to */
};

struct znx_options {
    const char* rom_path;
    const char* inputs_path;   /* or NULL */
    int frames;                /* frames (vertical interrupts) to run; 0 = until znx_bench_stop() */
    int hash_every;            /* "hash" line every K frames, 0 = never */
    int emumode;               /* 2 recompiler in a rollback session; 0/1 interpreters */
    int verbose;               /* also pass the core's info and verbose messages on */
    int rb_a, rb_b;            /* rollback test: save at A, rewind at B; -1 = none */
    int check_every;           /* "check" line every K frames: fast checksums of RDRAM, whole and without */
    long check_skip_lo, check_skip_hi;   /* the RDRAM range [lo, hi) (multiples of 32), for comparing builds */
    int stress_every;          /* rollback stress: every K frames rewind the last D and re-run them */
    int stress_depth;          /*   (0 = off); each re-run is compared frame by frame with the first pass */
    long watch;                /* RDRAM offset of a byte to report, or -1 */
    int dump_frame;            /* write RDRAM to dump_path after this many frames, or -1 */
    const char* dump_path;
    int progress_every;        /* "progress" line every K frames, 0 = never */
    struct znx_window windows[ZNX_MAX_WINDOWS];
    int window_count;
};

struct znx_config;

struct znx_host {
    /* Library handles in the form CoreAttachPlugin wants them, and the way to look a
     * function up in one: dlopen/dlsym on the Mac, the static tables on the Switch. */
    void* core;
    void* video;
    void* rsp;
    void* (*sym)(void* lib, const char* name);
    /* One finished line without its newline; is_error: diagnostics, not results. */
    void (*print)(int is_error, const char* line);
    const char* data_dir;      /* holds mupen64plus.ini, the core's ROM database */
    const char* user_dir;      /* scratch profile; must exist */

    /* The rest is for a program that shows a picture (game/); znx-bench leaves it zero. */
    const char* name;          /* prefix of diagnostics, "znx-bench" when NULL */
    /* An m64p_video_extension_functions table for CoreOverrideVidExt: how the video plugin
     * gets its window and OpenGL context from this program instead of from SDL. */
    void* vidext;
    /* Called after the video plugin's PluginStartup (its settings exist with their defaults)
     * and before it is attached: write the plugin's settings. Non-zero stops the run. */
    int (*video_config)(const struct znx_config* config);
    /* Called on the emulation thread after every completed frame with the number of frames
     * completed and the time the frame took (begin_frame to end_frame), before hashing. */
    void (*frame_done)(int frames, uint64_t frame_ns);
    /* An audio plugin's handle, or NULL for the core's silent dummy. */
    void* audio;
    /* Live controllers: called on the emulation thread once per frame when no input script is
     * loaded; fills the four players' N64 BUTTONS words. NULL: no input. */
    void (*live_inputs)(uint32_t buttons[4]);
};

/* The core's configuration, for znx_host.video_config. Each returns 0 on success. */
struct znx_config {
    int (*set_int)(const char* section, const char* name, int value);
    int (*set_bool)(const char* section, const char* name, int value);
    int (*set_string)(const char* section, const char* name, const char* value);
};

void znx_options_defaults(struct znx_options* options);

/* Runs the whole measurement on the calling thread. Returns 0 when every frame ran (or, with
 * frames = 0, when the run was stopped) and the rollback test (if any) passed. */
int znx_bench_run(const struct znx_host* host, const struct znx_options* options);

/* For a watchdog on another thread. znx_bench_stall_ns: how long the frame (or replayed
 * frame) in flight has been running, 0 outside the frame loop. znx_bench_frame: frames
 * completed. znx_bench_stop: ends the run at the next frame boundary. */
uint64_t znx_bench_stall_ns(void);
int znx_bench_frame(void);
void znx_bench_stop(void);

uint64_t znx_now_ns(void);

#endif
