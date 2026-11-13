// SPDX-License-Identifier: GPL-3.0-or-later
/* See catalog.h. */

#if defined(__GNUC__) && !defined(__clang__)
/* File names and paths are cut to the size of their field on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "catalog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "md5.h"

#define VANILLA_SIZE 16777216ull

/* The values are the ones in RMG-K's ROM database (Data/mupen64plus.ini) and in the releases' own
 * ini.txt; the zips are the release assets as GitHub lists them. */
static const struct znx_game_info l_games[ZNX_GAME_COUNT] = {
    { "Super Smash Bros.", "F7C52568A31AADF26E14DC2B6416B2ED", VANILLA_SIZE, NULL,
      { 0x91, 0x6b, 0x8b, 0x5b, 0x78, 0x0b, 0x85, 0xa4 },
      NULL, 0, NULL, NULL, NULL, 0, NULL, "your own cartridge dump (USA)" },
    { "Smash Remix 2.0.1", "2B2D6B295106C54216B7FC7A2F14346E", 65536000ull, "smashremix-2.0.1.z64",
      { 0x34, 0xee, 0x47, 0x49, 0x18, 0x33, 0xc6, 0x4c },
      "smashremix2.0.1.xdelta", 35204600ull, "6E0CA18C55DF1533DB5DA5BD85C45E56",
      "https://github.com/JSsixtyfour/smashremix/releases/download/2.0.1/smashremix2.0.1.zip",
      "smashremix2.0.1.zip", 66074199ull, "B0110389CEE327A20B43662B35BC5C48",
      "JSsixtyfour/smashremix, release 2.0.1" },
    { "Smash Remix 2.0.1 +EXTRA 0.6.0", "823DFA3414285939A44FD715413421F6", 80421504ull, "smashremix-2.0.1-extra-0.6.0.z64",
      { 0x61, 0x75, 0x1d, 0x6a, 0x77, 0x1b, 0x98, 0x88 },
      "SmashRemix2.0.1 +EXTRA (0.6.0).xdelta", 49135962ull, "1550D49DDC578E812B33CF94CF210F9B",
      "https://github.com/joaorb64/smashremix-plus-extra/releases/download/0.6.0/SmashRemix2.0.1.%2BEXTRA.0.6.0.zip",
      "SmashRemix2.0.1.+EXTRA.0.6.0.zip", 46199952ull, "1E3E84F4FBB025B3BCCBE6786644F4F8",
      "joaorb64/smashremix-plus-extra, release 0.6.0" },
};

const struct znx_game_info* znx_game_info(int game)
{
    return game >= 0 && game < ZNX_GAME_COUNT ? &l_games[game] : NULL;
}

/* ---- MD5 and byte order ---- */

void znx_md5_hex(const uint8_t digest[16], char out[33])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[32] = '\0';
}

void znx_md5_buffer(const void* data, size_t size, char out[33])
{
    md5_state_t state;
    md5_byte_t digest[16];
    const uint8_t* p = data;
    md5_init(&state);
    while (size > 0) {
        unsigned chunk = size > (1u << 30) ? (1u << 30) : (unsigned)size;
        md5_append(&state, p, chunk);
        p += chunk;
        size -= chunk;
    }
    md5_finish(&state, digest);
    znx_md5_hex(digest, out);
}

enum order { ORDER_Z64, ORDER_V64, ORDER_N64, ORDER_UNKNOWN };

static enum order rom_order(const uint8_t b[4])
{
    if (b[0] == 0x80 && b[1] == 0x37 && b[2] == 0x12 && b[3] == 0x40) return ORDER_Z64;
    if (b[0] == 0x37 && b[1] == 0x80 && b[2] == 0x40 && b[3] == 0x12) return ORDER_V64;
    if (b[0] == 0x40 && b[1] == 0x12 && b[2] == 0x37 && b[3] == 0x80) return ORDER_N64;
    return ORDER_UNKNOWN;
}

static const char* order_name(enum order order)
{
    return order == ORDER_V64 ? "v64" : order == ORDER_N64 ? "n64" : "z64";
}

