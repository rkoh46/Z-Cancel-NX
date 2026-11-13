// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-video-null: a video plugin that draws nothing but finishes every display list.
 *
 * The core's built-in dummy video plugin never reports that the RDP is done. A game that
 * waits for that (Super Smash Bros. does, through its task scheduler) then sits on its
 * first graphics task for ever. A real plugin raises the DP interrupt when it reaches the
 * display list's full-sync command; this one raises it for every display list, without
 * reading the list. The core turns the flag into a DP interrupt at a fixed emulated time
 * (rsp_core.c, do_SP_Task), so the result does not depend on the host.
 *
 * Nothing is drawn and RDRAM is never written, so framebuffer effects that a real plugin
 * copies back into RDRAM are absent. */

#include <stddef.h>
#ifndef __SWITCH__
#include <dlfcn.h>
#endif

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_plugin.h"

#define VIDEO_NULL_API_VERSION 0x020200
#define MI_INTR_DP 0x20

static GFX_INFO l_gfx;
/* The core's ZnxRdramWriterReports (patches/0014), if it has it: this plugin never writes RDRAM. */
static void (*l_writer_reports)(int writer, int reports);

#ifdef __SWITCH__
void* osal_dynlib_getproc(void* lib, const char* name);   /* switch/dynlib_static.c */
#endif

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void* context, void (*debug)(void*, int, const char*))
{
    (void)context; (void)debug;
#ifdef __SWITCH__
    l_writer_reports = (void (*)(int, int))osal_dynlib_getproc(core, "ZnxRdramWriterReports");
#else
    l_writer_reports = (void (*)(int, int))dlsym(core, "ZnxRdramWriterReports");
#endif
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type* type, int* version, int* api_version,
                                        const char** name, int* capabilities)
{
    if (type != NULL) *type = M64PLUGIN_GFX;
    if (version != NULL) *version = 0x000100;
    if (api_version != NULL) *api_version = VIDEO_NULL_API_VERSION;
    if (name != NULL) *name = "Z-Cancel NX null video";
    if (capabilities != NULL) *capabilities = 0;
    return M64ERR_SUCCESS;
}

EXPORT int CALL InitiateGFX(GFX_INFO info)
{
    l_gfx = info;
    if (l_writer_reports != NULL)
        l_writer_reports(1, 1);   /* the video plugin writes no RDRAM */
    return 1;
}

EXPORT void CALL ProcessDList(void)
{
    *l_gfx.MI_INTR_REG |= MI_INTR_DP;
    l_gfx.CheckInterrupts();
}

/* Raw RDP command lists only come from a low-level RSP; the HLE RSP sends display lists. */
EXPORT void CALL ProcessRDPList(void) {}

EXPORT int  CALL RomOpen(void) { return 1; }
EXPORT void CALL RomClosed(void) {}
EXPORT void CALL ChangeWindow(void) {}
EXPORT void CALL MoveScreen(int x, int y) { (void)x; (void)y; }
EXPORT void CALL ShowCFB(void) {}
EXPORT void CALL UpdateScreen(void) {}
EXPORT void CALL ViStatusChanged(void) {}
EXPORT void CALL ViWidthChanged(void) {}
EXPORT void CALL ReadScreen2(void* dest, int* width, int* height, int front)
{
    (void)dest; (void)front;
    if (width != NULL) *width = 0;
    if (height != NULL) *height = 0;
}
EXPORT void CALL SetRenderingCallback(void (*callback)(int)) { (void)callback; }
EXPORT void CALL ResizeVideoOutput(int width, int height) { (void)width; (void)height; }
EXPORT void CALL FBRead(unsigned int addr) { (void)addr; }
EXPORT void CALL FBWrite(unsigned int addr, unsigned int size) { (void)addr; (void)size; }
EXPORT void CALL FBGetFrameBufferInfo(void* info) { (void)info; }
