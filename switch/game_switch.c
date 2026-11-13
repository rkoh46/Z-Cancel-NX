// SPDX-License-Identifier: GPL-3.0-or-later
/* Z-Cancel NX for the Nintendo Switch, step 3a of docs/PLAN.md: the picture.
 *
 * The same core, RSP plugin, forced netplay settings and frame driver as znx-bench
 * (bench/bench_core.c), with RMG-K's GLideN64 in place of the null video plugin. GLideN64
 * gets the screen through the core's video extension (vidext_switch.c: EGL, desktop OpenGL)
 * and its settings from game/game_core.c. The buffer swap waits for the display, which
 * keeps the game at 60 Hz; the core's own speed limiter is not used. There is no sound and
 * no controller input yet: input comes from a script, or there is none and the game shows
 * its intro and a demo fight.
 *
 * It opens on the start screen (menu/, switch/menu_switch.c): Super Smash Bros., Smash Remix 2.0.1
 * or Remix +EXTRA 0.6.0, the vanilla ROM found by its MD5 and the Remix ROMs built from it on the
 * console; then Play (this) or Online play (menu/online.c). Quitting a game (the quit combination)
 * comes back to the start screen: the core, GLideN64 and the audio plugin are shut down and started
 * again for the next game, on the one EGL window surface the program keeps (screen_switch.c).
 *
 * Files, all under sdmc:/switch/z-cancel-nx/:
 *   *.z64|n64|v64    the vanilla US ROM, any name, here or in roms/ (a ROM path as the NRO's first
 *                    argument, or menu=0, skips the start screen and plays rom.z64 or that file)
 *   roms/, patches/  the built Remix ROMs and their xdelta patches; rom-cache.txt remembers MD5s
 *   inputs.txt       optional input script, the format of bench/inputs/ (frame player buttons_hex)
 *   z-cancel-nx.ini  optional:
 *                      scale=0        0 draws at the screen's 1280x720 (default); 1 or 2 turns
 *                                     GLideN64's frame buffer emulation on at that multiple of
 *                                     the N64's resolution
 *                      rdram_check=1  print "hash <frame> <fnv>" every 600 frames, to compare
 *                                     with znx-bench (3600 -> 1499ebb25c8606c4 without input)
 *                      vsync=1        0 runs as fast as it can
 *                      frames=0       stop after this many frames; 0 runs until Plus
 *                      inputs=<file>  another script; "inputs=" for none
 *                      cpu_mhz=1785   0 leaves the clock alone
 *                      verbose=1      the core's and the plugin's own messages too
 *                      sound=1, stick_range=84   (the start screen's Settings write these two)
 *                      menu=1         0: no start screen, straight into rom.z64 as before
 *   latest.log       every line of output, flushed line by line
 *
 * Lines also go to a PC running nxlink (-s). */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "m64p_frontend.h"

#include "bench_core.h"
#include "front_switch.h"
#include "game_core.h"
#include "input_switch.h"
#include "menu.h"
#include "menu_switch.h"
#include "p2p/znx_p2p.h"
#include "screen_switch.h"
#include "vidext_switch.h"
#include "znx_version.h"
#include "znx_switch.h"

#define PROGRAM "z-cancel-nx"

struct settings {
    struct znx_options options;
    struct znx_game_settings game;
    char inputs_path[512];
    bool inputs_set;
    int rdram_check;
    uint32_t cpu_mhz;
    int sound;                 /* 1: the audio plugin; 0: silent */
    int stick_range;           /* what a fully tilted stick reads on the N64 axis; 0 = 84 */
    int menu;                  /* 1: the start screen */
    int net_overlay;           /* online: the match overlay */
    int net_vsync;             /* online: 0 RMG-K's timer pacer (swap interval 0), 1 vsync with drift correction */
};

