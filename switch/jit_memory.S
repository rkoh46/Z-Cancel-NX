// SPDX-License-Identifier: GPL-3.0-or-later
/* The recompiler's code cache: 32 MB of the program's own .text.
 *
 * Horizon never gives a process a page that is writable and executable at once. Code that
 * is part of the loaded program is executable, and svcMapProcessMemory can map a second,
 * writable view of it (jit_switch.c). So the cache is reserved here, inside .text, the way
 * mupen64plus-libretro-nx does it (its jit_memory array), and new_dynarec runs in its
 * DOUBLE_CACHE_ADDR mode: it writes through the view and executes here.
 *
 * Being in .text also keeps the generated code within reach of the C helpers (direct
 * branches, +/-128 MB) and of the device state (adrp, +/-4 GB) wherever the NRO is loaded;
 * builder/build-switch.sh checks the image size against the branch range.
 *
 * It is in an assembly file because a C array cannot be given an executable section
 * without a section-type conflict, and zero-filled because an NRO's code segment has no
 * bss: the file carries the 32 MB. */

    .section .text.znx_jit_memory, "ax", %progbits
    .balign 4096
    .global znx_jit_memory
    .hidden znx_jit_memory
    .type   znx_jit_memory, %object
znx_jit_memory:
    .space  33554432
    .size   znx_jit_memory, . - znx_jit_memory
