// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-bench, shared part: see bench_core.h.
 *
 * Sets the core up the way an RMG-K rollback session does, with a video plugin that draws
 * nothing (video-null.c) and the core's built-in dummy audio and input plugins, and drives
 * frames through M64CMD_ROLLBACK_EXECUTE exactly like rmgk_gekko.cpp does: the core calls
 * begin_frame/end_frame around every frame on the emulation thread, and controller data
 * comes from the rollback input callback.
 *
 * Result lines:
 *   rom "<goodname>" md5 ... countperop ... sidmaduration ... savetype ...
 *   hash <frame> <fnv1a64 of the 8 MB RDRAM>     every hash_every frames
 *   progress <frame> of <frames> ...             every progress_every frames
 *   watch <frame> <address> <byte>               when the watched RDRAM byte changes
 *   rollback save/load/replay/test ...           with a rollback test
 *   frames / wall / fps                          at the end
 *   window <name> <from>..<to>: ...              ms per frame and the worst frame of each window
 *
 * A frame's time is measured from the return of begin_frame to the call of end_frame, so
 * hashing, the rollback test and printing are never inside it. */

#include "bench_core.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_debugger.h"
#include "m64p_frontend.h"

#define RDRAM_SIZE 0x800000u
#define MAX_PLAYERS 4
#define FRONTEND_API_VERSION 0x020106 /* src/main/version.h of the core */

/* ---- core entry points ---- */

static ptr_CoreStartup        CoreStartup;
static ptr_CoreShutdown       CoreShutdown;
static ptr_CoreDoCommand      CoreDoCommand;
static ptr_CoreAttachPlugin   CoreAttachPlugin;
static ptr_CoreDetachPlugin   CoreDetachPlugin;
static ptr_CoreErrorMessage   CoreErrorMessage;
static ptr_ConfigOpenSection  ConfigOpenSection;
static ptr_ConfigSetParameter ConfigSetParameter;
static ptr_ConfigOverrideUserPaths ConfigOverrideUserPaths;
static ptr_DebugMemGetPointer DebugMemGetPointer;

/* ---- run state ---- */

static const struct znx_host* l_host;
static struct znx_options l_opt;

struct input_event { int frame; int player; uint32_t value; };
static struct input_event* l_events;
static int l_event_count;

static volatile int l_frame;   /* frames completed so far == index of the frame about to run */
static const uint8_t* l_rdram;
static uint64_t l_hash_ns;     /* time spent hashing */
static int l_failed;
static volatile int l_stop;
static int l_watch_last;

static uint64_t* l_rb_hashes;  /* l_rb_hashes[i]: RDRAM hash after i frames past A */
static int l_rb_done;
static m64p_rollback_state l_rb_state;

static uint32_t* l_frame_us10; /* time of every frame in units of 0.1 us */
static uint64_t l_frame_begin; /* when the frame in flight started, 0 between frames */
static uint64_t l_emu_ns;      /* sum of all frame times */
static uint64_t l_progress_ns; /* frame time since the last progress line */
static volatile uint64_t l_alive_ns; /* start of the frame or replayed frame in flight, 0 = idle */

uint64_t znx_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t znx_bench_stall_ns(void)
{
    uint64_t since = l_alive_ns;
    uint64_t now = znx_now_ns();
    return since != 0 && now > since ? now - since : 0;
}

int znx_bench_frame(void) { return l_frame; }
void znx_bench_stop(void) { l_stop = 1; }

void znx_options_defaults(struct znx_options* o)
{
    memset(o, 0, sizeof(*o));
    o->frames = 600;
    o->hash_every = 60;
    o->emumode = 2;
    o->rb_a = o->rb_b = -1;
    o->watch = -1;
    o->dump_frame = -1;
    /* Vanilla Super Smash Bros. (U) without input: logo and intro, title at 4313, then the
     * CPU demo fight from 4983. Windows that the run does not reach are not printed. */
    o->windows[0] = (struct znx_window){ "intro", 0, 4300 };
    o->windows[1] = (struct znx_window){ "fight", 5000, 7200 };
    o->window_count = 2;
}

/* ---- output ---- */

static void out(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    l_host->print(0, line);
}

static void err(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    l_host->print(1, line);
}