/* In place, to big-endian order; size is a multiple of 4 for any N64 ROM. */
static void to_z64(uint8_t* p, size_t size, enum order order)
{
    size &= ~(size_t)3;
    if (order == ORDER_V64) {
        for (size_t i = 0; i < size; i += 2) { uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
    } else if (order == ORDER_N64) {
        for (size_t i = 0; i < size; i += 4) {
            uint8_t a = p[i], b = p[i + 1];
            p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = b; p[i + 3] = a;
        }
    }
}

int znx_md5_file(const char* path, int as_rom, char out[33], int (*progress)(void*, uint64_t, uint64_t), void* user)
{
    enum { CHUNK = 1 << 20 };
    FILE* f = fopen(path, "rb");
    uint8_t* buffer;
    md5_state_t state;
    md5_byte_t digest[16];
    enum order order = ORDER_Z64;
    uint64_t total = 0, done = 0;
    size_t got;
    struct stat info;
    int result = 0;

    if (f == NULL)
        return -1;
    buffer = malloc(CHUNK);
    if (buffer == NULL) {
        fclose(f);
        return -1;
    }
    if (stat(path, &info) == 0)
        total = (uint64_t)info.st_size;
    md5_init(&state);
    while ((got = fread(buffer, 1, CHUNK, f)) > 0) {
        if (as_rom && done == 0 && got >= 4) {
            order = rom_order(buffer);
            if (order == ORDER_UNKNOWN) order = ORDER_Z64;
        }
        if (as_rom)
            to_z64(buffer, got, order);
        md5_append(&state, buffer, (unsigned)got);
        done += got;
        if (progress != NULL && progress(user, done, total) != 0) {
            result = -2;
            break;
        }
    }
    if (ferror(f))
        result = -1;
    fclose(f);
    free(buffer);
    md5_finish(&state, digest);
    znx_md5_hex(digest, out);
    return result;
}

uint8_t* znx_rom_load(const char* path, size_t* size, const char** format, char* error, size_t error_size)
{
    FILE* f = fopen(path, "rb");
    struct stat info;
    uint8_t* data;
    enum order order;

    if (f == NULL || stat(path, &info) != 0) {
        snprintf(error, error_size, "Cannot open %s: %s", path, strerror(errno));
        if (f != NULL) fclose(f);
        return NULL;
    }
    data = malloc((size_t)info.st_size > 4 ? (size_t)info.st_size : 4);
    if (data == NULL) {
        snprintf(error, error_size, "Not enough memory to read %s", path);
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)info.st_size, f) != (size_t)info.st_size) {
        snprintf(error, error_size, "Cannot read %s", path);
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    order = info.st_size >= 4 ? rom_order(data) : ORDER_UNKNOWN;
    if (order == ORDER_UNKNOWN) {
        snprintf(error, error_size, "%s is not an N64 ROM in any byte order", path);
        free(data);
        return NULL;
    }
    to_z64(data, (size_t)info.st_size, order);
    *size = (size_t)info.st_size;
    if (format != NULL)
        *format = order_name(order);
    return data;
}

void znx_format_size(uint64_t bytes, char* out, size_t size)
{
    /* Binary units, as the console's own Data Management screen counts: a 128 Mbit cartridge is 16 MB. */
    const double mb = bytes / 1048576.0;
    if (mb >= 10.0)
        snprintf(out, size, "%.0f MB", mb);
    else if (mb >= 1.0)
        snprintf(out, size, "%.1f MB", mb);
    else if (bytes >= 1024)
        snprintf(out, size, "%.0f KB", bytes / 1024.0);
    else
        snprintf(out, size, "%u bytes", (unsigned)bytes);
}

/* ---- the MD5 cache: "<md5> <size> <mtime> <path>" per line ---- */

#define CACHE_MAX 64

struct cache_entry { char md5[33]; uint64_t size; int64_t mtime; char path[512]; };
static struct cache_entry l_cache[CACHE_MAX];
static int l_cache_count;
static int l_cache_dirty;

static void cache_path(const char* base, char* out, size_t size)
{
    snprintf(out, size, "%s/rom-cache.txt", base);
}

