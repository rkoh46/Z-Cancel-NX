// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-bench for the Nintendo Switch: the speed test of docs/PLAN.md step 1 as a homebrew
 * NRO. The measurement is bench/bench_core.c, the same code the Mac harness runs; the
 * console around it (log, emulation thread, watchdog) is front_switch.c, shared with the
 * program that shows a picture (game_switch.c).
 *
 * Files, all under sdmc:/switch/z-cancel-nx/:
 *   rom.z64      the ROM (or pass a path as the NRO's first argument)
 *   bench.ini    optional: frames=7200  hash_every=600  rollback_test=5400,5460
 *                rollback_stress=K,D  inputs=<file>  cpu_mhz=1785 (0 leaves the clock alone)  verbose=1
 *   bench.log    every line of output, flushed line by line
 *
 * Lines also go to a PC running nxlink (-s) when the NRO was sent with it, and to the
 * screen when it was not. Plus stops the run early. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "bench_core.h"
#include "front_switch.h"
#include "znx_switch.h"

struct settings {
    struct znx_options options;
    char inputs_path[512];
    uint32_t cpu_mhz;
};

static bool ini_key(const char* key, const char* value, void* user)
{
    struct settings* s = user;
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
        else snprintf(s->inputs_path, sizeof(s->inputs_path), ZNX_BASE_DIR "/%s", value);
    }
    else if (!strcmp(key, "cpu_mhz")) s->cpu_mhz = (uint32_t)atoi(value);
    else if (!strcmp(key, "rollback_stress")) {   /* rollback_stress=K,D */
        int k, d;
        if (sscanf(value, "%d ,%d", &k, &d) == 2 && k > d && d > 0) { s->options.stress_every = k; s->options.stress_depth = d; s->options.rb_a = s->options.rb_b = -1; }
    }
    else if (!strcmp(key, "verbose")) s->options.verbose = atoi(value);
    else return false;
    return true;
}

int main(int argc, char** argv)
{
    static struct settings settings;
    static struct znx_host host;
    bool have_romfs;
    int exit_code = 1;

    znx_front_begin("znx-bench", "bench.log", true);
    have_romfs = znx_front_romfs();

    znx_options_defaults(&settings.options);
    settings.options.frames = 7200;
    settings.options.hash_every = 600;
    settings.options.rb_a = 5400;
    settings.options.rb_b = 5460;
    settings.options.progress_every = 600;
    settings.cpu_mhz = 1785; /* the highest stock CPU clock of the first Switch model */
    znx_front_read_ini("bench.ini", ini_key, &settings);
    if (settings.options.frames <= 0)
        settings.options.frames = 7200;
    if (settings.options.rb_a >= 0 && settings.options.rb_b > settings.options.frames) {
        znx_log("settings: rollback test %d..%d is past the last frame %d: skipped",
                settings.options.rb_a, settings.options.rb_b, settings.options.frames);
        settings.options.rb_a = settings.options.rb_b = -1;
    }
    settings.options.inputs_path = settings.inputs_path[0] ? settings.inputs_path : NULL;
    znx_log("settings: frames %d, hash_every %d, rollback_test %d,%d, inputs %s, cpu_mhz %u",
            settings.options.frames, settings.options.hash_every, settings.options.rb_a, settings.options.rb_b,
            settings.options.inputs_path ? settings.options.inputs_path : "none", (unsigned)settings.cpu_mhz);
    settings.options.rom_path = znx_front_find_rom(argc, argv);

    znx_front_power_begin(settings.cpu_mhz);

    host.core = znx_static_lib("core");
    host.video = znx_static_lib("video_null");
    host.rsp = znx_static_lib("rsp_hle");
    host.sym = osal_dynlib_getproc;
    host.print = znx_front_print;
    host.data_dir = "romfs:/data";
    host.user_dir = ZNX_BASE_DIR "/profile";

    if (settings.options.rom_path == NULL)
        ;
    else if (host.core == NULL || host.video == NULL || host.rsp == NULL || !have_romfs)
        znx_log("znx-bench: this build is incomplete (a linked library or the romfs is missing)");
    else
        exit_code = znx_front_run(&host, &settings.options, NULL);

    znx_front_end(exit_code, false);
    return 0;
}
