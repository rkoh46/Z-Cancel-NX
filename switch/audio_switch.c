// SPDX-License-Identifier: GPL-3.0-or-later
/* Z-Cancel NX audio plugin for the Switch: the game's samples to the console's speakers.
 *
 * The core hands over what the game writes to the N64's audio interface: a sample rate
 * (AiDacrateChanged) and blocks of 16-bit stereo samples in RDRAM (AiLenChanged), a few per
 * frame. They go into a ring; a feeder thread resamples the ring to the console's 48 kHz and
 * keeps four 5 ms buffers queued in audout. The resampling ratio leans up to 1% either way to
 * hold the ring near its target, because the game is paced by the screen (60.00 Hz) and not
 * by the N64's clock, so the two rates never match exactly.
 *
 * Re-simulated (rolled back) frames never get here: the core leaves audio out of hidden
 * frames (ai_controller.c, main_rollback_hidden_frame_active). */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_plugin.h"

#include "znx_switch.h"
#include "audio_switch.h"

#define AUDIO_API_VERSION 0x020000
#define OUT_RATE 48000
#define OUT_FRAMES 240                 /* 5 ms per audout buffer */
#define OUT_BUFFERS 4
#define RING_FRAMES 16384              /* about half a second at 32 kHz */
#define TARGET_MS 32                   /* samples kept waiting in the ring */

static AUDIO_INFO l_info;
static int l_rate = 32000;             /* the game's sample rate */
static int l_volume = 100;             /* percent */
static int l_muted;

static Mutex l_lock;
static int16_t l_ring[RING_FRAMES * 2];
static uint32_t l_read, l_fill;        /* frames; l_read is the oldest frame's index */
static double l_phase;                 /* position between l_read and l_read + 1, 0..1 */

static Thread l_thread;
static atomic_int l_running;
static int l_started;
static AudioOutBuffer l_buffers[OUT_BUFFERS];
static void* l_memory[OUT_BUFFERS];

static atomic_uint l_underruns, l_overruns;

/* Counters for the frame log: buffers that ran dry and blocks that had to drop old samples. */
void znx_audio_stats(unsigned* underruns, unsigned* overruns, unsigned* fill_ms)
{
    if (underruns) *underruns = atomic_load(&l_underruns);
    if (overruns) *overruns = atomic_load(&l_overruns);
    if (fill_ms) {
        mutexLock(&l_lock);
        *fill_ms = l_rate > 0 ? (unsigned)((uint64_t)l_fill * 1000 / (uint32_t)l_rate) : 0;
        mutexUnlock(&l_lock);
    }
}

/* Fills one output buffer from the ring. Called by the feeder thread only. */
static void render(int16_t* out)
{
    mutexLock(&l_lock);
    const double target = (double)l_rate * TARGET_MS / 1000.0;
    double lean = target > 0 ? ((double)l_fill - target) / target * 0.02 : 0.0;
    if (lean > 0.01) lean = 0.01;
    if (lean < -0.01) lean = -0.01;
    const double step = (double)l_rate / OUT_RATE * (1.0 + lean);
    const int volume = l_muted ? 0 : l_volume;
    int produced = 0;
    for (; produced < OUT_FRAMES; ++produced) {
        if (l_fill < 2) break;         /* interpolation needs this frame and the next */
        const uint32_t a = l_read % RING_FRAMES, b = (l_read + 1) % RING_FRAMES;
        for (int c = 0; c < 2; ++c) {
            const double s = l_ring[a * 2 + c] + (l_ring[b * 2 + c] - l_ring[a * 2 + c]) * l_phase;
            int v = (int)(s * volume / 100.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[produced * 2 + c] = (int16_t)v;
        }
        l_phase += step;
        while (l_phase >= 1.0 && l_fill >= 2) {
            l_phase -= 1.0;
            l_read = (l_read + 1) % RING_FRAMES;
            --l_fill;
        }
        if (l_phase >= 1.0) l_phase = 0.0;   /* ran out mid-step */
    }
    mutexUnlock(&l_lock);
    if (produced < OUT_FRAMES) {
        /* Hold the last value down to silence instead of a click. */
        const int16_t last_l = produced ? out[(produced - 1) * 2] : 0, last_r = produced ? out[(produced - 1) * 2 + 1] : 0;
        const int rest = OUT_FRAMES - produced;
        for (int i = 0; i < rest; ++i) {
            out[(produced + i) * 2] = (int16_t)(last_l * (rest - 1 - i) / rest);
            out[(produced + i) * 2 + 1] = (int16_t)(last_r * (rest - 1 - i) / rest);
        }
        if (l_started > 1) atomic_fetch_add(&l_underruns, 1);
    }
}

static void feeder(void* unused)
{
    (void)unused;
    while (atomic_load(&l_running)) {
        AudioOutBuffer* released = NULL;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 50000000ull)) || released == NULL)
            continue;
        render((int16_t*)released->buffer);
        released->data_size = OUT_FRAMES * 4;
        audoutAppendAudioOutBuffer(released);
    }
}

