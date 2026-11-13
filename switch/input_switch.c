// SPDX-License-Identifier: GPL-3.0-or-later
/* Z-Cancel NX input for the Switch: the console's controllers as N64 controllers.
 *
 * A rollback session takes its inputs from the front through the core's input callback (the netcode
 * decides which frame gets which input), so this is not an input plugin: the front polls here once a
 * frame and passes the BUTTONS words on.
 *
 * Layout. The system reports a GameCube controller on the adapter with L and R as ZL and ZR and Z as R,
 * so one table serves it and a Pro Controller:
 *
 *   N64 A       <- A                      N64 Start <- Plus (Start on a GameCube controller)
 *   N64 B       <- B                      N64 D-pad <- D-pad
 *   N64 Z       <- ZL or ZR (GameCube L or R: shield)
 *   N64 R       <- R (GameCube Z: grab)
 *   N64 L       <- L (Pro Controller only: taunt)
 *   N64 C-down  <- X          N64 C-left <- Y          (jump, as on the common GameCube layouts)
 *   N64 C pad   <- right stick (the C stick), by direction
 *   N64 stick   <- left stick, scaled so a full tilt reads stick_range, no dead zone (the game has its own)
 */

#include <switch.h>

#include "input_switch.h"

#define PLAYERS 4

/* BUTTONS.Value bits (m64p_plugin.h). */
#define N64_R_DPAD 0x0001u
#define N64_L_DPAD 0x0002u
#define N64_D_DPAD 0x0004u
#define N64_U_DPAD 0x0008u
#define N64_START  0x0010u
#define N64_Z      0x0020u
#define N64_B      0x0040u
#define N64_A      0x0080u
#define N64_R_C    0x0100u
#define N64_L_C    0x0200u
#define N64_D_C    0x0400u
#define N64_U_C    0x0800u
#define N64_R      0x1000u
#define N64_L      0x2000u

static PadState l_pads[PLAYERS];
static uint32_t l_buttons[PLAYERS];
static int l_connected[PLAYERS];
static int l_range = 84;
static int l_quit_frames;

void znx_input_init(int stick_range)
{
    if (stick_range > 0 && stick_range <= 127) l_range = stick_range;
    padConfigureInput(PLAYERS, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadGc);
    /* Player 1 is also the console itself in handheld mode. */
    padInitialize(&l_pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
    padInitialize(&l_pads[1], HidNpadIdType_No2);
    padInitialize(&l_pads[2], HidNpadIdType_No3);
    padInitialize(&l_pads[3], HidNpadIdType_No4);
}

static uint32_t axis(int32_t value)
{
    /* Round to nearest; the system's range is +-32767. */
    int32_t v = (value * l_range + (value >= 0 ? 16383 : -16383)) / 32767;
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    return (uint32_t)(uint8_t)(int8_t)v;
}

void znx_input_poll(void)
{
    int quit = 0;
    for (int i = 0; i < PLAYERS; ++i) {
        padUpdate(&l_pads[i]);
        l_connected[i] = padIsConnected(&l_pads[i]);
        if (!l_connected[i]) { l_buttons[i] = 0; continue; }
        const u64 held = padGetButtons(&l_pads[i]);
        const HidAnalogStickState left = padGetStickPos(&l_pads[i], 0);
        const HidAnalogStickState right = padGetStickPos(&l_pads[i], 1);
        uint32_t b = 0;
        if (held & HidNpadButton_A) b |= N64_A;
        if (held & HidNpadButton_B) b |= N64_B;
        if (held & (HidNpadButton_ZL | HidNpadButton_ZR)) b |= N64_Z;
        if (held & HidNpadButton_R) b |= N64_R;
        if (held & HidNpadButton_L) b |= N64_L;
        if (held & HidNpadButton_X) b |= N64_D_C;
        if (held & HidNpadButton_Y) b |= N64_L_C;
        if (held & HidNpadButton_Plus) b |= N64_START;
        if (held & HidNpadButton_Up) b |= N64_U_DPAD;
        if (held & HidNpadButton_Down) b |= N64_D_DPAD;
        if (held & HidNpadButton_Left) b |= N64_L_DPAD;
        if (held & HidNpadButton_Right) b |= N64_R_DPAD;
        /* The C stick: past 40% of its travel in a direction presses that C button. */
        if (right.y > 13000) b |= N64_U_C;
        if (right.y < -13000) b |= N64_D_C;
        if (right.x < -13000) b |= N64_L_C;
        if (right.x > 13000) b |= N64_R_C;
        b |= axis(left.x) << 16;
        b |= axis(left.y) << 24;
        l_buttons[i] = b;
        const u64 combo = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_R | HidNpadButton_Plus;
        if ((held & HidNpadButton_Minus) || (held & combo) == combo) quit = 1;
    }
    l_quit_frames = quit ? l_quit_frames + 1 : 0;
}

uint32_t znx_input_buttons(int player) { return player >= 0 && player < PLAYERS ? l_buttons[player] : 0; }
int znx_input_connected(int player) { return player >= 0 && player < PLAYERS ? l_connected[player] : 0; }
int znx_input_quit_requested(void) { return l_quit_frames >= 30; }
