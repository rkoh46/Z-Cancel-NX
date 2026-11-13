// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-bench for macOS: headless speed and determinism harness around RMG-K's mupen64plus
 * core. This file is the Mac front: command line, and the core and plugins loaded the way
 * any mupen64plus front end loads them (dlopen). The measurement itself is bench_core.c,
 * shared with the Switch build (switch/main_switch.c).
 *
 * Output (stdout):
 *   hash <frame> <fnv1a64 of the 8 MB RDRAM>     every --hash-every frames
 *   watch <frame> <address> <byte>               with --watch ADDR, when that RDRAM byte changes
 *   rollback ...                                 with --rollback-test A B
 *   frames/wall/fps, window ...                  at the end
 */

#include <dlfcn.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bench_core.h"

static void print_line(int is_error, const char* line)
{
    fprintf(is_error ? stderr : stdout, "%s\n", line);
}

static void* load(const char* dir, const char* name, int flags)
{
    char path[4200];
    void* lib;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    lib = dlopen(path, flags);
    if (lib == NULL)
        fprintf(stderr, "znx-bench: %s\n", dlerror());
    return lib;
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
        "  --rollback-stress K D  every K frames rewind the last D frames and re-run them, comparing every frame\n"
        "  --cpu MODE           2 dynamic recompiler in a rollback session, what RMG-K runs (default);\n"
        "                       0 pure or 1 cached interpreter, run without the rollback session\n"
        "  --dump-ram FRAME FILE  write the 8 MB RDRAM after FRAME frames (host word order). It holds\n"
        "                       game data: keep the file out of the repository\n"
        "  --watch ADDR         print \"watch <frame> <addr> <byte>\" when that RDRAM byte changes (hex address)\n");
    exit(2);
}

int main(int argc, char** argv)
{
    char exe[4096], dir[4096], data_dir[4200], user_dir[4200];
    uint32_t exe_size = sizeof(exe);
    struct znx_options o;
    struct znx_host host;
    int result;

    znx_options_defaults(&o);
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--rom") && i + 1 < argc) o.rom_path = argv[++i];
        else if (!strcmp(a, "--frames") && i + 1 < argc) o.frames = atoi(argv[++i]);
        else if (!strcmp(a, "--inputs") && i + 1 < argc) o.inputs_path = argv[++i];
        else if (!strcmp(a, "--hash-every") && i + 1 < argc) o.hash_every = atoi(argv[++i]);
        else if (!strcmp(a, "--cpu") && i + 1 < argc) o.emumode = atoi(argv[++i]);
        else if (!strcmp(a, "--rollback-test") && i + 2 < argc) { o.rb_a = atoi(argv[++i]); o.rb_b = atoi(argv[++i]); }
        else if (!strcmp(a, "--rollback-stress") && i + 2 < argc) { o.stress_every = atoi(argv[++i]); o.stress_depth = atoi(argv[++i]); }
        else if (!strcmp(a, "--watch") && i + 1 < argc) o.watch = strtol(argv[++i], NULL, 16) & 0x7fffff;
        else if (!strcmp(a, "--dump-ram") && i + 2 < argc) { o.dump_frame = atoi(argv[++i]); o.dump_path = argv[++i]; }
        else if (!strcmp(a, "--verbose")) o.verbose = 1;
        else usage();
    }
    if (o.rom_path == NULL || o.frames <= 0)
        usage();
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* The libraries, the core's data files and a scratch profile all sit next to the program. */
    if (_NSGetExecutablePath(exe, &exe_size) != 0 || realpath(exe, dir) == NULL) {
        fprintf(stderr, "znx-bench: cannot find my own path\n");
        return 1;
    }
    strcpy(dir, dirname(dir));
    snprintf(data_dir, sizeof(data_dir), "%s/data", dir);
    snprintf(user_dir, sizeof(user_dir), "%s/profile", dir);
    mkdir(user_dir, 0755);

    memset(&host, 0, sizeof(host));
    host.core = load(dir, "libmupen64plus.dylib", RTLD_NOW | RTLD_GLOBAL);
    host.video = load(dir, "znx-video-null.dylib", RTLD_NOW);
    host.rsp = load(dir, "mupen64plus-rsp-hle.dylib", RTLD_NOW);
    if (host.core == NULL || host.video == NULL || host.rsp == NULL)
        return 1;
    host.sym = dlsym;
    host.print = print_line;
    host.data_dir = data_dir;
    host.user_dir = user_dir;

    result = znx_bench_run(&host, &o);

    dlclose(host.rsp);
    dlclose(host.video);
    dlclose(host.core);
    return result;
}