static int start_output(void)
{
    if (l_started) return 1;
    if (R_FAILED(audoutInitialize())) { znx_log("audio: audoutInitialize failed"); return 0; }
    if (R_FAILED(audoutStartAudioOut())) { znx_log("audio: audoutStartAudioOut failed"); audoutExit(); return 0; }
    for (int i = 0; i < OUT_BUFFERS; ++i) {
        l_memory[i] = aligned_alloc(0x1000, 0x1000);
        if (l_memory[i] == NULL) { znx_log("audio: out of memory"); return 0; }
        memset(l_memory[i], 0, 0x1000);
        l_buffers[i].next = NULL;
        l_buffers[i].buffer = l_memory[i];
        l_buffers[i].buffer_size = 0x1000;
        l_buffers[i].data_size = OUT_FRAMES * 4;
        l_buffers[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&l_buffers[i]);
    }
    atomic_store(&l_running, 1);
    /* Above the emulation thread's priority (0x1C; a lower number runs first) so a busy frame never starves
     * the output; it wakes for a few microseconds every 5 ms. Core -2: wherever the system puts it. */
    if (R_FAILED(threadCreate(&l_thread, feeder, NULL, NULL, 0x8000, 0x1A, -2)) || R_FAILED(threadStart(&l_thread))) {
        znx_log("audio: could not start the feeder thread");
        atomic_store(&l_running, 0);
        return 0;
    }
    l_started = 1;
    znx_log("audio: %d Hz from the game, %d Hz out, %d ms buffers, ring target %d ms", l_rate, OUT_RATE,
            OUT_FRAMES * 1000 / OUT_RATE, TARGET_MS);
    return 1;
}

static void stop_output(void)
{
    if (!l_started) return;
    atomic_store(&l_running, 0);
    threadWaitForExit(&l_thread);
    threadClose(&l_thread);
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < OUT_BUFFERS; ++i) { free(l_memory[i]); l_memory[i] = NULL; }
    l_started = 0;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void* context, void (*debug)(void*, int, const char*))
{
    (void)core; (void)context; (void)debug;
    mutexInit(&l_lock);
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    stop_output();
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type* type, int* version, int* api_version,
                                        const char** name, int* capabilities)
{
    if (type != NULL) *type = M64PLUGIN_AUDIO;
    if (version != NULL) *version = 0x000100;
    if (api_version != NULL) *api_version = AUDIO_API_VERSION;
    if (name != NULL) *name = "Z-Cancel NX audio";
    if (capabilities != NULL) *capabilities = 0;
    return M64ERR_SUCCESS;
}

EXPORT int CALL InitiateAudio(AUDIO_INFO info)
{
    l_info = info;
    return 1;
}

EXPORT void CALL AiDacrateChanged(int system_type)
{
    /* The video clock the sample rate is divided from (NTSC, PAL, MPAL). */
    const unsigned clock = system_type == SYSTEM_PAL ? 49656530u : system_type == SYSTEM_MPAL ? 48628316u : 48681812u;
    const int rate = (int)(clock / (*l_info.AI_DACRATE_REG + 1));
    if (rate < 4000 || rate > 96000) return;
    mutexLock(&l_lock);
    if (rate != l_rate) {
        l_rate = rate;
        l_fill = 0; l_read = 0; l_phase = 0;
    }
    mutexUnlock(&l_lock);
}

EXPORT void CALL AiLenChanged(void)
{
    const uint32_t length = *l_info.AI_LEN_REG;
    const uint8_t* p = l_info.RDRAM + (*l_info.AI_DRAM_ADDR_REG & 0xFFFFFF);
    uint32_t frames = length / 4;
    if (frames == 0 || !l_started) return;
    if (frames > RING_FRAMES / 2) { p += (size_t)(frames - RING_FRAMES / 2) * 4; frames = RING_FRAMES / 2; }
    mutexLock(&l_lock);
    /* More than four times the target waiting: something stalled; keep the newest target's worth. */
    const uint32_t most = (uint32_t)l_rate * TARGET_MS / 1000 * 4;
    if (l_fill + frames > RING_FRAMES || l_fill > most) {
        const uint32_t keep = (uint32_t)l_rate * TARGET_MS / 1000;
        if (l_fill > keep) { l_read = (l_read + (l_fill - keep)) % RING_FRAMES; l_fill = keep; }
        atomic_fetch_add(&l_overruns, 1);
    }
    uint32_t write = (l_read + l_fill) % RING_FRAMES;
    for (uint32_t i = 0; i < frames; ++i) {
        /* RDRAM holds big-endian sample pairs in host-order words: left is the word's upper half. */
        l_ring[write * 2] = (int16_t)(p[i * 4 + 2] | (p[i * 4 + 3] << 8));
        l_ring[write * 2 + 1] = (int16_t)(p[i * 4] | (p[i * 4 + 1] << 8));
        write = (write + 1) % RING_FRAMES;
    }
    l_fill += frames;
    if (l_started == 1 && l_fill >= (uint32_t)l_rate * TARGET_MS / 1000) l_started = 2;   /* underruns count from here */
    mutexUnlock(&l_lock);
}

EXPORT void CALL ProcessAList(void) {}

EXPORT int CALL RomOpen(void)
{
    return start_output();
}

EXPORT void CALL RomClosed(void)
{
    stop_output();
}

EXPORT void CALL SetSpeedFactor(int percent) { (void)percent; }

EXPORT void CALL VolumeMute(void) { l_muted = !l_muted; }
EXPORT void CALL VolumeUp(void) { l_volume = l_volume > 95 ? 100 : l_volume + 5; }
EXPORT void CALL VolumeDown(void) { l_volume = l_volume < 5 ? 0 : l_volume - 5; }
EXPORT int CALL VolumeGetLevel(void) { return l_muted ? 0 : l_volume; }
EXPORT void CALL VolumeSetLevel(int level) { l_volume = level < 0 ? 0 : level > 100 ? 100 : level; }
EXPORT const char* CALL VolumeGetString(void)
{
    static char text[16];
    if (l_muted) return "Mute";
    snprintf(text, sizeof text, "%d%%", l_volume);
    return text;
}
