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
    int frames;                /* frames (vertical interrupts) to run */
    int hash_every;            /* "hash" line every K frames, 0 = never */
    int emumode;               /* 2 recompiler in a rollback session; 0/1 interpreters */
    int verbose;               /* also pass the core's info and verbose messages on */
    int rb_a, rb_b;            /* rollback test: save at A, rewind at B; -1 = none */
    long watch;                /* RDRAM offset of a byte to report, or -1 */
    int dump_frame;            /* write RDRAM to dump_path after this many frames, or -1 */
    const char* dump_path;
    int progress_every;        /* "progress" line every K frames, 0 = never */
    struct znx_window windows[ZNX_MAX_WINDOWS];
    int window_count;
};

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
};

void znx_options_defaults(struct znx_options* options);

/* Runs the whole measurement on the calling thread. Returns 0 when every frame ran and
 * the rollback test (if any) passed. */
int znx_bench_run(const struct znx_host* host, const struct znx_options* options);

/* For a watchdog on another thread. znx_bench_stall_ns: how long the frame (or replayed
 * frame) in flight has been running, 0 outside the frame loop. znx_bench_frame: frames
 * completed. znx_bench_stop: ends the run at the next frame boundary. */
uint64_t znx_bench_stall_ns(void);
int znx_bench_frame(void);
void znx_bench_stop(void);

uint64_t znx_now_ns(void);

#endif