static bool ini_key(const char* key, const char* value, void* user)
{
    struct settings* s = user;
    if (!strcmp(key, "scale")) s->game.scale = atoi(value);
    else if (!strcmp(key, "rdram_check")) s->rdram_check = atoi(value);
    else if (!strcmp(key, "vsync")) s->game.vsync = atoi(value) != 0;
    else if (!strcmp(key, "frames")) s->options.frames = atoi(value);
    else if (!strcmp(key, "inputs")) {
        s->inputs_set = true;
        if (*value == '\0') s->inputs_path[0] = '\0';
        else if (strchr(value, ':') != NULL) snprintf(s->inputs_path, sizeof(s->inputs_path), "%s", value);
        else snprintf(s->inputs_path, sizeof(s->inputs_path), ZNX_BASE_DIR "/%s", value);
    }
    else if (!strcmp(key, "cpu_mhz")) s->cpu_mhz = (uint32_t)atoi(value);
    else if (!strcmp(key, "sound")) s->sound = atoi(value) != 0;
    else if (!strcmp(key, "stick_range")) s->stick_range = atoi(value);
    else if (!strcmp(key, "verbose")) s->options.verbose = atoi(value);
    else if (!strcmp(key, "menu")) s->menu = atoi(value) != 0;
    else if (!strcmp(key, "net_overlay")) s->net_overlay = atoi(value) != 0;
    else if (!strcmp(key, "net_vsync")) s->net_vsync = atoi(value) != 0;
    /* the online settings the start screen keeps (menu/settings.h): read there */
    else if (!strcmp(key, "name") || !strcmp(key, "input_delay") || !strcmp(key, "public_list") ||
             !strcmp(key, "p2p_port") || !strcmp(key, "p2p_code") || !strcmp(key, "p2p_token") ||
             !strcmp(key, "nat_server") || !strcmp(key, "list_server") || !strcmp(key, "lobby_server") ||
             !strcmp(key, "lobby_stun") || !strcmp(key, "lobby_bind")) {}
    else return false;
    return true;
}

/* On the emulation thread, after every frame. */
static void frame_done(int frames, uint64_t frame_ns)
{
    znx_game_frame_done(frames, frame_ns);
    if (frames % 600 == 0) {
        /* The audio plugin's own counters, through the same table its API is looked up in. */
        static void (*stats)(unsigned*, unsigned*, unsigned*);
        static bool looked;
        if (!looked) {
            void* audio = znx_static_lib("audio");
            stats = audio != NULL ? (void (*)(unsigned*, unsigned*, unsigned*))osal_dynlib_getproc(audio, "znx_audio_stats") : NULL;
            looked = true;
        }
        if (stats != NULL) {
            unsigned underruns = 0, overruns = 0, fill = 0;
            stats(&underruns, &overruns, &fill);
            znx_log("sound %d: %u ms waiting, ran dry %u times and dropped %u blocks so far", frames, fill, underruns, overruns);
        }
    }
    /* The plugin swaps when the game has drawn something. A frame without a swap (a scene
     * loading) would otherwise not wait for the display at all. */
    if (znx_game_last_frame_swaps() == 0)
        znx_vidext_pace_without_swap();
}

/* Running time of the calling thread on any core (13.0.0+), in ns: system ticks are 19.2 MHz. */
static uint64_t thread_cpu_ns(void)
{
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, (u64)-1)))
        return 0;
    return armTicksToNs(ticks);
}

/* The four controllers for this frame. Holding the quit combination (input_switch.h) ends the run. */
static bool l_quitting;

static void live_inputs(uint32_t buttons[4])
{
    znx_input_poll();
    for (int p = 0; p < 4; ++p)
        buttons[p] = znx_input_buttons(p);
    if (!l_quitting && znx_input_quit_requested()) {
        l_quitting = true;
        znx_log(PROGRAM ": quit combination held at frame %d", znx_bench_frame());
        znx_bench_stop();
    }
}

/* ---- an online match: RMG-K's P2P rollback session (net/p2p/znx_p2p.h) ---- */

static int l_net_mode;            /* enum znx_net_present */
static int l_net_overlay, l_net_first, l_net_leaving, l_net_verbose;
static void* l_core_lib;

/* The local player's controller, once per visible frame (rmgk_gekko submit_local_input ->
 * M64CMD_ROLLBACK_SAMPLE_INPUT -> the input plugin's GetKeys -> here). Port 1 of this console, whichever
 * player this side is: GekkoNet puts it on the host's or the joiner's controller. */
static uint32_t net_input(int control)
{
    if (control != 0)
        return 0;
    znx_input_poll();
    if (!l_net_leaving && znx_input_quit_requested()) {
        l_net_leaving = 1;
        znx_log(PROGRAM ": quit combination held at frame %d: leaving the match", znx_p2p_visible_frames());
        znx_p2p_leave_game();
    }
    return znx_input_buttons(0);
}

