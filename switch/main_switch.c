// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-bench for the Nintendo Switch: the speed test of docs/PLAN.md step 1 as a homebrew
 * NRO. The measurement is bench/bench_core.c, the same code the Mac harness runs; this
 * file is the console around it.
 *
 * Files, all under sdmc:/switch/z-cancel-nx/:
 *   rom.z64      the ROM (or pass a path as the NRO's first argument)
 *   bench.ini    optional: frames=7200  hash_every=600  rollback_test=5400,5460
 *                inputs=<file>  cpu_mhz=1785 (0 leaves the clock alone)  verbose=1
 *   bench.log    every line of output, flushed line by line
 *
 * Lines also go to a PC running nxlink (-s) when the NRO was sent with it, and to the
 * screen when it was not. Plus stops the run early. The emulation runs on its own thread,
 * pinned to core 2; this thread watches it and ends the program with a message if a
 * single frame takes more than 10 seconds. */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "bench_core.h"
#include "znx_version.h"
#include "znx_switch.h"

#define BASE_DIR "sdmc:/switch/z-cancel-nx"
#define EMU_STACK (8u * 1024 * 1024)
#define EMU_CORE 2
#define STALL_LIMIT_NS 10000000000ull

static Mutex l_log_lock;
static FILE* l_log;
static bool l_nxlink;
static bool l_console;
static struct znx_clock_state l_clock;
static bool l_clock_set;

static void restore_clock(void)
{
    if (l_clock_set) {
        l_clock_set = false;
        znx_clocks_end(&l_clock);
    }
}

/* ---- output: nxlink or the screen, and the log file ---- */

/* The lock keeps lines whole. It is given up on after two seconds, so that the watchdog can
 * still speak (and leave) if the emulation thread is stuck inside a write. */
static bool lock_log(void)
{
    for (int i = 0; i < 200; ++i) {
        if (mutexTryLock(&l_log_lock))
            return true;
        svcSleepThread(10000000ll);
    }
    return false;
}

static void emit(const char* line)
{
    bool locked = lock_log();
    if (l_log != NULL) {
        fputs(line, l_log);
        fputc('\n', l_log);
        fflush(l_log);
        fsync(fileno(l_log)); /* so the line is on the card even if the next thing is a crash */
    }
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (locked)
        mutexUnlock(&l_log_lock);
}

void znx_log(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    emit(line);
}

static void bench_print(int is_error, const char* line)
{
    (void)is_error;
    emit(line);
}

/* ---- leaving when the emulation thread cannot be joined ---- */

/* Returning to the Homebrew Menu with a thread still running would leave it executing code
 * the loader is about to replace. Ask the system to close the whole program instead, the
 * way Melee-NX-NetPlay's switch_threads.c does (GPL-3.0-or-later, same author):
 * ISelfController::Exit for an application, RequestExitToSelf for an applet. */
static void __attribute__((noreturn)) hard_exit(void)
{
    Result rc;
    bool locked = lock_log();
    restore_clock();
    if (l_log != NULL) {
        fclose(l_log);
        l_log = NULL;
    }
    if (locked)
        mutexUnlock(&l_log_lock);
    if (appletGetAppletType() == AppletType_Application)
        rc = serviceDispatch(appletGetServiceSession_SelfController(), 0);
    else
        rc = appletRequestExitToSelf();
    if (R_SUCCEEDED(rc))
        svcSleepThread(10000000000ll); /* the system ends the process */
    svcExitProcess();
    __builtin_unreachable();
}

/* abort() and a failed assert() in the core: say so in the log, then leave. Linked with
 * -Wl,--wrap=abort -Wl,--wrap=__assert_func. */
void __attribute__((noreturn)) __wrap_abort(void)
{
    znx_log("znx-bench: abort() was called at frame %d; closing", znx_bench_frame());
    hard_exit();
}

void __attribute__((noreturn)) __wrap___assert_func(const char* file, int line, const char* function, const char* expression)
{
    znx_log("znx-bench: assertion failed at frame %d: %s, %s:%d (%s); closing", znx_bench_frame(),
            expression, file, line, function ? function : "?");
    hard_exit();
}

/* ---- settings ---- */

struct settings {
    struct znx_options options;
    char rom_path[512];
    char inputs_path[512];
    uint32_t cpu_mhz;
};

static bool file_exists(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static void read_ini(struct settings* s)
{
    char line[512];
    FILE* f = fopen(BASE_DIR "/bench.ini", "r");
    if (f == NULL) {
        znx_log("settings: no bench.ini, using the defaults");
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char* key = line + strspn(line, " \t");
        char* value = strchr(key, '=');
        if (*key == '#' || *key == ';' || value == NULL)
            continue;
        *value++ = '\0';
        key[strcspn(key, " \t")] = '\0';
        value += strspn(value, " \t");
        value[strcspn(value, "\r\n")] = '\0';
        if (!strcmp(key, "frames")) s->options.frames = atoi(value);
        else if (!strcmp(key, "hash_every")) s->options.hash_every = atoi(value);
        else if (!strcmp(key, "rollback_test")) {
            int a, b;
            if (sscanf(value, "%d ,%d", &a, &b) == 2) { s->options.rb_a = a; s->options.rb_b = b; }
            else { s->options.rb_a = s->options.rb_b = -1; } /* "rollback_test=" or "none" turns it off */
        }
        else if (!strcmp(key, "inputs")) {
            if (*value == '\0') s->inputs_path[0] = '\0';
            else if (strchr(value, ':') != NULL) snprintf(s->inputs_path, sizeof(s->inputs_path), "%s", value);
            else snprintf(s->inputs_path, sizeof(s->inputs_path), BASE_DIR "/%s", value);
        }
        else if (!strcmp(key, "cpu_mhz")) s->cpu_mhz = (uint32_t)atoi(value);
        else if (!strcmp(key, "rollback_stress")) {   /* rollback_stress=K,D */
            int k, d;
            if (sscanf(value, "%d ,%d", &k, &d) == 2 && k > d && d > 0) { s->options.stress_every = k; s->options.stress_depth = d; s->options.rb_a = s->options.rb_b = -1; }
        }
        else if (!strcmp(key, "verbose")) s->options.verbose = atoi(value);
        else znx_log("settings: unknown key \"%s\" in bench.ini", key);
    }
    fclose(f);
}

/* ---- the emulation thread ---- */

struct run {
    struct znx_host host;
    struct settings* settings;
    volatile int done;
    int result;
};

static void emulation_thread(void* arg)
{
    struct run* run = arg;
    s32 priority = -1, preferred = -1;
    u64 mask = 0;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE);
    znx_log("thread: emulation on core %d (mask 0x%llx), priority 0x%02x", (int)preferred,
            (unsigned long long)mask, (unsigned)priority);
    znx_log("cpu clock: %u MHz measured on the emulation core before the run", znx_measure_cpu_mhz());
    run->result = znx_bench_run(&run->host, &run->settings->options);
    znx_log("cpu clock: %u MHz measured on the emulation core after the run", znx_measure_cpu_mhz());
    run->done = 1;
}

/* ---- main ---- */

static u64 memory_info(InfoType type)
{
    u64 value = 0;
    svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0);
    return value;
}

