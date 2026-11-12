// SPDX-License-Identifier: GPL-3.0-or-later
/* CPU clock for the speed test: ask for the console's highest stock CPU clock while the
 * measurement runs, the way RetroArch and other homebrew do through the clkrst service
 * (8.0.0+; pcv before that), and measure what the core really runs at. sys-clk, when
 * installed, overrides whatever is set here.
 *
 * Adapted from Melee-NX-NetPlay's switch/src/switch_clocks.cpp (GPL-3.0-or-later, same
 * author): the rate request and the dependent-add measurement, reduced to the CPU clock
 * and to C. */

#include <switch.h>

#include "znx_switch.h"

static bool l_clkrst, l_pcv;

static bool rate(bool set, uint32_t hz, uint32_t* actual_hz)
{
    Result rc = 0;
    uint32_t got = 0;
    if (l_clkrst) {
        ClkrstSession session;
        if (R_FAILED(clkrstOpenSession(&session, PcvModuleId_CpuBus, 3)))
            return false;
        if (set)
            rc = clkrstSetClockRate(&session, hz);
        clkrstGetClockRate(&session, &got);
        clkrstCloseSession(&session);
    } else if (l_pcv) {
        if (set)
            rc = pcvSetClockRate(PcvModule_CpuBus, hz);
        pcvGetClockRate(PcvModule_CpuBus, &got);
    } else {
        return false;
    }
    if (actual_hz != NULL)
        *actual_hz = got;
    return R_SUCCEEDED(rc);
}

void znx_clocks_begin(uint32_t cpu_mhz, struct znx_clock_state* state)
{
    *state = (struct znx_clock_state){ 0 };
    if (hosversionAtLeast(8, 0, 0))
        l_clkrst = R_SUCCEEDED(clkrstInitialize());
    else
        l_pcv = R_SUCCEEDED(pcvInitialize());
    state->available = l_clkrst || l_pcv;
    if (!state->available)
        return;
    rate(false, 0, &state->before_hz);
    state->after_hz = state->before_hz;
    if (cpu_mhz != 0) {
        state->requested_hz = cpu_mhz * 1000000u;
        state->request_ok = rate(true, state->requested_hz, &state->after_hz);
    }
}

void znx_clocks_end(const struct znx_clock_state* state)
{
    if (state->available && state->requested_hz != 0 && state->before_hz != 0)
        rate(true, state->before_hz, NULL);
    if (l_clkrst)
        clkrstExit();
    if (l_pcv)
        pcvExit();
    l_clkrst = l_pcv = false;
}

/* The CPU clock the calling core really runs at: 32 M dependent adds (one cycle each on
 * the A57) timed against the 19.2 MHz system counter, about 16 ms at 2 GHz. The service
 * reports the rate it was asked for; this shows whether the core got it. */
uint32_t znx_measure_cpu_mhz(void)
{
    uint64_t x = 0;
    const uint64_t t0 = armGetSystemTick();
    for (int i = 0; i < 1 << 18; ++i) {
#define ADD8 "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
        __asm__ volatile(ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 : "+r"(x));
#undef ADD8
    }
    const uint64_t ticks = armGetSystemTick() - t0;
    return ticks ? (uint32_t)(x * armGetSystemTickFreq() / ticks / 1000000u) : 0;
}
