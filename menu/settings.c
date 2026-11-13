// SPDX-License-Identifier: GPL-3.0-or-later
/* See settings.h. */

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INI_NAME "z-cancel-nx.ini"
#define MAX_LINES 256

static void ini_path(const char* base, char* out, size_t size)
{
    snprintf(out, size, "%s/" INI_NAME, base);
}

/* The key of a "key = value" line, copied into key; 0 for comments and other lines. */
static int line_key(const char* line, char* key, size_t size)
{
    const char* p = line + strspn(line, " \t");
    size_t n = strcspn(p, " \t=\r\n");
    if (*p == '#' || *p == ';' || n == 0 || n >= size || strchr(p, '=') == NULL)
        return 0;
    memcpy(key, p, n);
    key[n] = '\0';
    return 1;
}

static int clamp_stick(int value)
{
    if (value <= 0) return ZNX_STICK_DEFAULT;   /* 0 means the default to the game, too */
    return value < ZNX_STICK_MIN ? ZNX_STICK_MIN : value > ZNX_STICK_MAX ? ZNX_STICK_MAX : value;
}

void znx_settings_load(const char* base, struct znx_menu_settings* s)
{
    char path[600], line[512], key[64];
    FILE* f;
    s->sound = 1;
    s->stick_range = ZNX_STICK_DEFAULT;
    ini_path(base, path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        const char* value;
        if (!line_key(line, key, sizeof(key)))
            continue;
        value = strchr(line, '=') + 1;
        if (!strcmp(key, "sound")) s->sound = atoi(value) != 0;
        else if (!strcmp(key, "stick_range")) s->stick_range = clamp_stick(atoi(value));
    }
    fclose(f);
}

int znx_settings_save(const char* base, const struct znx_menu_settings* s)
{
    char path[600], temp[620], key[64];
    static char lines[MAX_LINES][512];
    int count = 0, wrote_sound = 0, wrote_stick = 0;
    FILE* f;

    ini_path(base, path, sizeof(path));
    f = fopen(path, "r");
    if (f != NULL) {
        while (count < MAX_LINES && fgets(lines[count], sizeof(lines[count]), f) != NULL)
            count++;
        fclose(f);
    }
    snprintf(temp, sizeof(temp), "%s.new", path);
    f = fopen(temp, "w");
    if (f == NULL)
        return 1;
    for (int i = 0; i < count; ++i) {
        char* line = lines[i];
        size_t length = strlen(line);
        if (line_key(line, key, sizeof(key)) && !strcmp(key, "sound")) {
            if (!wrote_sound) fprintf(f, "sound=%d\n", s->sound ? 1 : 0);
            wrote_sound = 1;       /* a second sound= line would override the first: dropped */
            continue;
        }
        if (line_key(line, key, sizeof(key)) && !strcmp(key, "stick_range")) {
            if (!wrote_stick) fprintf(f, "stick_range=%d\n", clamp_stick(s->stick_range));
            wrote_stick = 1;
            continue;
        }
        fputs(line, f);
        if (length > 0 && line[length - 1] != '\n')
            fputc('\n', f);
    }
    if (!wrote_sound) fprintf(f, "sound=%d\n", s->sound ? 1 : 0);
    if (!wrote_stick) fprintf(f, "stick_range=%d\n", clamp_stick(s->stick_range));
    if (fclose(f) != 0) {
        remove(temp);
        return 1;
    }
    remove(path);   /* rename over an existing file is refused on the SD card's FAT */
    return rename(temp, path) != 0;
}
