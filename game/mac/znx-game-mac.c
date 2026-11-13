// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-game for macOS: the picture build's plumbing checked without a console and without a
 * window. The same pieces as the Switch program (switch/game_switch.c): bench_core.c's
 * frame driver, game_core.c's GLideN64 settings and report, RMG-K's GLideN64 with the
 * patches of patches/, loaded here as a dynamic library and drawing into an offscreen
 * OpenGL 4.1 context (vidext_cgl.c). It answers two questions: does GLideN64 start and
 * draw through this front end (--shot), and does it leave RDRAM alone (the "hash" lines
 * must equal znx-bench's).
 *
 *   znx-game --rom FILE --frames N [--inputs FILE] [--hash-every K] [--scale S]
 *            [--shot FRAME FILE.png]... [--verbose]
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
#include "game_core.h"
#include "vidext_cgl.h"

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
        fprintf(stderr, "znx-game: %s\n", dlerror());
    return lib;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: znx-game --rom FILE --frames N [--inputs FILE] [--hash-every K] [--scale S]\n"
        "                [--shot FRAME FILE.png]... [--verbose]\n"
        "  --hash-every K   \"hash <frame> <fnv1a64 of RDRAM>\" every K frames (default 600), as znx-bench prints\n"
        "  --scale S        0 (default): no frame buffer emulation; 1, 2: on, at that multiple of the N64's resolution\n"
        "  --shot FRAME F   save the picture shown after FRAME frames as a PNG. It is the game's\n"
        "                   picture: keep the file out of the repository\n");
    exit(2);
}

int main(int argc, char** argv)
{
    char exe[4096], dir[4096], data_dir[4200], user_dir[4200];
    uint32_t exe_size = sizeof(exe);
    struct znx_options o;
    struct znx_host host;
    struct znx_game_settings game = { 1280, 720, 0, 0, 600 };
    static const struct znx_game_video video = { znx_cgl_take_swap_ns, znx_cgl_take_swaps };
    int result;

    znx_options_defaults(&o);
    o.hash_every = 600;
    o.window_count = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--rom") && i + 1 < argc) o.rom_path = argv[++i];
        else if (!strcmp(a, "--frames") && i + 1 < argc) o.frames = atoi(argv[++i]);
        else if (!strcmp(a, "--inputs") && i + 1 < argc) o.inputs_path = argv[++i];
        else if (!strcmp(a, "--hash-every") && i + 1 < argc) o.hash_every = atoi(argv[++i]);
        else if (!strcmp(a, "--scale") && i + 1 < argc) game.scale = atoi(argv[++i]);
        else if (!strcmp(a, "--shot") && i + 2 < argc) { int frame = atoi(argv[++i]); znx_cgl_shot(frame, argv[++i]); }
        else if (!strcmp(a, "--verbose")) o.verbose = 1;
        else usage();
    }
    if (o.rom_path == NULL || o.frames <= 0)
        usage();
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* The libraries, the core's data files and a scratch profile all sit next to the program. */
    if (_NSGetExecutablePath(exe, &exe_size) != 0 || realpath(exe, dir) == NULL) {
        fprintf(stderr, "znx-game: cannot find my own path\n");
        return 1;
    }
    strcpy(dir, dirname(dir));
    snprintf(data_dir, sizeof(data_dir), "%s/data", dir);
    snprintf(user_dir, sizeof(user_dir), "%s/profile-game", dir);
    mkdir(user_dir, 0755);

    memset(&host, 0, sizeof(host));
    host.name = "znx-game";
    host.core = load(dir, "libmupen64plus.dylib", RTLD_NOW | RTLD_GLOBAL);
    host.video = load(dir, "mupen64plus-video-GLideN64.dylib", RTLD_NOW);
    host.rsp = load(dir, "mupen64plus-rsp-hle.dylib", RTLD_NOW);
    if (host.core == NULL || host.video == NULL || host.rsp == NULL)
        return 1;
    host.sym = dlsym;
    host.print = print_line;
    host.data_dir = data_dir;
    host.user_dir = user_dir;
    host.vidext = znx_cgl_functions();
    host.video_config = znx_game_video_config;
    host.frame_done = znx_game_frame_done;

    znx_cgl_begin(print_line);
    znx_game_begin(&game, &video, print_line);
    result = znx_bench_run(&host, &o);

    dlclose(host.rsp);
    dlclose(host.video);
    dlclose(host.core);
    return result;
}