static int net_begin(void* user)
{
    (void)user;
    if (l_net_first) {
        /* RMG-K's way (CoreStartEmulation leaves the frame output at its default, pacing on): the core calls
         * rmgk_gekko::pace_before_present from GLideN64's rendering callback right before the swap, and its
         * own limiter, at the time sync's speed, for a frame without a swap. With the vsync presenter the
         * display paces and vidext_switch.c follows the timeline, so the core does not. */
        ptr_CoreDoCommand command = (ptr_CoreDoCommand)osal_dynlib_getproc(l_core_lib, "CoreDoCommand");
        l_net_first = 0;
        if (command != NULL)
            command(M64CMD_FRAME_OUTPUT_SET, l_net_mode == ZNX_PRESENT_TIMER
                    ? M64FRAME_OUTPUT_VIDEO | M64FRAME_OUTPUT_AUDIO | M64FRAME_OUTPUT_PACING
                    : M64FRAME_OUTPUT_VIDEO | M64FRAME_OUTPUT_AUDIO, NULL);
    }
    if (!l_net_leaving && znx_bench_stop_requested()) {
        /* the system asks the program to close (or Plus with an input script): tell the peer */
        l_net_leaving = 1;
        znx_p2p_leave_game();
    }
    return 1;
}

static int net_end(void* user)
{
    const int frames = znx_p2p_visible_frames();
    (void)user;
    znx_vidext_net_frame_end();
    if (frames % 30 == 0) {
        struct znx_p2p_live live;
        znx_p2p_live(&live);
        if (l_net_overlay) {
            char ping[16], delay[16], rollbacks[16];
            const char* labels[3] = { "Ping", "Delay", "Rollbacks" };
            const char* values[3] = { ping, delay, rollbacks };
            if (live.ping >= 0) snprintf(ping, sizeof(ping), "%d ms", live.ping);
            else snprintf(ping, sizeof(ping), "-");
            snprintf(delay, sizeof(delay), "%d", live.delay);
            snprintf(rollbacks, sizeof(rollbacks), "%d", live.rollbacks);
            znx_vidext_set_overlay(labels, values, 3);
        }
        if (frames % 600 == 0)
            znx_log("online %d: ping %d ms, rollbacks %d (deepest %d), %.2f frames ahead, speed %.4f", frames, live.ping,
                    live.rollbacks, live.deepest, live.frames_ahead, live.speed);
    }
    return 1;
}

static void net_speed(double scale, void* user)
{
    (void)user;
    znx_vidext_set_speed(scale);
}

/* rdram_check=1 during an online match: "check <frames> <whole> <rest>" for every 600th frame once it can no
 * longer be rolled back, the lines znx-net prints (net/znx-net.cpp), so a Switch and a Mac log can be diffed. */
static const uint8_t* l_check_rdram;
static int l_check_on, l_check_final = -1, l_check_max_saved = -1;
static struct { int frame; uint64_t whole, rest; } l_check_pending[64];
static int l_check_count;

static void net_state_saved(int frame, void* user)
{
    (void)user;
    if (frame > l_check_max_saved) l_check_max_saved = frame;
    if (l_check_rdram == NULL) {   /* the core hands out the RDRAM pointer once the ROM is running */
        void* (*mem_pointer)(int) = (void* (*)(int))osal_dynlib_getproc(l_core_lib, "DebugMemGetPointer");
        l_check_rdram = mem_pointer != NULL ? (const uint8_t*)mem_pointer(M64P_DBG_PTR_RDRAM) : NULL;
    }
    if (l_check_rdram == NULL || (frame + 1) % 600 != 0) return;
    for (int i = 0; i < l_check_count; ++i)
        if (l_check_pending[i].frame == frame) {   /* re-simulated: the newer state replaces the guess */
            znx_check_rdram(l_check_rdram, 0x40500, 0x40E00, &l_check_pending[i].whole, &l_check_pending[i].rest);
            return;
        }
    if (l_check_count < 64) {
        l_check_pending[l_check_count].frame = frame;
        znx_check_rdram(l_check_rdram, 0x40500, 0x40E00, &l_check_pending[l_check_count].whole, &l_check_pending[l_check_count].rest);
        l_check_count++;
    }
}

static void net_end_frame(int confirmed, void* user)
{
    (void)user;
    const int final_frame = confirmed < l_check_max_saved ? confirmed : l_check_max_saved;
    if (final_frame > l_check_final) l_check_final = final_frame;
    int kept = 0;
    for (int i = 0; i < l_check_count; ++i) {
        if (l_check_pending[i].frame <= l_check_final)
            znx_log("check %d %016llx %016llx", l_check_pending[i].frame + 1,
                    (unsigned long long)l_check_pending[i].whole, (unsigned long long)l_check_pending[i].rest);
        else
            l_check_pending[kept++] = l_check_pending[i];
    }
    l_check_count = kept;
}

