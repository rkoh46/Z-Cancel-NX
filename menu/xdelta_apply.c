// SPDX-License-Identifier: GPL-3.0-or-later
/* See xdelta_apply.h.
 *
 * xdelta3 is one translation unit by design (xdelta3.c includes the rest), configured with
 * defines: the decoder only, no secondary compressors (the Smash Remix patches use none:
 * "VCDIFF secondary compressor: none" in xdelta3 printhdr for both), 64-bit sizes, no main().
 * The build passes -I ref/xdelta/xdelta3. The whole source is one block in memory, so xdelta3
 * never asks for source data (no XD3_GETSRCBLK). The patch is read 1 MB at a time and every
 * window (8 MB here) is written to the SD card as soon as it is decoded. */

#define SIZEOF_SIZE_T 8
#define SIZEOF_UNSIGNED_LONG 8
#define SIZEOF_UNSIGNED_LONG_LONG 8
#define SIZEOF_UNSIGNED_INT 4
#define XD3_USE_LARGEFILE64 1
#define XD3_USE_LARGESIZET 1
#define XD3_ENCODER 0
#define XD3_MAIN 0
#define XD3_DEBUG 0
#define REGRESSION_TEST 0
#define SECONDARY_DJW 0
#define SECONDARY_FGK 0
#define SECONDARY_LZMA 0
#define HAVE_CONFIG_H 0

#include "xdelta3.c"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "catalog.h"
#include "md5.h"
#include "xdelta_apply.h"

#define CHUNK (1u << 20)

int znx_xdelta_apply(const uint8_t* source, size_t source_size, const char* patch_path, const char* out_path,
                     char out_md5[33], uint64_t* out_size,
                     int (*progress)(void* user, uint64_t done, uint64_t total), void* user,
                     char* error, size_t error_size)
{
    xd3_stream stream;
    xd3_config config;
    xd3_source src;
    FILE* in = NULL;
    FILE* out = NULL;
    uint8_t* chunk = NULL;
    md5_state_t md5;
    md5_byte_t digest[16];
    struct stat info;
    uint64_t total = 0, read_so_far = 0, written = 0;
    int result = 1, ret, finished = 0, configured = 0;
    size_t got = 0;

    memset(&stream, 0, sizeof(stream));
    memset(&src, 0, sizeof(src));
    out_md5[0] = '\0';
    *out_size = 0;

    in = fopen(patch_path, "rb");
    if (in == NULL || stat(patch_path, &info) != 0) {
        snprintf(error, error_size, "Cannot open the patch %s: %s", patch_path, strerror(errno));
        goto done;
    }
    total = (uint64_t)info.st_size;
    out = fopen(out_path, "wb");
    if (out == NULL) {
        snprintf(error, error_size, "Cannot write %s: %s", out_path, strerror(errno));
        goto done;
    }
    chunk = malloc(CHUNK);
    if (chunk == NULL) {
        snprintf(error, error_size, "Not enough memory to apply the patch");
        goto done;
    }

    xd3_init_config(&config, 0);
    config.winsize = CHUNK;
    if ((ret = xd3_config_stream(&stream, &config)) != 0) {
        snprintf(error, error_size, "The patch decoder cannot start: %s", stream.msg ? stream.msg : xd3_strerror(ret));
        goto done;
    }
    configured = 1;
    src.blksize = (usize_t)source_size;
    src.onblk = (usize_t)source_size;
    src.curblk = source;
    src.curblkno = 0;
    if ((ret = xd3_set_source_and_size(&stream, &src, (xoff_t)source_size)) != 0) {
        snprintf(error, error_size, "The patch decoder refuses the ROM: %s", stream.msg ? stream.msg : xd3_strerror(ret));
        goto done;
    }
    md5_init(&md5);

    while (!finished) {
        got = fread(chunk, 1, CHUNK, in);
        if (ferror(in)) {
            snprintf(error, error_size, "Cannot read the patch %s", patch_path);
            goto done;
        }
        read_so_far += got;
        if (got < CHUNK)
            xd3_set_flags(&stream, XD3_FLUSH | stream.flags);
        xd3_avail_input(&stream, chunk, (usize_t)got);
        for (;;) {
            ret = xd3_decode_input(&stream);
            if (ret == XD3_INPUT) {
                if (got < CHUNK)
                    finished = 1;   /* everything given, nothing more wanted */
                break;
            }
            if (ret == XD3_OUTPUT) {
                if (fwrite(stream.next_out, 1, stream.avail_out, out) != stream.avail_out) {
                    snprintf(error, error_size, "Cannot write %s (is the SD card full?)", out_path);
                    goto done;
                }
                for (usize_t at = 0; at < stream.avail_out;) {
                    usize_t n = stream.avail_out - at > (1u << 30) ? (1u << 30) : stream.avail_out - at;
                    md5_append(&md5, stream.next_out + at, (unsigned)n);
                    at += n;
                }
                written += stream.avail_out;
                xd3_consume_output(&stream);
                continue;
            }
            if (ret == XD3_GOTHEADER || ret == XD3_WINSTART || ret == XD3_WINFINISH)
                continue;
            if (ret == XD3_GETSRCBLK) {   /* cannot happen with the whole source in one block */
                snprintf(error, error_size, "The patch asks for ROM data beyond the ROM's end");
                goto done;
            }
            snprintf(error, error_size, "The patch is damaged or does not fit this ROM (%s)",
                     stream.msg ? stream.msg : xd3_strerror(ret));
            goto done;
        }
        if (progress != NULL && progress(user, read_so_far, total) != 0) {
            result = 2;
            goto done;
        }
        if (got == 0)
            finished = 1;
    }
    if (fflush(out) != 0 || ferror(out)) {
        snprintf(error, error_size, "Cannot write %s (is the SD card full?)", out_path);
        goto done;
    }
    md5_finish(&md5, digest);
    znx_md5_hex(digest, out_md5);
    *out_size = written;
    result = 0;

done:
    if (configured) {
        xd3_close_stream(&stream);
        xd3_free_stream(&stream);
    }
    if (out != NULL && fclose(out) != 0 && result == 0) {
        snprintf(error, error_size, "Cannot finish writing %s (is the SD card full?)", out_path);
        result = 1;
    }
    if (in != NULL)
        fclose(in);
    free(chunk);
    return result;
}