static void cache_load(const char* base)
{
    char path[600], line[700];
    FILE* f;
    l_cache_count = 0;
    l_cache_dirty = 0;
    cache_path(base, path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (l_cache_count < CACHE_MAX && fgets(line, sizeof(line), f) != NULL) {
        struct cache_entry* e = &l_cache[l_cache_count];
        unsigned long long s;
        long long m;
        int consumed = 0;
        line[strcspn(line, "\r\n")] = '\0';
        if (sscanf(line, "%32s %llu %lld %n", e->md5, &s, &m, &consumed) == 3 && consumed > 0 && strlen(e->md5) == 32) {
            e->size = s;
            e->mtime = m;
            snprintf(e->path, sizeof(e->path), "%s", line + consumed);
            l_cache_count++;
        }
    }
    fclose(f);
}

static void cache_save(const char* base)
{
    char path[600], temp[620];
    FILE* f;
    if (!l_cache_dirty)
        return;
    cache_path(base, path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.new", path);
    f = fopen(temp, "w");
    if (f == NULL)
        return;
    fprintf(f, "# Z-Cancel NX: MD5 of ROM files already checked (md5 size mtime path). Safe to delete.\n");
    for (int i = 0; i < l_cache_count; ++i)
        fprintf(f, "%s %llu %lld %s\n", l_cache[i].md5, (unsigned long long)l_cache[i].size,
                (long long)l_cache[i].mtime, l_cache[i].path);
    fclose(f);
    remove(path);
    rename(temp, path);
    l_cache_dirty = 0;
}

static const char* cache_find(const char* path, const struct stat* info)
{
    for (int i = 0; i < l_cache_count; ++i)
        if (!strcmp(l_cache[i].path, path) && l_cache[i].size == (uint64_t)info->st_size &&
            l_cache[i].mtime == (int64_t)info->st_mtime)
            return l_cache[i].md5;
    return NULL;
}

static void cache_put(const char* path, const struct stat* info, const char* md5)
{
    int slot = -1;
    for (int i = 0; i < l_cache_count; ++i)
        if (!strcmp(l_cache[i].path, path)) slot = i;
    if (slot < 0) {
        if (l_cache_count == CACHE_MAX) {   /* drop the oldest */
            memmove(&l_cache[0], &l_cache[1], sizeof(l_cache[0]) * (CACHE_MAX - 1));
            l_cache_count--;
        }
        slot = l_cache_count++;
    }
    snprintf(l_cache[slot].md5, sizeof(l_cache[slot].md5), "%s", md5);
    l_cache[slot].size = (uint64_t)info->st_size;
    l_cache[slot].mtime = (int64_t)info->st_mtime;
    snprintf(l_cache[slot].path, sizeof(l_cache[slot].path), "%s", path);
    l_cache_dirty = 1;
}

void znx_catalog_remember(const char* base, const char* path, const char* md5)
{
    struct stat info;
    if (stat(path, &info) != 0)
        return;
    cache_load(base);
    cache_put(path, &info, md5);
    cache_save(base);
}

/* ---- scanning ---- */

static int has_extension(const char* name, const char* const* extensions)
{
    const char* dot = strrchr(name, '.');
    if (dot == NULL)
        return 0;
    for (int i = 0; extensions[i] != NULL; ++i) {
        const char* a = dot + 1;
        const char* b = extensions[i];
        while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            return 1;
    }
    return 0;
}

static int header_matches(const char* path, const uint8_t crc[8])
{
    uint8_t header[0x18];
    FILE* f = fopen(path, "rb");
    int ok;
    if (f == NULL)
        return 0;
    ok = fread(header, 1, sizeof(header), f) == sizeof(header);
    fclose(f);
    if (!ok)
        return 0;
    to_z64(header, sizeof(header), rom_order(header) == ORDER_UNKNOWN ? ORDER_Z64 : rom_order(header));
    return memcmp(header + 0x10, crc, 8) == 0;
}

static void reject(struct znx_catalog* c, const char* name, const char* md5, uint64_t size, const char* reason)
{
    if (c->rejected_count < ZNX_MAX_REJECTED) {
        struct znx_rejected* r = &c->rejected[c->rejected_count];
        snprintf(r->name, sizeof(r->name), "%s", name);
        snprintf(r->md5, sizeof(r->md5), "%s", md5 != NULL ? md5 : "");
        r->size = size;
        r->reason = reason;
    }
    c->rejected_count++;
}

static void look_at(const char* base, struct znx_catalog* c, const char* path, const char* name)
{
    struct stat info;
    const char* known;
    char md5[33];
    int game = -1;

    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
        return;
    for (int g = 0; g < ZNX_GAME_COUNT; ++g)
        if ((uint64_t)info.st_size == l_games[g].size)
            game = g;
    if (game < 0) {
        reject(c, name, NULL, (uint64_t)info.st_size, "Not one of the three games (its size is wrong)");
        return;
    }
    known = cache_find(path, &info);
    if (known != NULL) {
        /* Remembered as the right ROM, with the same size and time: the header must match too, or the
         * file was replaced in a way that kept both. A file remembered as wrong is hashed again (the
         * player may have put the right one in its place, and the SD card's times are not trusted). */
        if (strcmp(known, l_games[game].md5) == 0 && header_matches(path, l_games[game].header_crc))
            snprintf(md5, sizeof(md5), "%s", known);
        else
            known = NULL;
    }
    if (known == NULL) {
        if (znx_md5_file(path, 1, md5, NULL, NULL) != 0) {
            reject(c, name, NULL, (uint64_t)info.st_size, "Cannot be read");
            return;
        }
        cache_put(path, &info, md5);
    }
    for (int g = 0; g < ZNX_GAME_COUNT; ++g) {
        if (strcmp(md5, l_games[g].md5) != 0 || c->state[g] == ZNX_STATE_READY)
            continue;
        c->state[g] = ZNX_STATE_READY;
        snprintf(c->rom_path[g], sizeof(c->rom_path[g]), "%s", path);
        if (g == ZNX_GAME_VANILLA) {
            uint8_t head[4] = { 0x80, 0x37, 0x12, 0x40 };
            FILE* f = fopen(path, "rb");
            if (f != NULL) { if (fread(head, 1, 4, f) != 4) head[0] = 0x80; fclose(f); }
            snprintf(c->vanilla_name, sizeof(c->vanilla_name), "%s", name);
            c->vanilla_format = order_name(rom_order(head));
        }
        return;
    }
    reject(c, name, md5, (uint64_t)info.st_size,
           game == ZNX_GAME_VANILLA ? "Not the USA version of Super Smash Bros." : "A different build of Smash Remix");
}

static void scan_folder(const char* base, struct znx_catalog* c, const char* folder)
{
    static const char* const roms[] = { "z64", "n64", "v64", NULL };
    DIR* dir = opendir(folder);
    struct dirent* entry;
    char path[1024];
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_extension(entry->d_name, roms))
            continue;
        snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
        look_at(base, c, path, entry->d_name);
    }
    closedir(dir);
}

