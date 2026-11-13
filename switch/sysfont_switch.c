// SPDX-License-Identifier: GPL-3.0-or-later
/* See sysfont_switch.h. */

#include "sysfont_switch.h"

#include <stdio.h>

#include <switch.h>

#include "znx_switch.h"

static bool l_asked, l_rejected;
static PlFontData l_standard, l_extended;
static bool l_have_standard, l_have_extended;

bool znx_sysfont_get(const void** standard, size_t* standard_size, const void** extended, size_t* extended_size)
{
    if (!l_asked) {
        /* pl:u. The session stays open for the life of the program: the fonts live in its shared memory. */
        Result rc = plInitialize(PlServiceType_User);
        l_asked = true;
        if (R_FAILED(rc)) {
            znx_log("menu: system font unavailable: 0x%08x (pl:u does not open), using the bundled font", rc);
        } else {
            rc = plGetSharedFontByType(&l_standard, PlSharedFontType_Standard);
            l_have_standard = R_SUCCEEDED(rc) && l_standard.address != NULL && l_standard.size > 0;
            if (l_have_standard)
                znx_log("menu: system font loaded (%u bytes)", (unsigned)l_standard.size);
            else
                znx_log("menu: system font unavailable: 0x%08x, using the bundled font", rc);
            rc = plGetSharedFontByType(&l_extended, PlSharedFontType_NintendoExt);
            l_have_extended = R_SUCCEEDED(rc) && l_extended.address != NULL && l_extended.size > 0;
            if (l_have_extended)
                znx_log("menu: system button glyphs loaded (%u bytes)", (unsigned)l_extended.size);
            else
                znx_log("menu: system button glyphs unavailable: 0x%08x, drawing stand-ins", rc);
        }
    }
    *standard = l_have_standard && !l_rejected ? l_standard.address : NULL;
    *standard_size = l_have_standard && !l_rejected ? l_standard.size : 0;
    *extended = l_have_extended ? l_extended.address : NULL;
    *extended_size = l_have_extended ? l_extended.size : 0;
    return *standard != NULL;
}

void znx_sysfont_reject(void)
{
    l_rejected = true;
}

int znx_sysfont_console_theme(char* note, size_t note_size)
{
    ColorSetId id = ColorSetId_Light;
    Result rc = setsysInitialize();
    note[0] = '\0';
    if (R_FAILED(rc)) {
        snprintf(note, note_size, "set:sys does not open, 0x%08x", rc);
        return -1;
    }
    rc = setsysGetColorSetId(&id);
    setsysExit();
    if (R_FAILED(rc)) {
        snprintf(note, note_size, "0x%08x", rc);
        return -1;
    }
    return id == ColorSetId_Dark ? 1 : 0;
}
