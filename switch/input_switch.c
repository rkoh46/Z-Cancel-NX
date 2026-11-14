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
 *   N64 L       <- L (taunt; with l_button=shield, L is a second shield: N64 Z)
 *   N64 C-down  <- X          N64 C-left <- Y          (jump, as on the common GameCube layouts)
 *   N64 C pad   <- right stick (the C stick), by direction; with c_stick=gamecube or c_stick=all a flick of
 *                  it is a smash attack on the ground or an aerial in the air instead (see c_stick_attack)
 *   N64 stick   <- left stick, scaled so a full tilt reads stick_range, no dead zone (the game has its own)
 */

#include <switch.h>

#include "input_switch.h"
#include "znx_switch.h"

#define PLAYERS 4

/* The N64 controller's button bits: the low byte is the reply's first byte, the next its second. */
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
static int l_l_shield;
static int l_style_logged[PLAYERS];
static int l_plus_frames[PLAYERS];
static int l_c_mode;                 /* 0 C buttons, 1 attacks on GameCube controllers, 2 attacks on every controller */
static int l_c_frames[PLAYERS];      /* frames left of the current C-stick attack */
static int l_c_dir[PLAYERS];         /* its direction: 1 up, 2 down, 3 left, 4 right */
static int l_c_armed[PLAYERS];       /* 1 once the C stick is back near the middle (one attack per flick) */

/* The C-stick attack, as Melee's C stick: when the C stick leaves the middle, the control stick is pushed all
 * the way that direction for 4 frames and A is pressed for the first 2. Smash 64 reads a full tilt reached in
 * one frame plus A as a smash attack on the ground, and direction plus A in the air as that aerial (forward
 * or back from the way the fighter faces). The C stick has to come back to the middle before the next one.
 * Only what this console sends changes, never the game, so it is safe online. Returns 1 while an attack is
 * being sent; *b and the stick values are updated. */
static int c_stick_attack(int i, HidAnalogStickState right, uint32_t* b, int32_t* sx, int32_t* sy)
{
    const int32_t ax = right.x < 0 ? -right.x : right.x, ay = right.y < 0 ? -right.y : right.y;
    if (ax < 8000 && ay < 8000)
        l_c_armed[i] = 1;
    if (l_c_frames[i] == 0 && l_c_armed[i] && (ax > 16000 || ay > 16000)) {
        l_c_dir[i] = ay >= ax ? (right.y > 0 ? 1 : 2) : (right.x < 0 ? 3 : 4);
        l_c_frames[i] = 4;
        l_c_armed[i] = 0;
    }
    if (l_c_frames[i] == 0)
        return 0;
    *sx = l_c_dir[i] == 3 ? -32767 : l_c_dir[i] == 4 ? 32767 : 0;
    *sy = l_c_dir[i] == 1 ? 32767 : l_c_dir[i] == 2 ? -32767 : 0;
    if (l_c_frames[i] > 2)
        *b |= N64_A;
    l_c_frames[i]--;
    return 1;
}

/* The quit combination: Minus, or Start (Plus) + Z (R) + both shoulders, whichever Switch buttons the
 * shoulders arrive as. A GameCube controller on the adapter reports L and R as ZL and ZR; a GameCube-style
 * wireless controller or a remapped one can report its left shoulder as L. Any two of L, ZL and ZR count. */
int znx_input_is_quit_combo(uint64_t held)
{
    int shoulders;
    if (held & HidNpadButton_Minus)
        return 1;
    if ((held & (HidNpadButton_Plus | HidNpadButton_R)) != (HidNpadButton_Plus | HidNpadButton_R))
        return 0;
    shoulders = !!(held & HidNpadButton_L) + !!(held & HidNpadButton_ZL) + !!(held & HidNpadButton_ZR);
    return shoulders >= 2;
}

static const char* style_name(u32 style)
{
    if (style & HidNpadStyleTag_NpadGc) return "GameCube controller (adapter)";
    if (style & HidNpadStyleTag_NpadFullKey) return "Pro Controller or compatible";
    if (style & HidNpadStyleTag_NpadHandheld) return "the console (handheld)";
    if (style & HidNpadStyleTag_NpadJoyDual) return "two Joy-Con";
    if (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) return "one Joy-Con";
    return "controller";
}

