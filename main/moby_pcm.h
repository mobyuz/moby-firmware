// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_PCM_H
#define MOBY_PCM_H

#include <stddef.h>
#include <stdint.h>

enum {
    MOBY_PCM_SAMPLE_RATE = 16000,
    MOBY_PCM_BLOCK_FRAMES = 320,
    MOBY_I2S_SLOTS = 2,
};

typedef struct {
    uint64_t square_sum;
    uint32_t samples;
    uint32_t peak;
    uint32_t full_scale;
} moby_pcm_stats_t;

void moby_pcm_from_i2s(const uint32_t *stereo, int16_t *mono, size_t frames);
void moby_pcm_measure(moby_pcm_stats_t *stats, const int16_t *pcm, size_t samples);
uint32_t moby_pcm_rms(const moby_pcm_stats_t *stats);

#endif
