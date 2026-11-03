// SPDX-License-Identifier: GPL-3.0-only

#include "moby_pcm.h"

#include <math.h>

void moby_pcm_from_i2s(const uint32_t *stereo, int16_t *mono, size_t frames)
{
    for (size_t frame = 0; frame < frames; ++frame) {
        // Left slot, 24-bit two's complement in bits 31..8. Keep the top 16 bits at unity gain.
        const int32_t bits = (int32_t)(stereo[frame * MOBY_I2S_SLOTS] >> 16);
        mono[frame] = (int16_t)(bits > INT16_MAX ? bits - 65536 : bits);
    }
}

void moby_pcm_measure(moby_pcm_stats_t *stats, const int16_t *pcm, size_t samples)
{
    for (size_t i = 0; i < samples; ++i) {
        const int32_t value = pcm[i];
        const uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        if (magnitude > stats->peak) {
            stats->peak = magnitude;
        }
        stats->full_scale += value == INT16_MIN || value == INT16_MAX;
        stats->square_sum += (uint64_t)(value * value);
        ++stats->samples;
    }
}

uint32_t moby_pcm_rms(const moby_pcm_stats_t *stats)
{
    return stats->samples ? (uint32_t)sqrt((double)stats->square_sum / stats->samples) : 0;
}
