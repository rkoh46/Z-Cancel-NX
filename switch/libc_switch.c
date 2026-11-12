// SPDX-License-Identifier: GPL-3.0-or-later
/* What the core expects from a POSIX libc and devkitA64's newlib does not have. */

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>

#include "znx_switch.h"

/* The core asks for a 512 MB block to lay the console's memory out at fixed offsets
 * (init_mem_base, src/device/memory/memory.c) and, when refused, falls back to a compact
 * layout of a few megabytes. Horizon commits the whole heap up front, so the big block only
 * fits when the program runs as an application (started over a game); as an applet
 * (started from the Album) the compact layout is used. Both are the core's own code paths
 * and emulate the same machine; the line says which one this run got. */
int posix_memalign(void** result, size_t alignment, size_t size)
{
    void* block = memalign(alignment, size);
    if (size >= (64u << 20))
        znx_log("memory: the core's %u MB block was %s", (unsigned)(size >> 20),
                block != NULL ? "allocated (full memory layout)" : "refused (compact memory layout)");
    if (block == NULL)
        return ENOMEM;
    *result = block;
    return 0;
}
