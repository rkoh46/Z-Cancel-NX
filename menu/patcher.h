// SPDX-License-Identifier: GPL-3.0-or-later
/* Building Smash Remix or Remix +EXTRA on the console, on a thread of its own:
 *
 *   1. download (only if the patch is not in patches/): the release zip from GitHub over HTTPS
 *      (libcurl: the system's TLS service on the Switch), saved as patches/<zip>.part; its size
 *      and MD5 must be the release's; the .xdelta alone is taken out of it (minizip) into
 *      patches/, checked again (size, MD5), and the zip is deleted;
 *   2. build: the vanilla ROM read into memory in big-endian order and its MD5 checked, the patch's
 *      MD5 checked, then xdelta3 writes roms/<name>.part while the MD5 of what it writes is taken;
 *   3. check: that MD5 and the size must be the ones RMG-K's ROM database has, or the file is
 *      deleted; otherwise it is renamed to roms/<name> and remembered (catalog.h).
 *
 * The menu reads the progress fields from its own thread; they are plain words written by one
 * thread and read by the other, good enough for a progress bar. */
#ifndef ZNX_PATCHER_H
#define ZNX_PATCHER_H

#include <stdint.h>

enum znx_job_step { ZNX_STEP_DOWNLOAD, ZNX_STEP_BUILD, ZNX_STEP_CHECK, ZNX_STEP_COUNT };
enum znx_job_status { ZNX_JOB_RUNNING, ZNX_JOB_DONE, ZNX_JOB_FAILED, ZNX_JOB_CANCELLED };

struct znx_job {
    /* in */
    int game;
    char base[256];
    char vanilla_path[512];
    char patch_path[512];      /* "" = download it */
    /* progress, written by the job */
    volatile int step;         /* enum znx_job_step */
    volatile int status;       /* enum znx_job_status */
    volatile uint64_t done, total; /* bytes of the current step */
    volatile int cancel;       /* set by the menu */
    int downloads;             /* 1 if the job includes step 1 */
    /* out */
    char error_title[96];
    char error_text[512];
    char result_path[512];
    char result_md5[33];
    void* thread;              /* a pthread_t, allocated */
};

/* Starts the job (job's "in" fields filled). 0 if the thread could not be started (error_* set). */
int znx_job_start(struct znx_job* job);
/* Waits for the thread after status left ZNX_JOB_RUNNING. */
void znx_job_finish(struct znx_job* job);

/* The work itself, on the calling thread; what znx_job_start runs. Also used by the Mac preview. */
void znx_job_run(struct znx_job* job);

/* Free space on the volume holding path, in bytes; 0 if unknown. */
uint64_t znx_free_space(const char* path);

#endif