static uint64_t hash_rdram(void)
{
    uint64_t begin = znx_now_ns();
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < RDRAM_SIZE; ++i) {
        h ^= l_rdram[i];
        h *= 0x100000001b3ull;
    }
    l_hash_ns += znx_now_ns() - begin;
    return h;
}

/* ---- callbacks from the core ---- */

static void debug_callback(void* context, int level, const char* message)
{
    if (level <= M64MSG_WARNING || l_opt.verbose)
        err("[%s] %s", (const char*)context, message);
}

/* The controller state for the frame about to run: the last script line at or before it. */
static int input_callback(void* values, int size, int players)
{
    uint32_t* result = values;
    if (size != (int)sizeof(uint32_t))
        return 0;
    for (int p = 0; p < players && p < MAX_PLAYERS; ++p)
        result[p] = 0;
    for (int i = 0; i < l_event_count && l_events[i].frame <= l_frame; ++i) {
        if (l_events[i].player < players)
            result[l_events[i].player] = l_events[i].value;
    }
    return 1;
}

/* Saves at frame A, and at frame B rewinds to A and runs A..B again as hidden frames
 * (M64CMD_ROLLBACK_RUN_FRAME without output, what a real rollback does), comparing RDRAM
 * after every frame with the first pass. */
static void rollback_save(void)
{
    uint64_t begin = znx_now_ns();
    memset(&l_rb_state, 0, sizeof(l_rb_state));
    l_rb_state.frame = l_frame;
    m64p_error error = CoreDoCommand(M64CMD_ROLLBACK_SAVE_STATE, 0, &l_rb_state);
    uint64_t elapsed = znx_now_ns() - begin;
    if (error != M64ERR_SUCCESS) {
        out("rollback save FAILED: %s", CoreErrorMessage(error));
        l_failed = 1;
        l_rb_done = 1;
        return;
    }
    out("rollback save frame %d: %llu us, %d bytes", l_frame,
        (unsigned long long)(elapsed / 1000), l_rb_state.len);
    l_rb_hashes = calloc((size_t)(l_opt.rb_b - l_opt.rb_a + 1), sizeof(uint64_t));
    l_rb_hashes[0] = hash_rdram();
}

static void rollback_replay(void)
{
    const int a = l_opt.rb_a, b = l_opt.rb_b;
    uint64_t begin = znx_now_ns();
    int mismatches = 0, first_bad = -1;

    l_alive_ns = begin;
    m64p_error error = CoreDoCommand(M64CMD_ROLLBACK_LOAD_STATE, 0, &l_rb_state);
    uint64_t load_ns = znx_now_ns() - begin;
    if (error != M64ERR_SUCCESS) {
        out("rollback load FAILED: %s", CoreErrorMessage(error));
        l_failed = 1;
        l_rb_done = 1;
        return;
    }
    out("rollback load frame %d -> %d: %llu us", b, a, (unsigned long long)(load_ns / 1000));

    l_frame = a;
    if (hash_rdram() != l_rb_hashes[0]) {
        mismatches++;
        first_bad = a;
    }
    uint64_t replay_begin = znx_now_ns(), hash_before = l_hash_ns;
    while (l_frame < b) {
        l_alive_ns = znx_now_ns();
        error = CoreDoCommand(M64CMD_ROLLBACK_RUN_FRAME, M64FRAME_OUTPUT_NONE, NULL);
        if (error != M64ERR_SUCCESS) {
            out("rollback replay FAILED at frame %d: %s", l_frame, CoreErrorMessage(error));
            l_failed = 1;
            break;
        }
        l_frame++;
        if (hash_rdram() != l_rb_hashes[l_frame - a]) {
            mismatches++;
            if (first_bad < 0)
                first_bad = l_frame;
        }
    }
    uint64_t replay_ns = (znx_now_ns() - replay_begin) - (l_hash_ns - hash_before);
    int replayed = b - a;
    out("rollback replay %d frames: %llu us (%.0f us/frame)", replayed,
        (unsigned long long)(replay_ns / 1000), replayed ? replay_ns / 1000.0 / replayed : 0.0);
    if (mismatches == 0 && !l_failed) {
        out("rollback test %d..%d: PASS (%d frame hashes identical)", a, b, replayed + 1);
    } else {
        out("rollback test %d..%d: FAIL (%d of %d hashes differ, first at frame %d)",
            a, b, mismatches, replayed + 1, first_bad);
        l_failed = 1;
    }
    CoreDoCommand(M64CMD_ROLLBACK_FREE_STATE, 0, l_rb_state.buffer);
    l_rb_state.buffer = NULL;
    l_rb_done = 1;
}