int main(int argc, char** argv)
{
    static struct settings settings;
    static struct run run;
    PadState pad;
    Thread thread;
    Result rc;
    bool have_romfs, started = false;
    int exit_code = 1;

    mutexInit(&l_log_lock);
    socketInitializeDefault();
    l_nxlink = nxlinkStdio() >= 0; /* stdout and stderr to the PC that sent the NRO with nxlink -s */
    if (!l_nxlink) {
        consoleInit(NULL);         /* otherwise to the screen */
        l_console = true;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    mkdir("sdmc:/switch", 0777);
    mkdir(BASE_DIR, 0777);
    mkdir(BASE_DIR "/profile", 0777);
    l_log = fopen(BASE_DIR "/bench.log", "w");

    znx_log("znx-bench " ZNX_VERSION " (Nintendo Switch), built " __DATE__ " " __TIME__);
    if (l_log == NULL)
        znx_log("log: cannot write " BASE_DIR "/bench.log: %s", strerror(errno));
    znx_log("system: firmware %u.%u.%u, %s, memory %llu MB total, %llu MB in use, nxlink %s",
            HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()),
            appletGetAppletType() == AppletType_Application ? "running as an application" : "running as an applet",
            (unsigned long long)(memory_info(InfoType_TotalMemorySize) >> 20),
            (unsigned long long)(memory_info(InfoType_UsedMemorySize) >> 20),
            l_nxlink ? "connected" : "not connected");

    have_romfs = R_SUCCEEDED(rc = romfsInit()); /* the core's ROM database: romfs:/data/mupen64plus.ini */
    if (!have_romfs)
        znx_log("romfs: cannot open the NRO's data: 0x%08x", rc);

    znx_options_defaults(&settings.options);
    settings.options.frames = 7200;
    settings.options.hash_every = 600;
    settings.options.rb_a = 5400;
    settings.options.rb_b = 5460;
    settings.options.progress_every = 600;
    settings.cpu_mhz = 1785; /* the highest stock CPU clock of the first Switch model */
    read_ini(&settings);
    if (settings.options.rb_a >= 0 && settings.options.rb_b > settings.options.frames) {
        znx_log("settings: rollback test %d..%d is past the last frame %d: skipped",
                settings.options.rb_a, settings.options.rb_b, settings.options.frames);
        settings.options.rb_a = settings.options.rb_b = -1;
    }
    if (file_exists(BASE_DIR "/rom.z64"))
        snprintf(settings.rom_path, sizeof(settings.rom_path), BASE_DIR "/rom.z64");
    else if (argc > 1 && file_exists(argv[1]))
        snprintf(settings.rom_path, sizeof(settings.rom_path), "%s", argv[1]);
    settings.options.rom_path = settings.rom_path[0] ? settings.rom_path : NULL;
    settings.options.inputs_path = settings.inputs_path[0] ? settings.inputs_path : NULL;
    znx_log("settings: frames %d, hash_every %d, rollback_test %d,%d, inputs %s, cpu_mhz %u",
            settings.options.frames, settings.options.hash_every, settings.options.rb_a, settings.options.rb_b,
            settings.options.inputs_path ? settings.options.inputs_path : "none", (unsigned)settings.cpu_mhz);

    /* Keep the console awake and the screen on for the two minutes this takes. */
    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);

    znx_clocks_begin(settings.cpu_mhz, &l_clock);
    l_clock_set = true;
    const struct znx_clock_state clock = l_clock;
    if (!clock.available)
        znx_log("cpu clock: the clock service is not reachable; the system's own rate stays");
    else if (clock.requested_hz == 0)
        znx_log("cpu clock: system reports %u MHz; left alone", (unsigned)(clock.before_hz / 1000000u));
    else
        znx_log("cpu clock: system reported %u MHz, asked for %u MHz: %s, now reports %u MHz",
                (unsigned)(clock.before_hz / 1000000u), (unsigned)(clock.requested_hz / 1000000u),
                clock.request_ok ? "accepted" : "refused", (unsigned)(clock.after_hz / 1000000u));

    run.settings = &settings;
    run.host.core = znx_static_lib("core");
    run.host.video = znx_static_lib("video_null");
    run.host.rsp = znx_static_lib("rsp_hle");
    run.host.sym = osal_dynlib_getproc;
    run.host.print = bench_print;
    run.host.data_dir = "romfs:/data";
    run.host.user_dir = BASE_DIR "/profile";

    if (settings.options.rom_path == NULL) {
        znx_log("rom: not found. Put your ROM at " BASE_DIR "/rom.z64");
    } else if (run.host.core == NULL || run.host.video == NULL || run.host.rsp == NULL || !have_romfs) {
        znx_log("znx-bench: this build is incomplete (a linked library or the romfs is missing)");
    } else {
        znx_log("rom: %s", settings.options.rom_path);
        /* Core 2 at the highest priority an application may use, as Melee-NX-NetPlay runs its
         * simulation thread; lower priorities if the kernel refuses. */
        static const int priorities[] = { 0x1C, 0x28, 0x2C };
        /* This thread only watches. It must not share the emulation's core: there it would
         * wait behind the busier, higher-priority thread and the watchdog would never run. */
        rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1u << 0);
        if (R_FAILED(rc))
            znx_log("thread: cannot move the watchdog to core 0: 0x%08x", rc);
        rc = -1;
        for (size_t i = 0; i < sizeof(priorities) / sizeof(priorities[0]) && R_FAILED(rc); ++i)
            rc = threadCreate(&thread, emulation_thread, &run, NULL, EMU_STACK, priorities[i], EMU_CORE);
        if (R_FAILED(rc)) /* -2: the process's default core */
            rc = threadCreate(&thread, emulation_thread, &run, NULL, EMU_STACK, 0x2C, -2);
        if (R_SUCCEEDED(rc) && R_FAILED(rc = threadStart(&thread)))
            threadClose(&thread);
        if (R_FAILED(rc))
            znx_log("thread: cannot start the emulation thread: 0x%08x", rc);
        else
            started = true;
    }

    if (started) {
        bool stopping = false;
        while (!run.done) {
            svcSleepThread(50000000ll);
            padUpdate(&pad);
            if (!stopping && (!appletMainLoop() || (padGetButtonsDown(&pad) & HidNpadButton_Plus))) {
                znx_log("znx-bench: stop requested at frame %d", znx_bench_frame());
                znx_bench_stop();
                stopping = true;
            }
            if (l_console)
                consoleUpdate(NULL);
            if (znx_bench_stall_ns() > STALL_LIMIT_NS) {
                znx_log("znx-bench: WATCHDOG: frame %d has been running for more than 10 s; giving up",
                        znx_bench_frame());
                hard_exit();
            }
        }
        threadWaitForExit(&thread);
        threadClose(&thread);
        exit_code = run.result;
    }

    restore_clock();
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    znx_log("znx-bench: %s", exit_code == 0 ? "done" : "finished with errors");

    if (l_console) {
        /* Nobody is reading over nxlink: leave the result on the screen for a while. */
        printf("\nThe same text is in " BASE_DIR "/bench.log\nPress + to leave (or wait two minutes).\n");
        for (int i = 0; i < 2400 && appletMainLoop(); ++i) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            consoleUpdate(NULL);
            svcSleepThread(50000000ll);
        }
        consoleExit(NULL);
    }

    mutexLock(&l_log_lock);
    if (l_log != NULL) {
        fclose(l_log);
        l_log = NULL;
    }
    mutexUnlock(&l_log_lock);
    if (have_romfs)
        romfsExit();
    socketExit();
    return 0;
}
