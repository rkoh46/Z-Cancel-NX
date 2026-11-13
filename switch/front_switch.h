// SPDX-License-Identifier: GPL-3.0-or-later
/* The console around a run of bench/bench_core.c, shared by the two Switch programs
 * (main_switch.c: znx-bench; game_switch.c: Z-Cancel NX with a picture): the log, the
 * settings file, the emulation thread with its watchdog, and leaving. */
#ifndef ZNX_FRONT_SWITCH_H
#define ZNX_FRONT_SWITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bench_core.h"

#define ZNX_BASE_DIR "sdmc:/switch/z-cancel-nx"

/* Start of main(): the log lock, sockets and nxlink, the pads, the folders, the log file
 * (ZNX_BASE_DIR/<log_name>) and the first two lines of output. With text_screen, lines go
 * to the on-screen console when no PC is listening over nxlink; a program that draws its
 * own picture passes false and can show its last lines afterwards (znx_front_end). */
void znx_front_begin(const char* program, const char* log_name, bool text_screen);

/* The NRO's data (the core's ROM database, romfs:/data). False, with a line, if it cannot be opened. */
bool znx_front_romfs(void);

/* One finished line to the log, nxlink or the screen: znx_host.print. */
void znx_front_print(int is_error, const char* line);

bool znx_file_exists(const char* path);

/* Calls key(name, value, user) for every "name = value" line of ZNX_BASE_DIR/<ini_name>;
 * key returns false for a name it does not know. False if there is no such file. */
bool znx_front_read_ini(const char* ini_name, bool (*key)(const char* name, const char* value, void* user), void* user);

/* The ROM: ZNX_BASE_DIR/rom.z64, else argv[1]. NULL, with a line, if neither exists. */
const char* znx_front_find_rom(int argc, char** argv);

/* Keeps the console awake, asks for the CPU clock (0 leaves it alone) and reports it. */
void znx_front_power_begin(uint32_t cpu_mhz);

/* After a run, when the program goes back to its start screen: the clock back to what the system
 * had, auto-sleep allowed again. znx_front_end does the same at the end of the program. */
void znx_front_power_end(void);

/* The last `lines` lines of output (at most 40), one per line, into out. Returns the length. */
size_t znx_front_tail(char* out, size_t size, unsigned lines);

/* Runs znx_bench_run(host, options) on the emulation thread (core 2) and watches it from
 * this one: Plus on any controller or the system asking the program to close stops the run
 * at the next frame; a frame that takes more than ten seconds ends the program with a line
 * in the log. tick, if not NULL, is called about twenty times a second on this thread.
 * Returns znx_bench_run's result, or 1 if the thread could not be started. */
/* Plus on any controller stops a run by default. A program whose player uses Plus (the N64's Start) turns
 * that off and stops the run itself (znx_bench_stop). */
void znx_front_plus_stops(bool on);

int znx_front_run(const struct znx_host* host, const struct znx_options* options, void (*tick)(void));

/* End of main(): puts the clock back, prints "<program>: done" or "finished with errors",
 * shows the text screen until Plus (with text_screen; or, without it, when show_log is set
 * and nobody is listening over nxlink, the last lines of the log), closes everything. */
void znx_front_end(int exit_code, bool show_log);

#endif
