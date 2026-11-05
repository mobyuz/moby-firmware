// SPDX-License-Identifier: GPL-3.0-only

#include "moby_pcm.h"

#include <math.h>

int16_t moby_pcm_sample(uint32_t slot, unsigned gain_q8)
{
    if (gain_q8 > MOBY_PCM_GAIN_Q8_MAX) {
        gain_q8 = MOBY_PCM_GAIN_Q8_MAX;
    }
    // Reinterpret the slot as signed and shift arithmetically without relying on the compiler's choice for
    // negative values: shift the unsigned word, then sign-extend the 16 bits that are left.
    const uint32_t top = slot >> MOBY_PCM_SHIFT;
    const int32_t value = top >= 0x8000U ? (int32_t)top - 65536 : (int32_t)top;
    if (gain_q8 == 256U) {
        return (int16_t)value;
    }
    // Floor division by 256 for negative products too, so the rounding does not depend on the sign.
    const int32_t product = value * (int32_t)gain_q8;
    const int32_t scaled = product >= 0 ? product / 256 : -((-product + 255) / 256);
    return (int16_t)(scaled > INT16_MAX ? INT16_MAX : scaled < INT16_MIN ? INT16_MIN : scaled);
}

void moby_pcm_convert(const uint32_t *stereo, int16_t *mono, size_t frames)
{
    for (size_t frame = 0; frame < frames; ++frame) {
        // The left slot, as the contract says.
        mono[frame] = moby_pcm_sample(stereo[frame * MOBY_I2S_SLOTS], MOBY_PCM_GAIN_Q8);
    }
}

void moby_pcm_from_i2s(const uint32_t *stereo, int16_t *mono, size_t frames)
{
    moby_pcm_convert(stereo, mono, frames);
}

void moby_pcm_to_le_bytes(const int16_t *samples, size_t count, uint8_t *bytes)
{
    for (size_t i = 0; i < count; ++i) {
        const uint16_t value = (uint16_t)samples[i];
        bytes[2 * i] = (uint8_t)(value & 0xffU);
        bytes[2 * i + 1] = (uint8_t)(value >> 8);
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
