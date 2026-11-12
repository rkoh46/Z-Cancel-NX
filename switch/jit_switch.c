// SPDX-License-Identifier: GPL-3.0-or-later
/* The writable view of the recompiler's code cache (see jit_memory.S).
 *
 * Mechanism ported from mupen64plus-libretro-nx (switch/mman.h: svcMapProcessMemory of the
 * jit_memory array), with the address chosen by the current libnx virtmem API. libnx's own
 * Jit object (switch/kernel/jit.h) was not used: it puts the code in freshly reserved
 * address space, which can be further than a direct branch (128 MB) from the C helpers the
 * generated code calls all the time, and its portable mode flips one mapping between
 * writable and executable, while the recompiler patches code that is executing. */

#include <switch.h>

#include "znx_switch.h"

static Handle own_process(void)
{
    Handle handle = envGetOwnProcessHandle();
    return handle != INVALID_HANDLE ? handle : CUR_PROCESS_HANDLE;
}

int znx_jit_map(size_t size, void** rw, void** rx)
{
    void* view;
    Result rc;

    if (size != ZNX_JIT_SIZE || ((uintptr_t)znx_jit_memory & 0xfff) != 0) {
        znx_log("jit: bad cache geometry (size %zu, at %p)", size, (void*)znx_jit_memory);
        return 0;
    }
    if (!envIsSyscallHinted(0x74))
        znx_log("jit: the loader does not list svcMapProcessMemory as available; trying anyway");

    /* The kernel wants the view inside the ASLR region; the lock keeps another thread from
     * picking the same hole between the search and the mapping. */
    virtmemLock();
    view = virtmemFindAslr(size, 0x1000);
    rc = view != NULL ? svcMapProcessMemory(view, own_process(), (u64)(uintptr_t)znx_jit_memory, size)
                      : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_FAILED(rc)) {
        znx_log("jit: svcMapProcessMemory failed: 0x%08x (module %u, description %u)", rc,
                R_MODULE(rc), R_DESCRIPTION(rc));
        return 0;
    }
    *rw = view;
    *rx = znx_jit_memory;
    znx_log("jit: code cache executes at %p, written through %p (%u MB)", *rx, *rw, (unsigned)(size >> 20));
    return 1;
}

void znx_jit_unmap(void* rw, void* rx, size_t size)
{
    Result rc = svcUnmapProcessMemory(rw, own_process(), (u64)(uintptr_t)rx, size);
    if (R_FAILED(rc))
        znx_log("jit: svcUnmapProcessMemory failed: 0x%08x", rc);
}
