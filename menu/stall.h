// SPDX-License-Identifier: GPL-3.0-or-later
/* The start screen's watchdog, the part without the console: decides from a heartbeat whether the
 * screen's loop has stopped. switch/menu_switch.c beats once per step of a frame (input, the system's
 * messages, update, draw, swap) and a thread polls this twice a second.
 *
 * Polls are counted, not time: while the console sleeps or the program is suspended on the HOME
 * menu the polling thread is suspended too, so that time never counts as a stall. */
#ifndef ZNX_STALL_H
#define ZNX_STALL_H

enum znx_stall_phase {
    ZNX_STALL_INPUT,
    ZNX_STALL_APPLET,    /* appletMainLoop: may wait on the system as long as it likes */
    ZNX_STALL_UPDATE,    /* the menu and the lobby (network) */
    ZNX_STALL_DRAW,
    ZNX_STALL_SWAP,      /* eglSwapBuffers: the GPU and the display */
    ZNX_STALL_KEYBOARD,  /* the system keyboard: the player is typing */
    ZNX_STALL_PHASES
};

enum znx_stall_verdict {
    ZNX_STALL_OK,
    ZNX_STALL_REPORT,    /* log where it is stuck */
    ZNX_STALL_GIVE_UP    /* stuck where waiting longer cannot help: leave the program */
};

struct znx_stall {
    unsigned beat;       /* the heartbeat at the last poll */
    int polls;           /* polls in a row that saw the same beat */
};

#define ZNX_STALL_REPORT_POLLS 10   /* 5 s at two polls a second */
#define ZNX_STALL_REPEAT_POLLS 60   /* then every 30 s */
#define ZNX_STALL_GIVE_UP_POLLS 30  /* 15 s inside the buffer swap */

const char* znx_stall_phase_name(enum znx_stall_phase phase);
enum znx_stall_verdict znx_stall_poll(struct znx_stall* s, unsigned beat, enum znx_stall_phase phase);

#endif
