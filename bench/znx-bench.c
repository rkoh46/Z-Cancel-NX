// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-bench: headless speed and determinism harness around RMG-K's mupen64plus core.
 *
 * Loads the core and the RSP HLE plugin the way any mupen64plus front end does
 * (dlopen, CoreStartup, CoreAttachPlugin), with a video plugin that draws nothing
 * (video-null.c) and the core's built-in dummy audio and input plugins, forces the settings an RMG-K rollback session forces, and
 * drives frames through M64CMD_ROLLBACK_EXECUTE exactly like rmgk_gekko.cpp does:
 * the core calls begin_frame/end_frame around every frame on the emulation thread, and
 * controller data comes from the rollback input callback.
 *
 * Output (stdout):
 *   hash <frame> <fnv1a64 of the 8 MB RDRAM>     every --hash-every frames
 *   watch <frame> <address> <byte>               with --watch ADDR, when that RDRAM byte changes
 *   rollback ...                                 with --rollback-test A B
 *   frames/wall/fps                              at the end
 */

#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void* l_core;
static void* l_rsp;
static void* l_video;
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

/* ---- options ---- */

static const char* l_rom_path;
static const char* l_inputs_path;
static int l_frames = 600;
static int l_hash_every = 60;
static int l_emumode = 2;
static int l_verbose;
static int l_rb_a = -1, l_rb_b = -1;
static long l_watch = -1;      /* RDRAM offset of a byte to report, or -1 */
static int l_watch_last = -1;
static int l_dump_frame = -1;  /* write RDRAM to l_dump_path after this many frames */
static const char* l_dump_path;

/* ---- run state ---- */

struct input_event { int frame; int player; uint32_t value; };
static struct input_event* l_events;
static int l_event_count;

static int l_frame;            /* frames completed so far == index of the frame about to run */
static const uint8_t* l_rdram;
static uint64_t l_hash_ns;     /* time spent hashing, kept out of the speed figure */
static uint64_t l_rollback_ns; /* time spent in the rollback test's save, load and replay */
static int l_failed;

static uint64_t* l_rb_hashes;  /* l_rb_hashes[i]: RDRAM hash after i frames past A */
static int l_rb_done;
static m64p_rollback_state l_rb_state;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t hash_rdram(void)
{
    uint64_t begin = now_ns();
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < RDRAM_SIZE; ++i) {
        h ^= l_rdram[i];
        h *= 0x100000001b3ull;
    }
    l_hash_ns += now_ns() - begin;
    return h;
}

/* ---- callbacks from the core ---- */

static void debug_callback(void* context, int level, const char* message)
{
    if (level <= M64MSG_WARNING || l_verbose)
        fprintf(stderr, "[%s] %s\n", (const char*)context, message);
}

/* The controller state for the frame about to run: the last script line at or before it. */
static int input_callback(void* values, int size, int players)
{
    uint32_t* out = values;
    if (size != (int)sizeof(uint32_t))
        return 0;
    for (int p = 0; p < players && p < MAX_PLAYERS; ++p)
        out[p] = 0;
    for (int i = 0; i < l_event_count && l_events[i].frame <= l_frame; ++i) {
        if (l_events[i].player < players)
            out[l_events[i].player] = l_events[i].value;
    }
    return 1;
}

/* Saves at frame A, and at frame B rewinds to A and runs A..B again as hidden frames
 * (M64CMD_ROLLBACK_RUN_FRAME without output, what a real rollback does), comparing RDRAM
 * after every frame with the first pass. */
static void rollback_save(void)
{
    uint64_t begin = now_ns();
    memset(&l_rb_state, 0, sizeof(l_rb_state));
    l_rb_state.frame = l_frame;
    m64p_error err = CoreDoCommand(M64CMD_ROLLBACK_SAVE_STATE, 0, &l_rb_state);
    uint64_t elapsed = now_ns() - begin;
    l_rollback_ns += elapsed;
    if (err != M64ERR_SUCCESS) {
        printf("rollback save FAILED: %s\n", CoreErrorMessage(err));
        l_failed = 1;
        l_rb_done = 1;
        return;
    }
    printf("rollback save frame %d: %llu us, %d bytes\n", l_frame,
           (unsigned long long)(elapsed / 1000), l_rb_state.len);
    l_rb_hashes = calloc((size_t)(l_rb_b - l_rb_a + 1), sizeof(uint64_t));
    l_rb_hashes[0] = hash_rdram();
}

