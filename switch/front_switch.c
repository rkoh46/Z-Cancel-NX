// SPDX-License-Identifier: GPL-3.0-or-later
/* See front_switch.h.
 *
 * Lines go to the log file, flushed line by line, and to a PC running nxlink (-s) when the
 * NRO was sent with it, or to the screen when it was not and the program has no picture of
 * its own. The emulation runs on its own thread, pinned to core 2; the main thread watches
 * it and ends the program with a message if a single frame takes more than 10 seconds. */

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
#include "front_switch.h"
#include "znx_version.h"
#include "znx_switch.h"

#define EMU_STACK (8u * 1024 * 1024)
#define EMU_CORE 2
#define STALL_LIMIT_NS 10000000000ull
#define TAIL_LINES 40

static const char* l_program = "znx";
static Mutex l_log_lock;
static FILE* l_log;
static bool l_nxlink;
static bool l_console;
static bool l_text_screen;
static bool l_have_romfs;
static PadState l_pad;
static struct znx_clock_state l_clock;
static bool l_clock_set;
/* The last lines, for a program without a text screen to show when it ends. */
static char l_tail[TAIL_LINES][160];
static unsigned l_tail_count;

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
    if (l_nxlink || l_console) {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    if (locked) {
        snprintf(l_tail[l_tail_count % TAIL_LINES], sizeof(l_tail[0]), "%s", line);
        l_tail_count++;
        mutexUnlock(&l_log_lock);
    }
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

void znx_front_print(int is_error, const char* line)
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

/* abort() and a failed assert() in the core or a plugin: say so in the log, then leave.
 * Linked with -Wl,--wrap=abort -Wl,--wrap=__assert_func. */
void __attribute__((noreturn)) __wrap_abort(void)
{
    znx_log("%s: abort() was called at frame %d; closing", l_program, znx_bench_frame());
    hard_exit();
}

void __attribute__((noreturn)) __wrap___assert_func(const char* file, int line, const char* function, const char* expression)
{
    znx_log("%s: assertion failed at frame %d: %s, %s:%d (%s); closing", l_program, znx_bench_frame(),
            expression, file, line, function ? function : "?");
    hard_exit();
}

/* ---- start ---- */

static u64 memory_info(InfoType type)
{
    u64 value = 0;
    svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0);
    return value;
}

void znx_front_begin(const char* program, const char* log_name, bool text_screen)
{
    char path[256];
    l_program = program;
    l_text_screen = text_screen;
    mutexInit(&l_log_lock);
    socketInitializeDefault();
    l_nxlink = nxlinkStdio() >= 0; /* stdout and stderr to the PC that sent the NRO with nxlink -s */
    if (!l_nxlink && text_screen) {
        consoleInit(NULL);         /* otherwise to the screen */
        l_console = true;
    }
    /* Plus on any controller, attached or handheld. */
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&l_pad);

    mkdir("sdmc:/switch", 0777);
    mkdir(ZNX_BASE_DIR, 0777);
    mkdir(ZNX_BASE_DIR "/profile", 0777);
    snprintf(path, sizeof(path), ZNX_BASE_DIR "/%s", log_name);
    l_log = fopen(path, "w");

    znx_log("%s " ZNX_VERSION " (Nintendo Switch), built " __DATE__ " " __TIME__, program);
    if (l_log == NULL)
        znx_log("log: cannot write %s: %s", path, strerror(errno));
    znx_log("system: firmware %u.%u.%u, %s, memory %llu MB total, %llu MB in use, nxlink %s",
            HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()),
            appletGetAppletType() == AppletType_Application ? "running as an application" : "running as an applet",
            (unsigned long long)(memory_info(InfoType_TotalMemorySize) >> 20),
            (unsigned long long)(memory_info(InfoType_UsedMemorySize) >> 20),
            l_nxlink ? "connected" : "not connected");
}

bool znx_front_romfs(void)
{
    Result rc = romfsInit(); /* the core's ROM database: romfs:/data/mupen64plus.ini */
    l_have_romfs = R_SUCCEEDED(rc);
    if (!l_have_romfs)
        znx_log("romfs: cannot open the NRO's data: 0x%08x", rc);
    return l_have_romfs;
}

/* ---- settings ---- */

bool znx_file_exists(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bool znx_front_read_ini(const char* ini_name, bool (*key_fn)(const char* name, const char* value, void* user), void* user)
{
    char line[512], path[256];
    FILE* f;
    snprintf(path, sizeof(path), ZNX_BASE_DIR "/%s", ini_name);
    f = fopen(path, "r");
    if (f == NULL) {
        znx_log("settings: no %s, using the defaults", ini_name);
        return false;
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
        if (!key_fn(key, value, user))
            znx_log("settings: unknown key \"%s\" in %s", key, ini_name);
    }
    fclose(f);
    return true;
}

const char* znx_front_find_rom(int argc, char** argv)
{
    static char rom_path[512];
    if (znx_file_exists(ZNX_BASE_DIR "/rom.z64"))
        snprintf(rom_path, sizeof(rom_path), ZNX_BASE_DIR "/rom.z64");
    else if (argc > 1 && znx_file_exists(argv[1]))
        snprintf(rom_path, sizeof(rom_path), "%s", argv[1]);
    else {
        znx_log("rom: not found. Put your ROM at " ZNX_BASE_DIR "/rom.z64");
        return NULL;
    }
    znx_log("rom: %s", rom_path);
    return rom_path;
}

void znx_front_power_begin(uint32_t cpu_mhz)
{
    /* Keep the console awake and the screen on while this runs. */
    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);

    znx_clocks_begin(cpu_mhz, &l_clock);
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
}

