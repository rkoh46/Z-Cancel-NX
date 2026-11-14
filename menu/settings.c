// SPDX-License-Identifier: GPL-3.0-or-later
/* See settings.h. */

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INI_NAME "z-cancel-nx.ini"
#define MAX_LINES 256
#define KEY_COUNT 12

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

static int clamp(int value, int lo, int hi) { return value < lo ? lo : value > hi ? hi : value; }

/* A text value: trimmed, no line breaks, at most size - 1 bytes. */
static void copy_text(char* out, size_t size, const char* value)
{
    size_t n;
    value += strspn(value, " \t");
    n = strcspn(value, "\r\n");
    while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t'))
        n--;
    if (n >= size)
        n = size - 1;
    memcpy(out, value, n);
    out[n] = '\0';
}

void znx_settings_load(const char* base, struct znx_menu_settings* s)
{
    char path[600], line[512], key[64];
    FILE* f;
    memset(s, 0, sizeof(*s));
    s->sound = 1;
    s->stick_range = ZNX_STICK_DEFAULT;
    snprintf(s->name, sizeof(s->name), "Switch");
    s->input_delay = 0;
    s->p2p_port = 27886;
    snprintf(s->theme, sizeof(s->theme), "auto");
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
        else if (!strcmp(key, "name")) { copy_text(s->name, sizeof(s->name), value); if (!s->name[0]) snprintf(s->name, sizeof(s->name), "Switch"); }
        else if (!strcmp(key, "input_delay")) s->input_delay = clamp(atoi(value), 0, 9);
        else if (!strcmp(key, "public_list")) s->public_list = atoi(value) != 0;
        else if (!strcmp(key, "net_overlay")) s->net_overlay = atoi(value) != 0;
        else if (!strcmp(key, "p2p_port")) { s->p2p_port = atoi(value); if (s->p2p_port <= 0 || s->p2p_port > 65535) s->p2p_port = 27886; }
        else if (!strcmp(key, "p2p_code")) copy_text(s->p2p_code, sizeof(s->p2p_code), value);
        else if (!strcmp(key, "p2p_token")) copy_text(s->p2p_token, sizeof(s->p2p_token), value);
        else if (!strcmp(key, "nat_server")) copy_text(s->nat_server, sizeof(s->nat_server), value);
        else if (!strcmp(key, "list_server")) copy_text(s->list_server, sizeof(s->list_server), value);
        else if (!strcmp(key, "net_vsync")) s->net_vsync = atoi(value) != 0;
        else if (!strcmp(key, "lobby_server")) copy_text(s->lobby_server, sizeof(s->lobby_server), value);
        else if (!strcmp(key, "lobby_stun")) copy_text(s->lobby_stun, sizeof(s->lobby_stun), value);
        else if (!strcmp(key, "lobby_bind")) copy_text(s->lobby_bind, sizeof(s->lobby_bind), value);
        else if (!strcmp(key, "lobby_ice_test")) copy_text(s->lobby_ice_test, sizeof(s->lobby_ice_test), value);
        else if (!strcmp(key, "theme")) copy_text(s->theme, sizeof(s->theme), value);
        else if (!strcmp(key, "debug_glyphs")) s->debug_glyphs = atoi(value) != 0;
    }
    fclose(f);
}

/* The keys this file writes, in the order missing ones are appended. */
static void format_key(const struct znx_menu_settings* s, int index, char* key, size_t key_size, char* line, size_t size)
{
    static const char* const keys[KEY_COUNT] = { "sound", "stick_range", "name", "input_delay", "public_list",
                                                 "net_overlay", "p2p_port", "p2p_code", "p2p_token", "nat_server",
                                                 "list_server", "net_vsync" };
    snprintf(key, key_size, "%s", keys[index]);
    switch (index) {
    case 0: snprintf(line, size, "sound=%d\n", s->sound ? 1 : 0); break;
    case 1: snprintf(line, size, "stick_range=%d\n", clamp_stick(s->stick_range)); break;
    case 2: snprintf(line, size, "name=%s\n", s->name); break;
    case 3: snprintf(line, size, "input_delay=%d\n", clamp(s->input_delay, 0, 9)); break;
    case 4: snprintf(line, size, "public_list=%d\n", s->public_list ? 1 : 0); break;
    case 5: snprintf(line, size, "net_overlay=%d\n", s->net_overlay ? 1 : 0); break;
    case 6: snprintf(line, size, "p2p_port=%d\n", s->p2p_port); break;
    case 7: snprintf(line, size, "p2p_code=%s\n", s->p2p_code); break;
    case 8: snprintf(line, size, "p2p_token=%s\n", s->p2p_token); break;
    case 9: snprintf(line, size, "nat_server=%s\n", s->nat_server); break;
    case 10: snprintf(line, size, "list_server=%s\n", s->list_server); break;
    default: snprintf(line, size, "net_vsync=%d\n", s->net_vsync ? 1 : 0); break;
    }
}

int znx_settings_save(const char* base, const struct znx_menu_settings* s)
{
    char path[600], temp[620], key[64], own_key[32], own_line[300];
    static char lines[MAX_LINES][512];
    int count = 0, wrote[KEY_COUNT] = { 0 };
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
        int handled = 0;
        if (line_key(line, key, sizeof(key))) {
            for (int k = 0; k < KEY_COUNT; ++k) {
                format_key(s, k, own_key, sizeof(own_key), own_line, sizeof(own_line));
                if (!strcmp(key, own_key)) {
                    if (!wrote[k]) fputs(own_line, f);
                    wrote[k] = 1;   /* a second line with the key would override the first: dropped */
                    handled = 1;
                    break;
                }
            }
        }
        if (handled)
            continue;
        fputs(line, f);
        if (length > 0 && line[length - 1] != '\n')
            fputc('\n', f);
    }
    for (int k = 0; k < KEY_COUNT; ++k) {
        /* the test-only server overrides and the pacing switch are not added to a file that lacks them */
        if (wrote[k] || ((k == 9 || k == 10) && !(k == 9 ? s->nat_server[0] : s->list_server[0])) ||
            (k == 11 && !s->net_vsync))
            continue;
        format_key(s, k, own_key, sizeof(own_key), own_line, sizeof(own_line));
        fputs(own_line, f);
    }
    if (fclose(f) != 0) {
        remove(temp);
        return 1;
    }
    remove(path);   /* rename over an existing file is refused on the SD card's FAT */
    return rename(temp, path) != 0;
}