static void find_patches(struct znx_catalog* c, const char* folder)
{
    static const char* const patches[] = { "xdelta", NULL };
    DIR* dir = opendir(folder);
    struct dirent* entry;
    char path[1024];
    struct stat info;
    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_extension(entry->d_name, patches))
            continue;
        snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        for (int g = 1; g < ZNX_GAME_COUNT; ++g) {
            /* The size picks the patch whatever the file is called; the MD5 is checked before patching. */
            if ((uint64_t)info.st_size == l_games[g].patch_size)
                snprintf(c->patch_path[g], sizeof(c->patch_path[g]), "%s", path);
            else if (!strcmp(entry->d_name, l_games[g].patch_name))
                snprintf(c->patch_problem[g], sizeof(c->patch_problem[g]),
                         "%s in patches/ is not the 2.0.1 patch (wrong size); it is not used", entry->d_name);
        }
    }
    closedir(dir);
}

void znx_catalog_scan(const char* base, struct znx_catalog* c)
{
    char folder[600];
    memset(c, 0, sizeof(*c));
    for (int g = 0; g < ZNX_GAME_COUNT; ++g)
        c->state[g] = ZNX_STATE_NEEDS_ROM;
    c->vanilla_format = "z64";
    cache_load(base);
    scan_folder(base, c, base);
    snprintf(folder, sizeof(folder), "%s/roms", base);
    scan_folder(base, c, folder);
    snprintf(folder, sizeof(folder), "%s/patches", base);
    find_patches(c, folder);
    cache_save(base);

    for (int g = 1; g < ZNX_GAME_COUNT; ++g) {
        if (c->state[g] == ZNX_STATE_READY)
            continue;
        if (c->state[ZNX_GAME_VANILLA] != ZNX_STATE_READY)
            c->state[g] = ZNX_STATE_NEEDS_ROM;
        else
            c->state[g] = c->patch_path[g][0] ? ZNX_STATE_NEEDS_BUILD : ZNX_STATE_NEEDS_PATCH;
    }
}