void znx_front_power_end(void)
{
    restore_clock();
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
}

size_t znx_front_tail(char* out, size_t size, unsigned lines)
{
    size_t used = 0;
    bool locked = lock_log();
    unsigned count = l_tail_count < TAIL_LINES ? l_tail_count : TAIL_LINES;
    if (lines < count)
        count = lines;
    if (size > 0)
        out[0] = '\0';
    for (unsigned i = l_tail_count - count; i != l_tail_count && used + 1 < size; ++i)
        used += (size_t)snprintf(out + used, size - used, "%s%s", used ? "\n" : "", l_tail[i % TAIL_LINES]);
    if (locked)
        mutexUnlock(&l_log_lock);
    return used < size ? used : size - 1;
}

/* ---- the emulation thread ---- */

struct run {
    const struct znx_host* host;
    const struct znx_options* options;
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
    run->result = znx_bench_run(run->host, run->options);
    znx_log("cpu clock: %u MHz measured on the emulation core after the run", znx_measure_cpu_mhz());
    run->done = 1;
}

static bool l_plus_stops = true;
void znx_front_plus_stops(bool on) { l_plus_stops = on; }

int znx_front_run(const struct znx_host* host, const struct znx_options* options, void (*tick)(void))
{
    static struct run run;
    Thread thread;
    Result rc;
    bool stopping = false;
    int stalled_polls = 0, stalled_frame = -1;
    /* Core 2 at the highest priority an application may use, as Melee-NX-NetPlay runs its
     * simulation thread; lower priorities if the kernel refuses. */
    static const int priorities[] = { 0x1C, 0x28, 0x2C };

    run.host = host;
    run.options = options;
    run.done = 0;
    run.result = 1;
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
    if (R_FAILED(rc)) {
        znx_log("thread: cannot start the emulation thread: 0x%08x", rc);
        return 1;
    }

    while (!run.done) {
        svcSleepThread(50000000ll);
        padUpdate(&l_pad);
        if (!stopping && (!appletMainLoop() || (l_plus_stops && (padGetButtonsDown(&l_pad) & HidNpadButton_Plus)))) {
            znx_log("%s: stop requested at frame %d", l_program, znx_bench_frame());
            znx_bench_stop();
            stopping = true;
        }
        if (tick != NULL)
            tick();
        if (l_console)
            consoleUpdate(NULL);
        /* The same frame over the limit on four polls in a row: a frame that was merely in
         * flight while the console slept or the program was suspended ends within the first. */
        if (znx_bench_stall_ns() > STALL_LIMIT_NS && znx_bench_frame() == stalled_frame) {
            if (++stalled_polls >= 4) {
                znx_log("%s: WATCHDOG: frame %d has been running for more than 10 s; giving up",
                        l_program, znx_bench_frame());
                hard_exit();
            }
        } else {
            stalled_polls = 0;
            stalled_frame = znx_bench_frame();
        }
    }
    threadWaitForExit(&thread);
    threadClose(&thread);
    return run.result;
}

/* ---- end ---- */

static void wait_for_plus(void)
{
    for (int i = 0; i < 2400 && appletMainLoop(); ++i) {
        padUpdate(&l_pad);
        if (padGetButtonsDown(&l_pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
        svcSleepThread(50000000ll);
    }
}

void znx_front_end(int exit_code, bool show_log)
{
    restore_clock();
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    znx_log("%s: %s", l_program, exit_code == 0 ? "done" : "finished with errors");

    if (l_console) {
        /* Nobody is reading over nxlink: leave the result on the screen for a while. */
        printf("\nThe same text is in the log in " ZNX_BASE_DIR "\nPress + to leave (or wait two minutes).\n");
        wait_for_plus();
        consoleExit(NULL);
        l_console = false;
    } else if (!l_text_screen && !l_nxlink && show_log) {
        /* The picture is gone (the video plugin has closed its window): say what happened. */
        unsigned count = l_tail_count < TAIL_LINES ? l_tail_count : TAIL_LINES;
        consoleInit(NULL);
        for (unsigned i = l_tail_count - count; i != l_tail_count; ++i)
            printf("%s\n", l_tail[i % TAIL_LINES]);
        printf("\nThe whole log is in " ZNX_BASE_DIR "\nPress + to leave (or wait two minutes).\n");
        wait_for_plus();
        consoleExit(NULL);
    }

    mutexLock(&l_log_lock);
    if (l_log != NULL) {
        fclose(l_log);
        l_log = NULL;
    }
    mutexUnlock(&l_log_lock);
    if (l_have_romfs)
        romfsExit();
    socketExit();
}
