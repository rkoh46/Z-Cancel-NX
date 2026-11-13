// SPDX-License-Identifier: GPL-3.0-or-later
/* Self-test of the Switch build recipe on Linux ARM64 (builder/build-switch.sh --selftest).
 *
 * Everything that is not libnx is the Switch build's own: the core compiled with
 * __SWITCH__, the plugins linked in and found through switch/dynlib_static.c, the code
 * cache reserved in .text by switch/jit_memory.S, bench/bench_core.c. This file stands in
 * for libnx and for switch/main_switch.c:
 *   - znx_jit_map: a second, writable view of the cache. Linux cannot alias program text,
 *     so the same memfd is mapped twice: executable over znx_jit_memory, writable elsewhere.
 *     The recompiler sees what it sees on the console: it executes inside the image and
 *     writes through another address.
 *   - armDCacheFlush / armICacheInvalidate: the instructions libnx's versions execute.
 *   - posix_memalign: with ZNX_COMPACT=1 in the environment it refuses the core's 512 MB
 *     block, as the console does when the program runs as an applet, so the core's compact
 *     memory layout is exercised too.
 * It cannot test Horizon itself (svcMapProcessMemory, threads, clocks, files).
 *
 * FRAMES may be written NxR ("3600x2"): the whole run R times in this process, core shut down and
 * started again each time, as the console program does when it returns to its start screen after a
 * game; every run must print the same hashes. */

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bench_core.h"
#include "znx_switch.h"

void znx_log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}

static void print_line(int is_error, const char* line)
{
    (void)is_error;
    puts(line);
}

int posix_memalign(void** result, size_t alignment, size_t size)
{
    void* block = size >= (64u << 20) && getenv("ZNX_COMPACT") != NULL ? NULL : memalign(alignment, size);
    if (size >= (64u << 20))
        znx_log("memory: the core's %u MB block was %s", (unsigned)(size >> 20),
                block != NULL ? "allocated (full memory layout)" : "refused (compact memory layout)");
    if (block == NULL)
        return ENOMEM;
    *result = block;
    return 0;
}

int znx_jit_map(size_t size, void** rw, void** rx)
{
    int fd = memfd_create("znx-jit", 0);
    void* exec_view;
    void* write_view;
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0 || ((uintptr_t)znx_jit_memory & (uintptr_t)(sysconf(_SC_PAGESIZE) - 1)) != 0)
        return 0;
    exec_view = mmap(znx_jit_memory, size, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, fd, 0);
    write_view = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (exec_view == MAP_FAILED || write_view == MAP_FAILED)
        return 0;
    *rw = write_view;
    *rx = exec_view;
    znx_log("jit: code cache executes at %p, written through %p (%u MB)", *rx, *rw, (unsigned)(size >> 20));
    return 1;
}

void znx_jit_unmap(void* rw, void* rx, size_t size)
{
    (void)rx; /* stays mapped: it is part of the program's text range */
    munmap(rw, size);
}

void armDCacheFlush(void* address, size_t size)
{
    uint64_t ctr, line, a, end = (uint64_t)address + size;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = 4u << ((ctr >> 16) & 0xf);
    for (a = (uint64_t)address & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

void armICacheInvalidate(void* address, size_t size)
{
    uint64_t ctr, line, a, end = (uint64_t)address + size;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = 4u << (ctr & 0xf);
    for (a = (uint64_t)address & ~(line - 1); a < end; a += line)
        __asm__ volatile("ic ivau, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy\nisb" : : : "memory");
}

int main(int argc, char** argv)
{
    struct znx_options options;
    struct znx_host host;
    char exe[4096], data_dir[4200], user_dir[4200], warm_dir[4200];
    int args = 1;
    ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char* slash;

    if (argc < 2 || length <= 0) {
        fprintf(stderr, "usage: znx-selftest ROM [FRAMES[xRUNS] [ROLLBACK_A ROLLBACK_B]] [key=value ...]\n"
                        "  precompile_ms=N warm=0 warm_flags=N vs=STAGE:F1,F2,F3,F4 stress=K,D hash_every=K\n");
        return 2;
    }
    exe[length] = '\0';
    slash = strrchr(exe, '/');
    *slash = '\0';
    snprintf(data_dir, sizeof(data_dir), "%s/data", exe);
    snprintf(user_dir, sizeof(user_dir), "%s/profile", exe);
    mkdir(user_dir, 0755);
    setvbuf(stdout, NULL, _IOLBF, 0);

    znx_options_defaults(&options);
    options.hash_every = 600;
    options.progress_every = 600;
    /* key=value arguments (the warm list and what exercises it) are taken out; the rest is positional */
    for (int i = 1; i < argc; ++i) {
        const char* value = strchr(argv[i], '=');
        if (i == 1 || value == NULL) { argv[args++] = argv[i]; continue; }
        value++;
        if (!strncmp(argv[i], "precompile_ms=", 14)) options.precompile_ms = atoi(value);
        else if (!strncmp(argv[i], "precompile_big=", 15)) options.precompile_big = atoi(value);
        else if (!strncmp(argv[i], "warm=", 5)) options.warm_off = atoi(value) == 0;
        else if (!strncmp(argv[i], "warm_flags=", 11)) options.warm_flags = atoi(value);
        else if (!strncmp(argv[i], "hash_every=", 11)) options.hash_every = atoi(value);
        else if (!strncmp(argv[i], "stress=", 7)) sscanf(value, "%d,%d", &options.stress_every, &options.stress_depth);
        else if (!strncmp(argv[i], "vs=", 3)) {
            const char* list = strchr(value, ':');
            options.vs_stage = (int)strtol(value, NULL, 0);
            for (int port = 0; port < 4 && list != NULL; ++port) {
                options.vs_fighters[port] = (int)strtol(list + 1, NULL, 0);
                list = strchr(list + 1, ',');
            }
        }
    }
    argc = args;
    options.rom_path = argv[1];
    options.frames = argc > 2 ? atoi(argv[2]) : 3600;
    if (argc > 4) {
        options.rb_a = atoi(argv[3]);
        options.rb_b = atoi(argv[4]);
    }

    memset(&host, 0, sizeof(host));
    host.core = znx_static_lib("core");
    host.video = znx_static_lib("video_null");
    host.rsp = znx_static_lib("rsp_hle");
    host.sym = osal_dynlib_getproc;
    host.print = print_line;
    host.data_dir = data_dir;
    host.user_dir = user_dir;
    snprintf(warm_dir, sizeof(warm_dir), "%s/data/warm", exe);
    host.warm_dir = warm_dir;
    if (host.core == NULL || host.video == NULL || host.rsp == NULL) {
        fprintf(stderr, "znx-selftest: a linked library is missing\n");
        return 1;
    }
    {
        const char* runs_text = argc > 2 ? strchr(argv[2], 'x') : NULL;
        int runs = runs_text != NULL ? atoi(runs_text + 1) : 1, result = 0;
        for (int run = 0; run < runs && result == 0; ++run) {
            if (runs > 1)
                printf("run %d of %d\n", run + 1, runs);
            result = znx_bench_run(&host, &options);
        }
        return result;
    }
}
