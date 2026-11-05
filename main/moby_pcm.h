// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_PCM_H
#define MOBY_PCM_H

#include <stddef.h>
#include <stdint.h>

enum {
    MOBY_PCM_SAMPLE_RATE = 16000,
    MOBY_PCM_BLOCK_FRAMES = 320,
    MOBY_I2S_SLOTS = 2,

    // One audio frame on the wire: 20 ms of 16 kHz mono signed 16-bit little-endian samples, 640 bytes.
    MOBY_PCM_FRAME_MS = 20,
    MOBY_PCM_FRAME_SAMPLES = MOBY_PCM_SAMPLE_RATE / 1000 * MOBY_PCM_FRAME_MS,
    MOBY_PCM_FRAME_BYTES = MOBY_PCM_FRAME_SAMPLES * 2,

    // The INMP441 needs 2^18 BCLK cycles (256 ms at 1.024 MHz) after the clock starts before its output is valid.
    // Capture drops the first 13 blocks of 20 ms (260 ms), so no startup garbage reaches the ring.
    MOBY_PCM_WARMUP_SAMPLES = 13 * MOBY_PCM_FRAME_SAMPLES,
};

// The conversion contract, which the wake word training must match exactly (see the F4 ADR).
//
// The INMP441 sends a 24-bit two's complement sample, MSB first, in the top bits of a 32-bit I2S slot (bits 31..8;
// bits 7..0 are zero or noise). Only the left slot is used (L/R tied to GND). The 16-bit sample is
//
//     pcm16 = clamp((int32_t)slot >> MOBY_PCM_SHIFT) * MOBY_PCM_GAIN_Q8 / 256)
//
// that is: an arithmetic shift right by 16 (the top 16 bits of the 24-bit value, truncating toward minus infinity,
// no rounding, no dither), then a gain of MOBY_PCM_GAIN_Q8 / 256 with saturation. The gain is 1.0 today. There is no
// DC removal and no filtering: the microphone's DC offset and its own high-pass response reach the desktop as is.
enum {
    MOBY_PCM_SHIFT = 16,
    MOBY_PCM_GAIN_Q8 = 256,
    // The largest gain the conversion accepts (16x), which keeps the multiplication inside 32 bits.
    MOBY_PCM_GAIN_Q8_MAX = 4096,
};

// One sample of the contract above for an explicit gain (a gain above the maximum is clamped to it).
int16_t moby_pcm_sample(uint32_t slot, unsigned gain_q8);

// Converts frames of interleaved stereo 32-bit slots to mono, with the gain MOBY_PCM_GAIN_Q8.
void moby_pcm_convert(const uint32_t *stereo, int16_t *mono, size_t frames);

// The same as moby_pcm_convert (the F2 bench uses this name).
void moby_pcm_from_i2s(const uint32_t *stereo, int16_t *mono, size_t frames);

// Writes samples as little-endian bytes, the byte order of an audio frame payload.
void moby_pcm_to_le_bytes(const int16_t *samples, size_t count, uint8_t *bytes);

typedef struct {
    uint64_t square_sum;
    uint32_t samples;
    uint32_t peak;
    uint32_t full_scale;
} moby_pcm_stats_t;

void moby_pcm_measure(moby_pcm_stats_t *stats, const int16_t *pcm, size_t samples);
uint32_t moby_pcm_rms(const moby_pcm_stats_t *stats);

// Where the link takes captured samples from. The platform provides it (a ring buffer filled by the capture
// task); host tests provide a fake. Nothing here blocks.
enum {
    // The source dropped samples because the consumer fell behind.
    MOBY_PCM_EVENT_OVERRUN = 1,
    // The capture hardware failed.
    MOBY_PCM_EVENT_FAULT = 2,
};

typedef struct {
    void *ctx;
    // Copies exactly `samples` samples into out and returns `samples`, or returns 0 and copies nothing when fewer
    // are buffered. The samples stay buffered until consume.
    size_t (*peek)(void *ctx, int16_t *out, size_t samples);
    // Drops (and wipes) the first `samples` buffered samples.
    void (*consume)(void *ctx, size_t samples);
    // Returns the MOBY_PCM_EVENT_* bits seen since the last call, and clears them.
    uint32_t (*take_events)(void *ctx);
} moby_pcm_source_t;

#endif
