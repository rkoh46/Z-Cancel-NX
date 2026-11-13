// SPDX-License-Identifier: GPL-3.0-or-later
/* audio_switch.c: the audio plugin is attached through the static plugin table like the others; this is
 * the one thing the front reads from it directly. */
#ifndef ZNX_AUDIO_SWITCH_H
#define ZNX_AUDIO_SWITCH_H
/* Output buffers that ran dry, sample blocks that dropped old audio, and how much is waiting (ms). */
/* Exported from the plugin like its API: look it up as "znx_audio_stats" in the static table. */
void znx_audio_stats(unsigned* underruns, unsigned* overruns, unsigned* fill_ms);
#endif
