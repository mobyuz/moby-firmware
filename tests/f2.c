// SPDX-License-Identifier: GPL-3.0-only

#include <assert.h>
#include <stdio.h>

#include "moby_audio.h"
#include "moby_pcm.h"

static void test_left_slot_pcm(void)
{
    const uint32_t stereo[] = {
        0x7fffff00, 0,
        0x80000000, 0,
        0xffffff00, 0,
        0x0000ffff, 0x80000000,
        0x00010000, 0,
        0xfffeff00, 0,
    };
    int16_t mono[6];
    moby_pcm_from_i2s(stereo, mono, 6);
    assert(mono[0] == INT16_MAX);
    assert(mono[1] == INT16_MIN);
    assert(mono[2] == -1);
    assert(mono[3] == 0);
    assert(mono[4] == 1);
    assert(mono[5] == -2);
}

static void test_level_accumulation(void)
{
    const int16_t pcm[] = {3, -4, 0, 12};
    moby_pcm_stats_t stats = {0};
    assert(moby_pcm_rms(&stats) == 0);
    moby_pcm_measure(&stats, pcm, 2);
    moby_pcm_measure(&stats, pcm + 2, 2);
    assert(stats.samples == 4 && stats.peak == 12 && stats.full_scale == 0);
    assert(stats.square_sum == 169 && moby_pcm_rms(&stats) == 6);
}

static void test_full_scale_accumulation(void)
{
    const int16_t pcm[] = {INT16_MIN, INT16_MAX};
    moby_pcm_stats_t stats = {0};
    for (size_t i = 0; i < 40000; ++i) {
        moby_pcm_measure(&stats, pcm, 2);
    }
    assert(stats.samples == 80000 && stats.peak == 32768 && stats.full_scale == 80000);
    assert(stats.square_sum == UINT64_C(40000) * (UINT64_C(1073741824) + UINT64_C(1073676289)));
    assert(moby_pcm_rms(&stats) == 32767);
}

static void test_capture_requests(void)
{
    const uint32_t press = 4 | MOBY_AUDIO_PRESS;
    const uint32_t release = 8 | MOBY_AUDIO_RELEASE;
    assert(moby_audio_request_state(press, press, false) == MOBY_CAPTURE_ACTIVE);
    assert(moby_audio_request_state(press, press, true) == MOBY_CAPTURE_CANCELLED);
    assert(moby_audio_request_state(press, release, false) == MOBY_CAPTURE_RELEASED);
    assert(moby_audio_request_state(press, release, true) == MOBY_CAPTURE_CANCELLED);
    assert(moby_audio_request_state(press, 8 | MOBY_AUDIO_MUTE, false) == MOBY_CAPTURE_CANCELLED);
    assert(moby_audio_request_state(press, 12 | MOBY_AUDIO_PRESS, false) == MOBY_CAPTURE_CANCELLED);
    assert(moby_audio_request_state(press, 16 | MOBY_AUDIO_RELEASE, false) == MOBY_CAPTURE_CANCELLED);
    assert(moby_audio_request_state(UINT32_MAX - 2, MOBY_AUDIO_RELEASE, false) == MOBY_CAPTURE_RELEASED);
}

int main(void)
{
    test_left_slot_pcm();
    test_level_accumulation();
    test_full_scale_accumulation();
    test_capture_requests();
    puts("4 F2 host tests passed");
    return 0;
}