static int net_session_run(void)
{
    struct znx_p2p_session_hooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.begin_visible = net_begin;
    hooks.end_visible = net_end;
    hooks.timesync_scale = net_speed;
    hooks.stop_on_peer_drop = 1;
    hooks.verbose_log = l_net_verbose;
    hooks.log_dir = ZNX_BASE_DIR "/profile";
    l_check_rdram = NULL; l_check_final = -1; l_check_max_saved = -1; l_check_count = 0;
    if (l_check_on) {
        hooks.state_saved = net_state_saved;
        hooks.end_frame = net_end_frame;
        znx_log("online: checkpoints every 600 frames on");
    }
    l_net_first = 1;
    l_net_leaving = 0;
    /* peer to peer (znx_p2p_run_session) or a lobby match (net/lobby_play.h): the same hooks, pacing,
     * overlay, quit hold and checkpoints */
    return znx_online_run_session(l_core_lib, &hooks);
}

static int net_session_prepare(void* core)
{
    return znx_online_prepare_core(core);
}

/* The settings file, read again before every game (the start screen may have changed it). */
static void read_settings(struct settings* settings)
{
    memset(settings, 0, sizeof(*settings));
    znx_options_defaults(&settings->options);
    settings->options.frames = 0;          /* until the quit combination */
    settings->options.hash_every = 0;
    settings->options.progress_every = 0;
    settings->options.window_count = 0;
    settings->game.width = 1280;
    settings->game.height = 720;
    settings->game.vsync = 1;
    settings->game.scale = 0;
    settings->game.report_every = 600;
    settings->cpu_mhz = 1785; /* the highest stock CPU clock of the first Switch model */
    settings->sound = 1;
    settings->menu = 1;
    znx_front_read_ini(PROGRAM ".ini", ini_key, settings);
    if (settings->options.frames < 0)
        settings->options.frames = 0;
    if (settings->game.scale < 0 || settings->game.scale > 4)
        settings->game.scale = 0;
    if (settings->rdram_check)
        settings->options.hash_every = 600;
    if (!settings->inputs_set && znx_file_exists(ZNX_BASE_DIR "/inputs.txt"))
        snprintf(settings->inputs_path, sizeof(settings->inputs_path), ZNX_BASE_DIR "/inputs.txt");
    settings->options.inputs_path = settings->inputs_path[0] ? settings->inputs_path : NULL;
    znx_log("settings: scale %d, vsync %d, rdram_check %d, frames %d, inputs %s, cpu_mhz %u, sound %d, stick_range %d",
            settings->game.scale, settings->game.vsync, settings->rdram_check, settings->options.frames,
            settings->options.inputs_path ? settings->options.inputs_path : "none", (unsigned)settings->cpu_mhz,
            settings->sound, settings->stick_range);
}

/* One game, as the program ran before it had a start screen; online, the match the lobby agreed on.
 * Returns znx_front_run's result. */