static void first_frame_setup(void)
{
    if (l_rdram != NULL)
        return;
    l_rdram = DebugMemGetPointer(M64P_DBG_PTR_RDRAM);
    /* Frames are shown to nobody: keep video and audio output on (they are the dummy
     * plugins) but no speed limiter and no host event polling. */
    CoreDoCommand(M64CMD_FRAME_OUTPUT_SET, M64FRAME_OUTPUT_VIDEO | M64FRAME_OUTPUT_AUDIO, NULL);
}

static void frame_timer_start(void)
{
    l_frame_begin = znx_now_ns();
    l_alive_ns = l_frame_begin;
}

/* Ends the timing of the frame in flight; l_frame is still its index. */
static void frame_timer_stop(void)
{
    uint64_t ns;
    if (l_frame_begin == 0)
        return;
    ns = znx_now_ns() - l_frame_begin;
    l_frame_begin = 0;
    l_alive_ns = 0;
    l_emu_ns += ns;
    l_progress_ns += ns;
    if (l_frame >= 0 && l_frame < l_opt.frames)
        l_frame_us10[l_frame] = ns / 100 > 0xffffffffull ? 0xffffffffu : (uint32_t)(ns / 100);
}

static int begin_frame(void* user)
{
    (void)user;
    first_frame_setup();
    if (l_opt.rb_a >= 0 && !l_rb_done) {
        if (l_frame == l_opt.rb_a && l_rb_hashes == NULL)
            rollback_save();
        else if (l_frame == l_opt.rb_b)
            rollback_replay();
    }
    if (l_frame >= l_opt.frames || l_failed || l_stop) {
        l_alive_ns = 0;
        return 0;
    }
    frame_timer_start();
    return 1;
}

/* Everything that happens once a frame is complete; the frame's timer has been stopped. */
static void frame_done(void)
{
    l_frame++;
    if (l_rb_hashes != NULL && !l_rb_done && l_frame <= l_opt.rb_b)
        l_rb_hashes[l_frame - l_opt.rb_a] = hash_rdram();
    if (l_opt.hash_every > 0 && l_frame % l_opt.hash_every == 0)
        out("hash %d %016llx", l_frame, (unsigned long long)hash_rdram());
    if (l_opt.progress_every > 0 && l_frame % l_opt.progress_every == 0) {
        out("progress %d of %d, %.3f ms/frame over the last %d", l_frame, l_opt.frames,
            l_progress_ns / 1e6 / l_opt.progress_every, l_opt.progress_every);
        l_progress_ns = 0;
    }
    if (l_frame == l_opt.dump_frame) {
        FILE* f = fopen(l_opt.dump_path, "wb");
        if (f == NULL || fwrite(l_rdram, 1, RDRAM_SIZE, f) != RDRAM_SIZE)
            err("znx-bench: cannot write %s", l_opt.dump_path);
        if (f != NULL)
            fclose(f);
    }
    if (l_opt.watch >= 0) {
        /* The core keeps RDRAM as host-order 32-bit words: byte n of a word sits at n^3. */
        int value = l_rdram[l_opt.watch ^ 3];
        if (value != l_watch_last)
            out("watch %d %06lx %02x", l_frame, l_opt.watch, value);
        l_watch_last = value;
    }
}

static int end_frame(void* user)
{
    (void)user;
    frame_timer_stop();
    frame_done();
    return 1;
}

/* emumode 0|1: the core forces the recompiler inside a rollback session, so the
 * interpreters run through plain M64CMD_EXECUTE, with the same hash points from the frame
 * callback. A frame is then the time between two callbacks. */
static void plain_frame_callback(unsigned int index)
{
    (void)index;
    first_frame_setup();
    if (l_frame >= l_opt.frames)
        return;
    frame_timer_stop();
    frame_done();
    if (l_frame >= l_opt.frames || l_stop)
        CoreDoCommand(M64CMD_STOP, 0, NULL);
    else
        frame_timer_start();
}

/* ---- setup ---- */

