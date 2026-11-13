// SPDX-License-Identifier: GPL-3.0-or-later
/* znx-input-net: the input plugin of znx-net.
 *
 * In an RMG-K rollback session the local player's controller is read once per frame through the
 * input plugin (rmgk_gekko.cpp submit_local_input -> M64CMD_ROLLBACK_SAMPLE_INPUT ->
 * input.getKeys(0)), handed to GekkoNet, and only GekkoNet's merged inputs reach the game. This
 * plugin answers getKeys from a function the program registers (znx_input_set_source), which
 * returns the controller word in the rollback callback's form (first reply byte highest, the
 * format of bench/inputs/). The core turns BUTTONS back into that word byte by byte
 * (api/frontend.c rollback_sample_input_value), so the word is stored most significant byte first.
 *
 * InitiateControllers does what the core's dummy input plugin does: controller 1 present, no pak.
 * In a rollback session the synchronized ports are forced present and the others absent anyway
 * (pif.c), so this matches the bench, which runs with the dummy plugin. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_plugin.h"

#define INPUT_NET_API_VERSION 0x020100

static uint32_t (*l_source)(int control);

EXPORT void CALL znx_input_set_source(uint32_t (*source)(int control))
{
    l_source = source;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void* context, void (*debug)(void*, int, const char*))
{
    (void)core; (void)context; (void)debug;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type* type, int* version, int* api_version,
                                        const char** name, int* capabilities)
{
    if (type != NULL) *type = M64PLUGIN_INPUT;
    if (version != NULL) *version = 0x000100;
    if (api_version != NULL) *api_version = INPUT_NET_API_VERSION;
    if (name != NULL) *name = "znx-input-net";
    if (capabilities != NULL) *capabilities = 0;
    return M64ERR_SUCCESS;
}

EXPORT void CALL InitiateControllers(CONTROL_INFO info)
{
    info.Controls[0].Present = 1;
}

EXPORT void CALL GetKeys(int control, BUTTONS* keys)
{
    uint32_t word = l_source != NULL ? l_source(control) : 0;
    uint8_t* bytes = (uint8_t*)&keys->Value;
    bytes[0] = (uint8_t)(word >> 24);
    bytes[1] = (uint8_t)(word >> 16);
    bytes[2] = (uint8_t)(word >> 8);
    bytes[3] = (uint8_t)word;
}

EXPORT void CALL ControllerCommand(int control, unsigned char* command) { (void)control; (void)command; }
EXPORT void CALL ReadController(int control, unsigned char* command) { (void)control; (void)command; }
EXPORT int CALL RomOpen(void) { return 1; }
EXPORT void CALL RomClosed(void) {}
EXPORT void CALL SDL_KeyDown(int keymod, int keysym) { (void)keymod; (void)keysym; }
EXPORT void CALL SDL_KeyUp(int keymod, int keysym) { (void)keymod; (void)keysym; }
