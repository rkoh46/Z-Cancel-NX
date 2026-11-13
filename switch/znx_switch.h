// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared between the files of the Switch front (switch/). */
#ifndef ZNX_SWITCH_H
#define ZNX_SWITCH_H

#include <stddef.h>
#include <stdint.h>

/* One line to nxlink (or the on-screen console) and to the log file; printf format (front_switch.c). */
void znx_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

/* jit_switch.c */
#define ZNX_JIT_SIZE 33554432u
extern char znx_jit_memory[ZNX_JIT_SIZE];
int znx_jit_map(size_t size, void** rw, void** rx);
void znx_jit_unmap(void* rw, void* rx, size_t size);

/* dynlib_static.c: the handles CoreAttachPlugin takes, by name ("core", "rsp_hle",
 * "video_null", "gliden64"), and the lookup that replaces dlsym. */
void* znx_static_lib(const char* name);
void* osal_dynlib_getproc(void* lib, const char* name);

/* clocks_switch.c */
struct znx_clock_state {
    int available;          /* clkrst or pcv could be opened */
    uint32_t before_hz;     /* CPU rate the system reported at start */
    uint32_t requested_hz;  /* 0 = left alone */
    int request_ok;
    uint32_t after_hz;      /* reported after the request */
};
void znx_clocks_begin(uint32_t cpu_mhz, struct znx_clock_state* state);
void znx_clocks_end(const struct znx_clock_state* state); /* puts the first rate back */
uint32_t znx_measure_cpu_mhz(void);

#endif