static int fail(const char* what, m64p_error error)
{
    err("znx-bench: %s: %s", what, CoreErrorMessage ? CoreErrorMessage(error) : "error");
    return 1;
}

static void* sym(void* lib, const char* name)
{
    void* p = l_host->sym(lib, name);
    if (p == NULL)
        err("znx-bench: %s is missing from the library", name);
    return p;
}

static void* read_file(const char* path, long* size)
{
    FILE* f = fopen(path, "rb");
    void* data;
    if (f == NULL) {
        err("znx-bench: cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = *size > 0 ? malloc((size_t)*size) : NULL;
    if (data == NULL || fread(data, 1, (size_t)*size, f) != (size_t)*size) {
        err("znx-bench: cannot read %s", path);
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

/* Lines "frame player buttons_hex": from that frame on, that player (1-4) holds that
 * value. The value is the controller's 4 answer bytes, first byte highest:
 * A B Z Start Up Down Left Right | - - L R C-up C-down C-left C-right | stick X | stick Y.
 * 10000000 is Start, 80000000 is A. '#' starts a comment. */
static int load_inputs(const char* path)
{
    FILE* f = fopen(path, "r");
    char line[256];
    int lineno = 0;
    if (f == NULL) {
        err("znx-bench: cannot open %s: %s", path, strerror(errno));
        return 1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        int frame, player, at;
        unsigned int value;
        lineno++;
        char* hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        if (line[strspn(line, " \t\r\n")] == '\0')
            continue;
        if (sscanf(line, "%d %d %x", &frame, &player, &value) != 3 || frame < 0 || player < 1 || player > MAX_PLAYERS) {
            err("znx-bench: %s:%d: expected \"frame player(1-4) buttons_hex\"", path, lineno);
            fclose(f);
            return 1;
        }
        l_events = realloc(l_events, (size_t)(l_event_count + 1) * sizeof(*l_events));
        /* Kept sorted by frame; lines for the same frame stay in file order. */
        for (at = l_event_count; at > 0 && l_events[at - 1].frame > frame; --at)
            l_events[at] = l_events[at - 1];
        l_events[at] = (struct input_event){ frame, player - 1, value };
        l_event_count++;
    }
    fclose(f);
    return 0;
}

static int set_int(m64p_handle section, const char* name, int value)
{
    m64p_error error = ConfigSetParameter(section, name, M64TYPE_INT, &value);
    return error == M64ERR_SUCCESS ? 0 : fail(name, error);
}

static int set_bool(m64p_handle section, const char* name, int value)
{
    m64p_error error = ConfigSetParameter(section, name, M64TYPE_BOOL, &value);
    return error == M64ERR_SUCCESS ? 0 : fail(name, error);
}

static void print_windows(void)
{
    for (int w = 0; w < l_opt.window_count; ++w) {
        const struct znx_window* win = &l_opt.windows[w];
        int to = win->to < l_frame ? win->to : l_frame;
        uint64_t total = 0;
        uint32_t worst = 0;
        int worst_frame = win->from;
        if (to <= win->from)
            continue;
        for (int f = win->from; f < to; ++f) {
            total += l_frame_us10[f];
            if (l_frame_us10[f] > worst) {
                worst = l_frame_us10[f];
                worst_frame = f;
            }
        }
        out("window %s %d..%d: %d frames, %.3f ms/frame, worst %.3f ms at frame %d", win->name,
            win->from, to, to - win->from, total / 1e4 / (to - win->from), worst / 1e4, worst_frame);
    }
}

int znx_bench_run(const struct znx_host* host, const struct znx_options* options)
{
    char user_dir[4200];
    m64p_error error;
    m64p_handle core_cfg;
    ptr_PluginStartup rsp_startup;
    ptr_PluginShutdown rsp_shutdown;
    int rom_open = 0, video_attached = 0, rsp_attached = 0, rsp_started = 0;

    l_host = host;
    l_opt = *options;
    l_frame = 0;
    l_failed = 0;
    l_watch_last = -1;

    if (l_opt.rom_path == NULL || l_opt.frames <= 0) {
        err("znx-bench: a ROM and a positive frame count are needed");
        return 2;
    }
    if (l_opt.rb_a >= 0 && (l_opt.rb_b <= l_opt.rb_a || l_opt.rb_b > l_opt.frames)) {
        err("znx-bench: the rollback test needs 0 <= A < B <= frames");
        return 2;
    }
    if (l_opt.rb_a >= 0 && l_opt.emumode < 2) {
        err("znx-bench: the rollback test needs the rollback session (cpu 2)");
        return 2;
    }
    if (l_opt.inputs_path != NULL && load_inputs(l_opt.inputs_path) != 0)
        return 1;
    l_frame_us10 = calloc((size_t)l_opt.frames, sizeof(*l_frame_us10));
    if (l_frame_us10 == NULL)
        return 1;

    CoreErrorMessage = sym(host->core, "CoreErrorMessage");
    CoreStartup = sym(host->core, "CoreStartup");
    CoreShutdown = sym(host->core, "CoreShutdown");
    CoreDoCommand = sym(host->core, "CoreDoCommand");
    CoreAttachPlugin = sym(host->core, "CoreAttachPlugin");
    CoreDetachPlugin = sym(host->core, "CoreDetachPlugin");
    ConfigOpenSection = sym(host->core, "ConfigOpenSection");
    ConfigSetParameter = sym(host->core, "ConfigSetParameter");
    ConfigOverrideUserPaths = sym(host->core, "ConfigOverrideUserPaths");
    DebugMemGetPointer = sym(host->core, "DebugMemGetPointer");
    rsp_startup = sym(host->rsp, "PluginStartup");
    rsp_shutdown = sym(host->rsp, "PluginShutdown");
    if (!CoreErrorMessage || !CoreStartup || !CoreShutdown || !CoreDoCommand || !CoreAttachPlugin ||
        !CoreDetachPlugin || !ConfigOpenSection || !ConfigSetParameter || !ConfigOverrideUserPaths ||
        !DebugMemGetPointer || !rsp_startup || !rsp_shutdown)
        return 1;

    error = CoreStartup(FRONTEND_API_VERSION, host->user_dir, host->data_dir, "core", debug_callback, NULL, NULL);
    if (error != M64ERR_SUCCESS)
        return fail("CoreStartup", error);
    snprintf(user_dir, sizeof(user_dir), "%s/", host->user_dir); /* the core appends "save/" and friends without a separator */
    ConfigOverrideUserPaths(user_dir, user_dir);

    /* What RMG-K forces for a netplay or rollback session (Emulation.cpp,
     * apply_kaillera_deterministic_settings and the local rollback branch). */
    error = ConfigOpenSection("Core", &core_cfg);
    if (error != M64ERR_SUCCESS) {
        l_failed = fail("ConfigOpenSection", error);
        goto shutdown;
    }
    if (set_int(core_cfg, "R4300Emulator", l_opt.emumode) || set_bool(core_cfg, "RandomizeInterrupt", 0) ||
        set_int(core_cfg, "CountPerOp", 0) || set_int(core_cfg, "CountPerOpDenomPot", 0) ||
        set_int(core_cfg, "SiDmaDuration", -1) || set_bool(core_cfg, "DisableExtraMem", 0) ||
        set_bool(core_cfg, "DisableSaveFileLoading", 1) || set_bool(core_cfg, "OnScreenDisplay", 0)) {
        l_failed = 1;
        goto shutdown;
    }

    {
        long rom_size = 0;
        void* rom = read_file(l_opt.rom_path, &rom_size);
        if (rom == NULL) {
            l_failed = 1;
            goto shutdown;
        }
        error = CoreDoCommand(M64CMD_ROM_OPEN, (int)rom_size, rom);
        free(rom);
        if (error != M64ERR_SUCCESS) {
            l_failed = fail("M64CMD_ROM_OPEN", error);
            goto shutdown;
        }
        rom_open = 1;
    }

    {
        m64p_rom_settings rom_settings;
        if (CoreDoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(rom_settings), &rom_settings) == M64ERR_SUCCESS)
            out("rom \"%s\" md5 %s countperop %u sidmaduration %u savetype %u", rom_settings.goodname,
                rom_settings.MD5, rom_settings.countperop, rom_settings.sidmaduration, rom_settings.savetype);
    }

    /* Video: znx-video-null, which draws nothing but tells the core each display list is
     * done (the core's own dummy plugin does not, and the game then waits for ever).
     * Audio and input stay unattached: the core uses its dummy plugins (src/plugin/plugin.c).
     * RSP: the HLE plugin. Order as in any front end: video, audio, input, RSP. */
    error = CoreAttachPlugin(M64PLUGIN_GFX, host->video);
    if (error != M64ERR_SUCCESS) {
        l_failed = fail("CoreAttachPlugin(video)", error);
        goto shutdown;
    }
    video_attached = 1;
    error = rsp_startup(host->core, "rsp-hle", debug_callback);
    if (error != M64ERR_SUCCESS) {
        l_failed = fail("RSP PluginStartup", error);
        goto shutdown;
    }
    rsp_started = 1;
    error = CoreAttachPlugin(M64PLUGIN_RSP, host->rsp);
    if (error != M64ERR_SUCCESS) {
        l_failed = fail("CoreAttachPlugin(RSP)", error);
        goto shutdown;
    }
    rsp_attached = 1;

    {
        /* Cartridge saves start blank and stay in memory, as in a netplay match. */
        m64p_save_data blank_saves;
        memset(&blank_saves, 0, sizeof(blank_saves));
        error = CoreDoCommand(M64CMD_ROLLBACK_SET_SAVE_DATA, 0, &blank_saves);
        if (error != M64ERR_SUCCESS) {
            l_failed = fail("M64CMD_ROLLBACK_SET_SAVE_DATA", error);
            goto shutdown;
        }
    }

    /* rmgk_gekko: deterministic interrupts, four synchronized controller ports whose data
     * comes from the callback, then the rollback execute loop. */
    error = CoreDoCommand(M64CMD_ROLLBACK_SET_DETERMINISTIC, 1, NULL);
    if (error != M64ERR_SUCCESS) {
        l_failed = fail("M64CMD_ROLLBACK_SET_DETERMINISTIC", error);
        goto shutdown;
    }
    CoreDoCommand(M64CMD_ROLLBACK_SET_INPUT_PLAYERS, MAX_PLAYERS, NULL);
    CoreDoCommand(M64CMD_ROLLBACK_SET_INPUT_CALLBACK, 0, (void*)input_callback);

    {
        m64p_rollback_execute_callbacks callbacks;
        uint64_t begin, wall;
        double emu_s;

        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.begin_frame = begin_frame;
        callbacks.end_frame = end_frame;

        begin = znx_now_ns();
        if (l_opt.emumode >= 2) {
            error = CoreDoCommand(M64CMD_ROLLBACK_EXECUTE, 0, &callbacks);
        } else {
            int off = 0;
            CoreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &off);
            CoreDoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void*)plain_frame_callback);
            frame_timer_start();
            error = CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
            CoreDoCommand(M64CMD_SET_FRAME_CALLBACK, 0, NULL);
        }
        wall = znx_now_ns() - begin;
        l_frame_begin = 0;
        l_alive_ns = 0;
        if (error != M64ERR_SUCCESS) {
            err("znx-bench: execute: %s", CoreErrorMessage(error));
            l_failed = 1;
        }

        emu_s = (double)l_emu_ns / 1e9;
        out("frames %d", l_frame);
        out("wall %.3f s (emulation %.3f s, hashing %.3f s)", wall / 1e9, emu_s, l_hash_ns / 1e9);
        out("fps %.1f", emu_s > 0 ? l_frame / emu_s : 0.0);
        print_windows();
        if (l_frame < l_opt.frames) {
            err("znx-bench: stopped after %d of %d frames", l_frame, l_opt.frames);
            l_failed = 1;
        }
    }

shutdown:
    CoreDoCommand(M64CMD_ROLLBACK_SET_INPUT_CALLBACK, 0, NULL);
    CoreDoCommand(M64CMD_ROLLBACK_SET_SAVE_DATA, 0, NULL);
    if (video_attached)
        CoreDetachPlugin(M64PLUGIN_GFX);
    if (rsp_attached)
        CoreDetachPlugin(M64PLUGIN_RSP);
    if (rsp_started)
        rsp_shutdown();
    if (rom_open)
        CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    CoreShutdown();
    free(l_rb_hashes);
    l_rb_hashes = NULL;
    free(l_events);
    l_events = NULL;
    l_event_count = 0;
    free(l_frame_us10);
    l_frame_us10 = NULL;
    return l_failed ? 1 : 0;
}