static int play(const char* rom_path, bool have_romfs, bool online)
{
    static struct settings settings;
    static struct znx_host host;
    static const struct znx_game_video video = { znx_vidext_take_swap_ns, znx_vidext_take_swaps, thread_cpu_ns };
    int exit_code = 1;

    read_settings(&settings);
    settings.options.rom_path = rom_path;
    znx_log("rom: %s", rom_path);
    if (online) {
        /* Swap interval 0 under RMG-K's timer by default: the console's compositor takes whole buffers at
         * the display's refresh, so a frame is never torn, only shown once more or replaced before it was
         * shown, which is what a 1% faster or 0.5% slower game must look like on a 60 Hz screen anyway.
         * net_vsync=1 keeps swap interval 1 and corrects the drift a frame at a time instead. */
        l_net_mode = settings.net_vsync ? ZNX_PRESENT_VSYNC : ZNX_PRESENT_TIMER;
        settings.game.vsync = l_net_mode == ZNX_PRESENT_VSYNC;
        settings.options.inputs_path = NULL;
        settings.options.input_players = znx_online_input_players();   /* what start_*_session sets too */
        l_net_overlay = settings.net_overlay;
        l_net_verbose = settings.options.verbose;
        l_check_on = settings.rdram_check;
        znx_vidext_set_netplay((enum znx_net_present)l_net_mode, l_net_overlay);
        znx_log("online: %s presenter, overlay %s", l_net_mode == ZNX_PRESENT_TIMER ? "RMG-K timer (swap interval 0)"
                : "vsync with drift correction", l_net_overlay ? "on" : "off");
    }

    znx_front_power_begin(settings.cpu_mhz);
    znx_game_begin(&settings.game, &video, znx_front_print);

    memset(&host, 0, sizeof(host));
    host.name = PROGRAM;
    host.core = znx_static_lib("core");
    host.video = znx_static_lib("gliden64");
    host.rsp = znx_static_lib("rsp_hle");
    host.sym = osal_dynlib_getproc;
    host.print = znx_front_print;
    host.data_dir = "romfs:/data";
    host.user_dir = ZNX_BASE_DIR "/profile";
    host.vidext = znx_vidext_functions();
    host.video_config = znx_game_video_config;
    host.frame_done = frame_done;
    host.audio = settings.sound ? znx_static_lib("audio") : NULL;
    l_quitting = false;
    if (online) {
        /* The controller goes through an input plugin, as in RMG-K: rmgk_gekko samples it and only
         * GekkoNet's merged inputs reach the game. */
        void (*set_source)(uint32_t (*)(int));
        host.input = znx_static_lib("input_net");
        set_source = host.input != NULL ? (void (*)(uint32_t (*)(int)))osal_dynlib_getproc(host.input, "znx_input_set_source") : NULL;
        if (set_source != NULL)
            set_source(net_input);
        else
            host.input = NULL;
        znx_input_init(settings.stick_range);
        l_core_lib = host.core;
        host.session_run = net_session_run;
        if (znx_online_lobby_match())
            host.session_prepare = net_session_prepare;   /* the pre-match manifest's cheats and saves */
        znx_front_plus_stops(false);
    } else if (settings.options.inputs_path == NULL) {
        /* Played with controllers: Plus is the N64's Start, so it no longer stops the run. */
        znx_input_init(settings.stick_range);
        host.live_inputs = live_inputs;
        znx_front_plus_stops(false);
        znx_log("input: controllers (hold Minus, or L + R + Z + Start on a GameCube controller, to quit); full stick tilt reads %d",
                settings.stick_range > 0 ? settings.stick_range : 84);
    } else {
        znx_front_plus_stops(true);
    }

    if (host.core == NULL || host.video == NULL || host.rsp == NULL || !have_romfs || (online && host.input == NULL))
        znx_log(PROGRAM ": this build is incomplete (a linked library or the romfs is missing)");
    else
        exit_code = znx_front_run(&host, &settings.options, NULL);
    if (online)
        znx_vidext_set_netplay(ZNX_PRESENT_LOCAL, 0);
    znx_front_power_end();
    return exit_code;
}

int main(int argc, char** argv)
{
    static struct settings settings;
    struct znx_menu* menu = NULL;
    struct znx_menu_config config;
    bool have_romfs;
    int exit_code = 0;

    znx_front_begin(PROGRAM, "latest.log", false);
    have_romfs = znx_front_romfs();
    read_settings(&settings);

    if (!settings.menu || (argc > 1 && znx_file_exists(argv[1]))) {
        /* No start screen: one game, then back to the Homebrew Menu (scripted runs, nxlink). */
        const char* rom = znx_front_find_rom(argc, argv);
        exit_code = rom != NULL ? play(rom, have_romfs, false) : 1;
        znx_front_end(exit_code, exit_code != 0 || settings.options.frames > 0);
        return 0;
    }

    config.base_dir = ZNX_BASE_DIR;
    config.version = ZNX_VERSION;
    config.keyboard = znx_switch_keyboard;
    menu = znx_menu_create(&config);
    if (menu == NULL) {
        znx_log(PROGRAM ": no memory for the start screen");
        znx_front_end(1, true);
        return 0;
    }
    for (;;) {
        enum znx_menu_action action = znx_menu_switch_run(menu);
        int result;
        if (action != ZNX_MENU_PLAY && action != ZNX_MENU_PLAY_ONLINE) {
            exit_code = znx_menu_switch_failed() ? 1 : 0;
            break;
        }
        result = play(znx_menu_rom_path(menu), have_romfs, action == ZNX_MENU_PLAY_ONLINE);
        if (action == ZNX_MENU_PLAY_ONLINE) {
            znx_menu_online_finished(menu);   /* back before the match, or why it ended */
        } else if (result != 0) {
            char tail[1200];
            znx_front_tail(tail, sizeof(tail), 8);
            znx_menu_notice(menu, "The game stopped with an error",
                            "The last lines of the log are below; all of it is in latest.log.", tail);
        }
        if (!appletMainLoop())
            break;
    }
    znx_menu_destroy(menu);
    znx_screen_close();
    /* Leaving from the start screen goes straight back to the Homebrew Menu; if the screen could
     * not be drawn at all, the log's last lines are shown on the text console instead. */
    znx_front_end(exit_code, exit_code != 0);
    return 0;
}
