// SPDX-License-Identifier: GPL-3.0-or-later
/* See stall.h. */

#include "stall.h"

const char* znx_stall_phase_name(enum znx_stall_phase phase)
{
    switch (phase) {
    case ZNX_STALL_INPUT: return "reading the controllers";
    case ZNX_STALL_APPLET: return "waiting on the system";
    case ZNX_STALL_UPDATE: return "the menu or lobby update";
    case ZNX_STALL_DRAW: return "drawing";
    case ZNX_STALL_SWAP: return "showing the frame (GPU)";
    case ZNX_STALL_KEYBOARD: return "the keyboard";
    default: return "?";
    }
}

enum znx_stall_verdict znx_stall_poll(struct znx_stall* s, unsigned beat, enum znx_stall_phase phase)
{
    if (beat != s->beat || phase == ZNX_STALL_APPLET || phase == ZNX_STALL_KEYBOARD) {
        s->beat = beat;
        s->polls = 0;
        return ZNX_STALL_OK;
    }
    s->polls++;
    /* A swap that never returns is a GPU or display that stopped: nothing in the program will
     * free it, and the console stays frozen until it is reset. Leaving may let the system recover. */
    if (phase == ZNX_STALL_SWAP && s->polls >= ZNX_STALL_GIVE_UP_POLLS)
        return ZNX_STALL_GIVE_UP;
    if (s->polls == ZNX_STALL_REPORT_POLLS ||
        (s->polls > ZNX_STALL_REPORT_POLLS && (s->polls - ZNX_STALL_REPORT_POLLS) % ZNX_STALL_REPEAT_POLLS == 0))
        return ZNX_STALL_REPORT;
    return ZNX_STALL_OK;
}