void znx_input_init_ex(int stick_range, int l_shield, int c_mode)
{
    l_l_shield = l_shield != 0;
    l_c_mode = c_mode >= 0 && c_mode <= 2 ? c_mode : 0;
    for (int i = 0; i < PLAYERS; ++i) { l_c_frames[i] = 0; l_c_armed[i] = 0; }
    znx_input_init(stick_range);
}

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
        if (!l_connected[i]) { l_buttons[i] = 0; l_style_logged[i] = 0; continue; }
        const u64 held = padGetButtons(&l_pads[i]);
        if (!l_style_logged[i]) {
            l_style_logged[i] = 1;
            const u32 style = padGetStyleSet(&l_pads[i]);
            znx_log("input: player %d is %s; L is %s; C stick is %s", i + 1, style_name(style),
                    l_l_shield ? "shield" : "taunt",
                    (l_c_mode == 2 || (l_c_mode == 1 && (style & HidNpadStyleTag_NpadGc))) ? "smash attacks and aerials"
                                                                                           : "the C buttons");
        }
        /* Start held for 2 s without the quit combination: say which buttons arrive, so a controller whose
         * shoulders are reported differently shows up in the log. */
        l_plus_frames[i] = (held & HidNpadButton_Plus) ? l_plus_frames[i] + 1 : 0;
        if (l_plus_frames[i] == 120 && !znx_input_is_quit_combo(held))
            znx_log("input: player %d holds Start with buttons 0x%llx (L 0x%x, R 0x%x, ZL 0x%x, ZR 0x%x)", i + 1,
                    (unsigned long long)held, (unsigned)HidNpadButton_L, (unsigned)HidNpadButton_R,
                    (unsigned)HidNpadButton_ZL, (unsigned)HidNpadButton_ZR);
        const HidAnalogStickState left = padGetStickPos(&l_pads[i], 0);
        const HidAnalogStickState right = padGetStickPos(&l_pads[i], 1);
        uint32_t b = 0;
        if (held & HidNpadButton_A) b |= N64_A;
        if (held & HidNpadButton_B) b |= N64_B;
        if (held & (HidNpadButton_ZL | HidNpadButton_ZR)) b |= N64_Z;
        if (held & HidNpadButton_R) b |= N64_R;
        if (held & HidNpadButton_L) b |= l_l_shield ? N64_Z : N64_L;
        if (held & HidNpadButton_X) b |= N64_D_C;
        if (held & HidNpadButton_Y) b |= N64_L_C;
        if (held & HidNpadButton_Plus) b |= N64_START;
        if (held & HidNpadButton_Up) b |= N64_U_DPAD;
        if (held & HidNpadButton_Down) b |= N64_D_DPAD;
        if (held & HidNpadButton_Left) b |= N64_L_DPAD;
        if (held & HidNpadButton_Right) b |= N64_R_DPAD;
        int32_t sx = left.x, sy = left.y;
        const int c_attacks = l_c_mode == 2 || (l_c_mode == 1 && (padGetStyleSet(&l_pads[i]) & HidNpadStyleTag_NpadGc));
        if (c_attacks) {
            c_stick_attack(i, right, &b, &sx, &sy);
        } else {
            l_c_frames[i] = 0;
            l_c_armed[i] = 0;
            /* The C stick: past 40% of its travel in a direction presses that C button. */
            if (right.y > 13000) b |= N64_U_C;
            if (right.y < -13000) b |= N64_D_C;
            if (right.x < -13000) b |= N64_L_C;
            if (right.x > 13000) b |= N64_R_C;
        }
        /* The core's rollback input word is the controller's four reply bytes in order, first byte
         * highest (pif.c, rollback_write_controller_input): buttons, buttons, stick x, stick y. */
        l_buttons[i] = ((b & 0xFFu) << 24) | ((b & 0xFF00u) << 8) | (axis(sx) << 8) | axis(sy);
        if (znx_input_is_quit_combo(held)) quit = 1;
    }
    l_quit_frames = quit ? l_quit_frames + 1 : 0;
}

uint32_t znx_input_buttons(int player) { return player >= 0 && player < PLAYERS ? l_buttons[player] : 0; }
int znx_input_connected(int player) { return player >= 0 && player < PLAYERS ? l_connected[player] : 0; }
int znx_input_quit_requested(void) { return l_quit_frames >= 30; }