static void rollback_replay(void)
{
    uint64_t begin = now_ns(), hash_at_begin = l_hash_ns;
    int mismatches = 0, first_bad = -1;

    m64p_error err = CoreDoCommand(M64CMD_ROLLBACK_LOAD_STATE, 0, &l_rb_state);
    uint64_t load_ns = now_ns() - begin;
    if (err != M64ERR_SUCCESS) {
        printf("rollback load FAILED: %s\n", CoreErrorMessage(err));
        l_failed = 1;
        l_rb_done = 1;
        return;
    }
    printf("rollback load frame %d -> %d: %llu us\n", l_rb_b, l_rb_a, (unsigned long long)(load_ns / 1000));

    l_frame = l_rb_a;
    if (hash_rdram() != l_rb_hashes[0]) {
        mismatches++;
        first_bad = l_rb_a;
    }
    uint64_t replay_begin = now_ns(), hash_before = l_hash_ns;
    while (l_frame < l_rb_b) {
        err = CoreDoCommand(M64CMD_ROLLBACK_RUN_FRAME, M64FRAME_OUTPUT_NONE, NULL);
        if (err != M64ERR_SUCCESS) {
            printf("rollback replay FAILED at frame %d: %s\n", l_frame, CoreErrorMessage(err));
            l_failed = 1;
            break;
        }
        l_frame++;
        if (hash_rdram() != l_rb_hashes[l_frame - l_rb_a]) {
            mismatches++;
            if (first_bad < 0)
                first_bad = l_frame;
        }
    }
    uint64_t replay_ns = (now_ns() - replay_begin) - (l_hash_ns - hash_before);
    int replayed = l_rb_b - l_rb_a;
    printf("rollback replay %d frames: %llu us (%.0f us/frame)\n", replayed,
           (unsigned long long)(replay_ns / 1000), replayed ? replay_ns / 1000.0 / replayed : 0.0);
    if (mismatches == 0 && !l_failed) {
        printf("rollback test %d..%d: PASS (%d frame hashes identical)\n", l_rb_a, l_rb_b, replayed + 1);
    } else {
        printf("rollback test %d..%d: FAIL (%d of %d hashes differ, first at frame %d)\n",
               l_rb_a, l_rb_b, mismatches, replayed + 1, first_bad);
        l_failed = 1;
    }
    CoreDoCommand(M64CMD_ROLLBACK_FREE_STATE, 0, l_rb_state.buffer);
    l_rb_state.buffer = NULL;
    l_rollback_ns += (now_ns() - begin) - (l_hash_ns - hash_at_begin); /* hashing is counted on its own */
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

static int begin_frame(void* user)
{
    (void)user;
    first_frame_setup();
    if (l_rb_a >= 0 && !l_rb_done) {
        if (l_frame == l_rb_a && l_rb_hashes == NULL)
            rollback_save();
        else if (l_frame == l_rb_b)
            rollback_replay();
    }
    return l_frame < l_frames && !l_failed;
}

static int end_frame(void* user)
{
    (void)user;
    l_frame++;
    if (l_rb_hashes != NULL && !l_rb_done && l_frame <= l_rb_b)
        l_rb_hashes[l_frame - l_rb_a] = hash_rdram();
    if (l_hash_every > 0 && l_frame % l_hash_every == 0)
        printf("hash %d %016llx\n", l_frame, (unsigned long long)hash_rdram());
    if (l_frame == l_dump_frame) {
        FILE* f = fopen(l_dump_path, "wb");
        if (f == NULL || fwrite(l_rdram, 1, RDRAM_SIZE, f) != RDRAM_SIZE)
            fprintf(stderr, "znx-bench: cannot write %s\n", l_dump_path);
        if (f != NULL)
            fclose(f);
    }
    if (l_watch >= 0) {
        /* The core keeps RDRAM as host-order 32-bit words: byte n of a word sits at n^3. */
        int value = l_rdram[l_watch ^ 3];
        if (value != l_watch_last)
            printf("watch %d %06lx %02x\n", l_frame, l_watch, value);
        l_watch_last = value;
    }
    return 1;
}

/* --cpu 0|1: the core forces the recompiler inside a rollback session, so the interpreters
 * run through plain M64CMD_EXECUTE, with the same hash points from the frame callback. */
static void plain_frame_callback(unsigned int index)
{
    (void)index;
    first_frame_setup();
    if (l_frame >= l_frames)
        return;
    end_frame(NULL);
    if (l_frame >= l_frames)
        CoreDoCommand(M64CMD_STOP, 0, NULL);
}

/* ---- setup ---- */

static void die(const char* what, m64p_error err)
{
    fprintf(stderr, "znx-bench: %s: %s\n", what, CoreErrorMessage ? CoreErrorMessage(err) : "error");
    exit(1);
}

static void* sym(void* lib, const char* name)
{
    void* p = dlsym(lib, name);
    if (p == NULL) {
        fprintf(stderr, "znx-bench: %s is missing from the library\n", name);
        exit(1);
    }
    return p;
}

static void* read_file(const char* path, long* size)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "znx-bench: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc((size_t)*size);
    if (data == NULL || fread(data, 1, (size_t)*size, f) != (size_t)*size) {
        fprintf(stderr, "znx-bench: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    return data;
}

static int compare_events(const void* a, const void* b)
{
    const struct input_event* x = a;
    const struct input_event* y = b;
    return x->frame - y->frame;
}

/* Lines "frame player buttons_hex": from that frame on, that player (1-4) holds that
 * value. The value is the controller's 4 answer bytes, first byte highest:
 * A B Z Start Up Down Left Right | - - L R C-up C-down C-left C-right | stick X | stick Y.
 * 10000000 is Start, 80000000 is A. '#' starts a comment. */
static void load_inputs(const char* path)
{
    FILE* f = fopen(path, "r");
    char line[256];
    int lineno = 0;
    if (f == NULL) {
        fprintf(stderr, "znx-bench: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        int frame, player;
        unsigned int value;
        lineno++;
        char* hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        if (line[strspn(line, " \t\r\n")] == '\0')
            continue;
        if (sscanf(line, "%d %d %x", &frame, &player, &value) != 3 || frame < 0 || player < 1 || player > MAX_PLAYERS) {
            fprintf(stderr, "znx-bench: %s:%d: expected \"frame player(1-4) buttons_hex\"\n", path, lineno);
            exit(1);
        }
        l_events = realloc(l_events, (size_t)(l_event_count + 1) * sizeof(*l_events));
        l_events[l_event_count++] = (struct input_event){ frame, player - 1, value };
    }
    fclose(f);
    mergesort(l_events, (size_t)l_event_count, sizeof(*l_events), compare_events); /* stable */
}

static void set_int(m64p_handle section, const char* name, int value)
{
    m64p_error err = ConfigSetParameter(section, name, M64TYPE_INT, &value);
    if (err != M64ERR_SUCCESS)
        die(name, err);
}

static void set_bool(m64p_handle section, const char* name, int value)
{
    m64p_error err = ConfigSetParameter(section, name, M64TYPE_BOOL, &value);
    if (err != M64ERR_SUCCESS)
        die(name, err);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: znx-bench --rom FILE [--frames N] [--inputs FILE] [--hash-every K]\n"
        "                 [--rollback-test A B] [--cpu 0|1|2] [--watch ADDR]\n"
        "                 [--dump-ram FRAME FILE] [--verbose]\n"
        "  --frames N           frames (vertical interrupts) to run, default 600\n"
        "  --inputs FILE        lines \"frame player buttons_hex\" (player 1-4)\n"
        "  --hash-every K       print \"hash <frame> <fnv1a64 of RDRAM>\" every K frames, default 60, 0 = never\n"
        "  --rollback-test A B  save at frame A, run to B, load, re-run A..B and compare every frame\n"
        "  --cpu MODE           2 dynamic recompiler in a rollback session, what RMG-K runs (default);\n"
        "                       0 pure or 1 cached interpreter, run without the rollback session\n"
        "  --dump-ram FRAME FILE  write the 8 MB RDRAM after FRAME frames (host word order). It holds\n"
        "                       game data: keep the file out of the repository\n"
        "  --watch ADDR         print \"watch <frame> <addr> <byte>\" when that RDRAM byte changes (hex address)\n");
    exit(2);
}

int main(int argc, char** argv)
{
    char exe[4096], dir[4096], path[4200], data_dir[4200], user_dir[4200];
    uint32_t exe_size = sizeof(exe);
    m64p_error err;
    m64p_handle core_cfg;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--rom") && i + 1 < argc) l_rom_path = argv[++i];
        else if (!strcmp(a, "--frames") && i + 1 < argc) l_frames = atoi(argv[++i]);
        else if (!strcmp(a, "--inputs") && i + 1 < argc) l_inputs_path = argv[++i];
        else if (!strcmp(a, "--hash-every") && i + 1 < argc) l_hash_every = atoi(argv[++i]);
        else if (!strcmp(a, "--cpu") && i + 1 < argc) l_emumode = atoi(argv[++i]);
        else if (!strcmp(a, "--rollback-test") && i + 2 < argc) { l_rb_a = atoi(argv[++i]); l_rb_b = atoi(argv[++i]); }
        else if (!strcmp(a, "--watch") && i + 1 < argc) l_watch = strtol(argv[++i], NULL, 16) & 0x7fffff;
        else if (!strcmp(a, "--dump-ram") && i + 2 < argc) { l_dump_frame = atoi(argv[++i]); l_dump_path = argv[++i]; }
        else if (!strcmp(a, "--verbose")) l_verbose = 1;
        else usage();
    }
    if (l_rom_path == NULL || l_frames <= 0)
        usage();
    if (l_rb_a >= 0 && (l_rb_b <= l_rb_a || l_rb_b > l_frames)) {
        fprintf(stderr, "znx-bench: --rollback-test needs 0 <= A < B <= frames\n");
        return 2;
    }
    if (l_rb_a >= 0 && l_emumode < 2) {
        fprintf(stderr, "znx-bench: --rollback-test needs the rollback session (--cpu 2)\n");
        return 2;
    }
    if (l_inputs_path != NULL)
        load_inputs(l_inputs_path);
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* The libraries, the core's data files and a scratch profile all sit next to the program. */
    if (_NSGetExecutablePath(exe, &exe_size) != 0 || realpath(exe, dir) == NULL) {
        fprintf(stderr, "znx-bench: cannot find my own path\n");
        return 1;
    }
    strcpy(dir, dirname(dir));
    snprintf(path, sizeof(path), "%s/libmupen64plus.dylib", dir);
    l_core = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (l_core == NULL) {
        fprintf(stderr, "znx-bench: %s\n", dlerror());
        return 1;
    }
    CoreStartup = sym(l_core, "CoreStartup");
    CoreShutdown = sym(l_core, "CoreShutdown");
    CoreDoCommand = sym(l_core, "CoreDoCommand");
    CoreAttachPlugin = sym(l_core, "CoreAttachPlugin");
    CoreDetachPlugin = sym(l_core, "CoreDetachPlugin");
    CoreErrorMessage = sym(l_core, "CoreErrorMessage");
    ConfigOpenSection = sym(l_core, "ConfigOpenSection");
    ConfigSetParameter = sym(l_core, "ConfigSetParameter");
    ConfigOverrideUserPaths = sym(l_core, "ConfigOverrideUserPaths");
    DebugMemGetPointer = sym(l_core, "DebugMemGetPointer");

    snprintf(data_dir, sizeof(data_dir), "%s/data", dir);
    snprintf(user_dir, sizeof(user_dir), "%s/profile", dir);
    mkdir(user_dir, 0755);
    err = CoreStartup(FRONTEND_API_VERSION, user_dir, data_dir, "core", debug_callback, NULL, NULL);
    if (err != M64ERR_SUCCESS)
        die("CoreStartup", err);
    strcat(user_dir, "/"); /* the core appends "save/" and friends without a separator */
    ConfigOverrideUserPaths(user_dir, user_dir);

    /* What RMG-K forces for a netplay or rollback session (Emulation.cpp,
     * apply_kaillera_deterministic_settings and the local rollback branch). */
    err = ConfigOpenSection("Core", &core_cfg);
    if (err != M64ERR_SUCCESS)
        die("ConfigOpenSection", err);
    set_int(core_cfg, "R4300Emulator", l_emumode);
    set_bool(core_cfg, "RandomizeInterrupt", 0);
    set_int(core_cfg, "CountPerOp", 0);
    set_int(core_cfg, "CountPerOpDenomPot", 0);
    set_int(core_cfg, "SiDmaDuration", -1);
    set_bool(core_cfg, "DisableExtraMem", 0);
    set_bool(core_cfg, "DisableSaveFileLoading", 1);
    set_bool(core_cfg, "OnScreenDisplay", 0);

    long rom_size;
    void* rom = read_file(l_rom_path, &rom_size);
    err = CoreDoCommand(M64CMD_ROM_OPEN, (int)rom_size, rom);
    free(rom);
    if (err != M64ERR_SUCCESS)
        die("M64CMD_ROM_OPEN", err);

    m64p_rom_settings rom_settings;
    if (CoreDoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(rom_settings), &rom_settings) == M64ERR_SUCCESS)
        printf("rom \"%s\" md5 %s countperop %u sidmaduration %u savetype %u\n", rom_settings.goodname,
               rom_settings.MD5, rom_settings.countperop, rom_settings.sidmaduration, rom_settings.savetype);

    /* Video: znx-video-null, which draws nothing but tells the core each display list is
     * done (the core's own dummy plugin does not, and the game then waits for ever).
     * Audio and input stay unattached: the core uses its dummy plugins (src/plugin/plugin.c).
     * RSP: the HLE plugin. Order as in any front end: video, audio, input, RSP. */
    snprintf(path, sizeof(path), "%s/znx-video-null.dylib", dir);
    l_video = dlopen(path, RTLD_NOW);
    if (l_video == NULL) {
        fprintf(stderr, "znx-bench: %s\n", dlerror());
        return 1;
    }
    err = CoreAttachPlugin(M64PLUGIN_GFX, l_video);
    if (err != M64ERR_SUCCESS)
        die("CoreAttachPlugin(video)", err);
    snprintf(path, sizeof(path), "%s/mupen64plus-rsp-hle.dylib", dir);
    l_rsp = dlopen(path, RTLD_NOW);
    if (l_rsp == NULL) {
        fprintf(stderr, "znx-bench: %s\n", dlerror());
        return 1;
    }
    err = ((ptr_PluginStartup)sym(l_rsp, "PluginStartup"))(l_core, "rsp-hle", debug_callback);
    if (err != M64ERR_SUCCESS)
        die("RSP PluginStartup", err);
    err = CoreAttachPlugin(M64PLUGIN_RSP, l_rsp);
    if (err != M64ERR_SUCCESS)
        die("CoreAttachPlugin(RSP)", err);

    /* Cartridge saves start blank and stay in memory, as in a netplay match. */
    m64p_save_data blank_saves;
    memset(&blank_saves, 0, sizeof(blank_saves));
    err = CoreDoCommand(M64CMD_ROLLBACK_SET_SAVE_DATA, 0, &blank_saves);
    if (err != M64ERR_SUCCESS)
        die("M64CMD_ROLLBACK_SET_SAVE_DATA", err);

    /* rmgk_gekko: deterministic interrupts, four synchronized controller ports whose data
     * comes from the callback, then the rollback execute loop. */
    err = CoreDoCommand(M64CMD_ROLLBACK_SET_DETERMINISTIC, 1, NULL);
    if (err != M64ERR_SUCCESS)
        die("M64CMD_ROLLBACK_SET_DETERMINISTIC", err);
    CoreDoCommand(M64CMD_ROLLBACK_SET_INPUT_PLAYERS, MAX_PLAYERS, NULL);
    CoreDoCommand(M64CMD_ROLLBACK_SET_INPUT_CALLBACK, 0, (void*)input_callback);

    m64p_rollback_execute_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_frame = begin_frame;
    callbacks.end_frame = end_frame;

    uint64_t begin = now_ns();
    if (l_emumode >= 2) {
        err = CoreDoCommand(M64CMD_ROLLBACK_EXECUTE, 0, &callbacks);
    } else {
        int off = 0;
        CoreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &off);
        CoreDoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void*)plain_frame_callback);
        err = CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
        CoreDoCommand(M64CMD_SET_FRAME_CALLBACK, 0, NULL);
    }
    uint64_t wall = now_ns() - begin;
    if (err != M64ERR_SUCCESS) {
        fprintf(stderr, "znx-bench: execute: %s\n", CoreErrorMessage(err));
        l_failed = 1;
    }

    double emu_s = (double)(wall - l_hash_ns - l_rollback_ns) / 1e9;
    printf("frames %d\n", l_frame);
    printf("wall %.3f s (emulation %.3f s, hashing %.3f s)\n", wall / 1e9, emu_s, l_hash_ns / 1e9);
    printf("fps %.1f\n", emu_s > 0 ? l_frame / emu_s : 0.0);
    if (l_frame < l_frames) {
        fprintf(stderr, "znx-bench: stopped after %d of %d frames\n", l_frame, l_frames);
        l_failed = 1;
    }

    CoreDoCommand(M64CMD_ROLLBACK_SET_INPUT_CALLBACK, 0, NULL);
    CoreDoCommand(M64CMD_ROLLBACK_SET_SAVE_DATA, 0, NULL);
    CoreDetachPlugin(M64PLUGIN_GFX);
    CoreDetachPlugin(M64PLUGIN_RSP);
    ((ptr_PluginShutdown)sym(l_rsp, "PluginShutdown"))();
    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    CoreShutdown();
    dlclose(l_rsp);
    dlclose(l_video);
    dlclose(l_core);
    free(l_rb_hashes);
    free(l_events);
    return l_failed ? 1 : 0;
}
