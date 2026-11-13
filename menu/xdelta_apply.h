// SPDX-License-Identifier: GPL-3.0-or-later
/* xdelta_apply.c: the VCDIFF decoder of xdelta3 (ref/xdelta, Apache-2.0) around a ROM in memory. */
#ifndef ZNX_XDELTA_APPLY_H
#define ZNX_XDELTA_APPLY_H

#include <stddef.h>
#include <stdint.h>

/* Decodes patch_path against source (the vanilla ROM in big-endian order) and writes the result to
 * out_path as it is produced, computing its MD5 on the way (out_md5, 32 hex digits) and its size.
 * progress(user, patch bytes read, patch size) is called after every chunk of the patch; non-zero
 * from it cancels (returns 2). Returns 0 on success, 1 with a message in error otherwise. The
 * output file is left as it is on failure: the caller deletes it. */
int znx_xdelta_apply(const uint8_t* source, size_t source_size, const char* patch_path, const char* out_path,
                     char out_md5[33], uint64_t* out_size,
                     int (*progress)(void* user, uint64_t done, uint64_t total), void* user,
                     char* error, size_t error_size);

#endif
