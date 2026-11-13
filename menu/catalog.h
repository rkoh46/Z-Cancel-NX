// SPDX-License-Identifier: GPL-3.0-or-later
/* The three games the start screen offers and where their ROMs are.
 *
 * The player brings one file: the vanilla US ROM, found by its MD5 (not its name) among the .z64,
 * .n64 and .v64 files in the base folder and its roms/ folder, in any of the three byte orders.
 * Smash Remix and Remix +EXTRA are built from it with the official xdelta patches (patcher.h); a
 * built ROM is kept in roms/ and reused. MD5s of whole files are remembered in rom-cache.txt
 * (size and modification time as the key), so a later start does not read 16 to 80 MB per file. */
#ifndef ZNX_CATALOG_H
#define ZNX_CATALOG_H

#include <stddef.h>
#include <stdint.h>

enum znx_game_id { ZNX_GAME_VANILLA, ZNX_GAME_REMIX, ZNX_GAME_EXTRA, ZNX_GAME_COUNT };

enum znx_game_state {
    ZNX_STATE_READY,        /* the ROM is there and checked */
    ZNX_STATE_NEEDS_BUILD,  /* the patch is in patches/: built from the vanilla ROM on A */
    ZNX_STATE_NEEDS_PATCH,  /* the patch is not there: downloaded, then built, on A */
    ZNX_STATE_NEEDS_ROM,    /* the vanilla ROM is missing */
};

/* What a game is; fixed. Sizes in bytes, MD5s as 32 upper-case hex digits. */
struct znx_game_info {
    const char* title;        /* "Smash Remix 2.0.1" */
    const char* md5;          /* of the ROM in big-endian (.z64) order; what RMG-K's lobby compares */
    uint64_t size;
    const char* out_name;     /* the built ROM's file name in roms/ (NULL for vanilla) */
    uint8_t header_crc[8];    /* bytes 0x10..0x17 of the ROM: the cheap check of a remembered ROM */
    /* The patch, NULL for vanilla. */
    const char* patch_name;   /* the .xdelta's name inside the release zip and in patches/ */
    uint64_t patch_size;
    const char* patch_md5;
    const char* zip_url;      /* the release asset's browser_download_url */
    const char* zip_name;
    uint64_t zip_size;
    const char* zip_md5;
    const char* source;       /* where it comes from, for the screen: "JSsixtyfour/smashremix, release 2.0.1" */
};

/* A ROM-looking file that is not one of the three games. */
struct znx_rejected {
    char name[96];            /* file name without the folder */
    char md5[33];             /* "" when it was not hashed (wrong size) */
    uint64_t size;
    const char* reason;       /* "not the USA version", ... */
};

#define ZNX_MAX_REJECTED 6

struct znx_catalog {
    enum znx_game_state state[ZNX_GAME_COUNT];
    char rom_path[ZNX_GAME_COUNT][512];   /* READY: the file to run */
    char patch_path[ZNX_GAME_COUNT][512]; /* NEEDS_BUILD: the patch */
    char vanilla_name[96];                /* the vanilla file's name, "" if none */
    const char* vanilla_format;           /* "z64", "v64" (byte-swapped) or "n64" (little-endian) */
    struct znx_rejected rejected[ZNX_MAX_REJECTED];
    int rejected_count;                   /* may be more than ZNX_MAX_REJECTED: only the first are kept */
    char patch_problem[ZNX_GAME_COUNT][160]; /* a file in patches/ that has the name but not the size */
};

const struct znx_game_info* znx_game_info(int game);

/* Looks through base/ and base/roms/, base/patches/; hashes what it has not seen before. */
void znx_catalog_scan(const char* base, struct znx_catalog* catalog);

/* Remembers the MD5 of a file just written (the patcher, after checking the result). */
void znx_catalog_remember(const char* base, const char* path, const char* md5);

/* Reads a whole ROM into memory in big-endian (.z64) order, converting a .v64 or .n64 dump.
 * Returns NULL with a message in error when it cannot. *format: "z64", "v64" or "n64". */
uint8_t* znx_rom_load(const char* path, size_t* size, const char** format, char* error, size_t error_size);

/* MD5 helpers (the core's md5.c). */
void znx_md5_hex(const uint8_t digest[16], char out[33]);
void znx_md5_buffer(const void* data, size_t size, char out[33]);
/* MD5 of a file read in big-endian order (converted on the fly if it is a .v64/.n64).
 * progress(user, done, total) may be NULL; returning non-zero from it stops with -2.
 * 0 on success, -1 if it cannot be read. */
int znx_md5_file(const char* path, int as_rom, char out[33], int (*progress)(void* user, uint64_t done, uint64_t total),
                 void* user);

/* "16 MB", "65.5 MB", "820 KB": sizes as the screen shows them. */
void znx_format_size(uint64_t bytes, char* out, size_t size);

#endif
