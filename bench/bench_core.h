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
    int stress_depth;          /*   (0 = off); each re-run is compared frame by frame with the first pass. The state
                                *   after every frame, re-run ones too, is saved into a ring of slots, as the session does */
    int rollback_verify;       /* the core checks every rollback save and load against a whole one, and that no RDRAM
                                * page changed without being noticed (patches/0014; slow) */
    long watch;                /* RDRAM offset of a byte to report, or -1 */
    int dump_frame;            /* write RDRAM to dump_path after this many frames, or -1 */
    const char* dump_path;
    int progress_every;        /* "progress" line every K frames, 0 = never */
    /* A forced VS match (docs/STAGE_MATRIX.md): START is pressed at frames 200 and 420, and on the frame the
     * scene byte first asks for the mode select the battle setup is written into RDRAM and the scene changed
     * to the VS battle. Same addresses in vanilla, Smash Remix 2.0.1 and +EXTRA 0.6.0. vs_stage < 0: off. */
    int vs_stage;              /* gkind / Remix stage id */
    int vs_fighters[4];        /* fighter kind per port, -1 = empty; level 9 CPUs */
    int vs_level;              /* CPU level 1..9, 0 = 9 */
    int input_players;         /* controller ports the rollback session synchronizes (1-4); 0 = 4. The
                                * rest read as unplugged (pif.c). RMG-K's P2P sessions use 2. */
    /* The recompiler's warm list (docs/PLAN.md, "Compiling ahead of need"): the blocks compiled in earlier
     * runs of this ROM, compiled again in the idle time after each frame before the game needs them. */
    int precompile_ms;         /* a frame's work plus the compiling after it stays under this; 0 = no compiling
                                * ahead (the list is still recorded and saved) */
    int precompile_big;        /* 1 (default): a block too large for any frame's spare time is compiled ahead
                                * all the same, alone, after a light frame: one late frame when its code has
                                * just been loaded instead of one when the game first runs it; 0: such blocks
                                * are left to be compiled on demand. A netplay session never does it. */
    int warm_off;              /* 1: no list at all: nothing loaded, nothing saved */
    const char* warm_path;     /* the list file to load and save, or NULL: warm-<ROM MD5>.bin in the user
                                * directory, with the shipped one (znx_host.warm_dir) merged in */
    int warm_flags;            /* DebugDynarecWarmConfig: 1 verify mode, 2 hash the generated code */
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

    /* Shipped warm lists (warm-<ROM MD5>.bin), or NULL: romfs:/data/warm on the Switch. */
    const char* warm_dir;
    /* The work since the frame before last ended, when the frame's time is not it (part of it is
     * waiting for the display in a buffer swap): the emulation thread's running time. It includes
     * the compiling ahead done after the last frame, which bench_core takes off. Called after
     * frame_done. NULL, or a result of 0: the frame's time. */
    uint64_t (*frame_work_ns)(void);

    /* For a netplay session (net/znx-net.cpp); everything else leaves these zero. */
    /* An input plugin's handle, or NULL for the core's dummy (controller 1 present, no input). */
    void* input;
    /* Replaces the frame loop: called once the core, the ROM, the plugins and the saves are set
     * up as for any session, it runs the session (M64CMD_ROLLBACK_EXECUTE through its own
     * callbacks) and returns 0 on success. The bench's own frame callbacks, hash/check lines
     * and summary are then not used. */
    int (*session_run)(void);
    /* With session_run: called once the ROM is open and the plugins are attached, in place of the blank
     * save data, to put the match's cheats and save data into the core (a lobby match's pre-match
     * manifest, net/lobby_play.h). Non-zero stops the run. NULL: blank saves, no cheats. */
    int (*session_prepare)(void* core);

    /* Non-zero when the attached video plugin never writes RDRAM (GLideN64 with frame buffer emulation off,
     * game/game_core.c). Then a rollback save copies only the RDRAM pages written since the slot's last save;
     * otherwise each display list counts as having written all of it. NULL: as 0. */
    int (*video_rdram_free)(void);
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
/* A stop was asked for (a session_run that drives its own frames checks this). */
int znx_bench_stop_requested(void);

uint64_t znx_now_ns(void);

/* Compiling ahead in idle time, for a session_run that drives its own frames (znx_bench_run's own
 * loop does this itself). Call on the emulation thread after each completed visible frame, never
 * for a re-simulated one. work_ns: what the frame's work took, everything since the last call
 * except waiting. spare_ns: the time that was left over in that frame (what the pacer slept), or
 * UINT64_MAX when nothing paces the frames. It compiles for at most
 * min(precompile_ms - the largest recent work, the smallest recent spare time - 2.5 ms). */
void znx_bench_idle(uint64_t work_ns, uint64_t spare_ns);

/* The two checksums of a "check" line: of the 8 MB RDRAM, whole, and without [skip_lo, skip_hi)
 * (multiples of 32; returns the whole checksum as rest when the range is empty). */
void znx_check_rdram(const uint8_t* rdram, long skip_lo, long skip_hi, uint64_t* whole, uint64_t* rest);

#endif
