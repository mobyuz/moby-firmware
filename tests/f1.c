// SPDX-License-Identifier: GPL-3.0-only

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "moby_controls.h"
#include "moby_face.h"

static void test_talk_debounce(void)
{
    moby_controls_t controls;
    moby_controls_init(&controls, false, false, 0);
    moby_controls_update(&controls, false, false, 29);
    assert(controls.muted);
    moby_controls_update(&controls, false, false, 30);
    assert(!controls.muted);

    moby_controls_update(&controls, false, true, 40);
    moby_controls_update(&controls, false, false, 50);
    moby_controls_update(&controls, false, true, 60);
    moby_controls_update(&controls, false, true, 89);
    assert(!controls.talk_down && !controls.attention);
    moby_controls_update(&controls, false, true, 90);
    assert(controls.talk_down && controls.attention);

    moby_controls_update(&controls, false, false, 100);
    moby_controls_update(&controls, false, false, 129);
    assert(controls.attention);
    moby_controls_update(&controls, false, false, 130);
    assert(!controls.talk_down && !controls.attention);
}

static void test_mute_requires_new_press(void)
{
    moby_controls_t controls;
    moby_controls_init(&controls, false, false, 0);
    moby_controls_update(&controls, false, false, 30);
    moby_controls_update(&controls, false, true, 40);
    moby_controls_update(&controls, false, true, 70);
    assert(controls.attention);
    moby_controls_update(&controls, true, true, 71);
    assert(controls.muted && !controls.attention);
    moby_controls_update(&controls, false, true, 80);
    moby_controls_update(&controls, false, true, 109);
    assert(controls.muted);
    moby_controls_update(&controls, false, true, 110);
    assert(!controls.muted && controls.talk_down && !controls.attention);

    moby_controls_update(&controls, false, false, 120);
    moby_controls_update(&controls, false, false, 150);
    moby_controls_update(&controls, false, true, 160);
    moby_controls_update(&controls, false, true, 190);
    assert(controls.attention);
}

static void test_press_started_during_mute(void)
{
    moby_controls_t controls;
    moby_controls_init(&controls, true, false, 0);
    moby_controls_update(&controls, false, false, 10);
    moby_controls_update(&controls, false, true, 20);
    moby_controls_update(&controls, false, true, 40);
    moby_controls_update(&controls, false, true, 50);
    assert(!controls.muted && controls.talk_down && !controls.attention);
}

static void test_boot_held_and_clock_wrap(void)
{
    moby_controls_t controls;
    const uint32_t start = UINT32_MAX - 10;
    moby_controls_init(&controls, false, true, start);
    moby_controls_update(&controls, false, true, 18);
    assert(controls.muted);
    moby_controls_update(&controls, false, true, 19);
    assert(!controls.muted && controls.talk_down && !controls.attention);
    moby_controls_update(&controls, false, true, start);
    assert(!controls.muted);
}

static bool pixel_at(const uint8_t *frame, int x, int y)
{
    return (frame[(y / 8) * MOBY_FACE_WIDTH + x] & (1U << (y % 8))) != 0;
}

static void test_face_frames(void)
{
    uint8_t frames[MOBY_FACE_COUNT][MOBY_FACE_BYTES];
    uint8_t guarded[MOBY_FACE_BYTES + 2];
    memset(guarded, 0xa5, sizeof guarded);

    for (int index = 0; index < MOBY_FACE_COUNT; ++index) {
        const moby_face_view_t view = { .expression = (moby_expression_t)index };
        moby_face_render(guarded + 1, view, false);
        assert(guarded[0] == 0xa5 && guarded[MOBY_FACE_BYTES + 1] == 0xa5);
        memcpy(frames[index], guarded + 1, MOBY_FACE_BYTES);
        for (int previous = 0; previous < index; ++previous) {
            assert(memcmp(frames[index], frames[previous], MOBY_FACE_BYTES) != 0);
        }
    }
    assert(pixel_at(frames[MOBY_FACE_IDLE], 40, 14));
    assert(!pixel_at(frames[MOBY_FACE_IDLE], 40, 13));
    assert(pixel_at(frames[MOBY_FACE_MUTED], 57, 55));

    uint8_t frame[MOBY_FACE_BYTES];
    moby_face_render(frame, (moby_face_view_t){ .expression = MOBY_FACE_DISCONNECTED }, true);
    assert(!pixel_at(frame, 40, 17) && pixel_at(frame, 40, 28) && pixel_at(frame, 56, 50));
    moby_face_render(frame, (moby_face_view_t){ .expression = MOBY_FACE_DISCONNECTED, .attention = true }, false);
    assert(pixel_at(frame, 40, 10) && pixel_at(frame, 56, 50));
    moby_face_render(frame, (moby_face_view_t){ .expression = MOBY_FACE_MUTED, .attention = true }, true);
    assert(memcmp(frame, frames[MOBY_FACE_MUTED], MOBY_FACE_BYTES) == 0);
    moby_face_render(frame, (moby_face_view_t){ .expression = MOBY_FACE_COUNT }, false);
    assert(memcmp(frame, frames[MOBY_FACE_ERROR], MOBY_FACE_BYTES) == 0);
}

int main(void)
{
    test_talk_debounce();
    test_mute_requires_new_press();
    test_press_started_during_mute();
    test_boot_held_and_clock_wrap();
    test_face_frames();
    puts("5 F1 host tests passed");
    return 0;
}
