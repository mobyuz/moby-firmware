// SPDX-License-Identifier: GPL-3.0-only

#include "moby_face.h"

#include <string.h>

static void pixel(uint8_t *frame, int x, int y)
{
    // SSD1306 pages store eight vertical pixels per column byte.
    frame[(y / 8) * MOBY_FACE_WIDTH + x] |= (uint8_t)(1U << (y % 8));
}

static void rounded_rect(uint8_t *frame, int x, int y, int width, int height, int radius)
{
    for (int row = 0; row < height; ++row) {
        const int edge = row < height / 2 ? row : height - 1 - row;
        const int inset = edge < radius ? radius - edge : 0;
        for (int col = inset; col < width - inset; ++col) {
            pixel(frame, x + col, y + row);
        }
    }
}

static void eye(uint8_t *frame, int center, int width, int height)
{
    rounded_rect(frame, center - width / 2, 28 - height / 2, width, height, 4);
}

void moby_face_render(uint8_t frame[MOBY_FACE_BYTES], moby_face_view_t view, bool blink)
{
    static const struct {
        uint8_t width;
        uint8_t left_height;
        uint8_t right_height;
    } eyes[MOBY_FACE_COUNT] = {
        [MOBY_FACE_IDLE] = {24, 28, 28},
        [MOBY_FACE_LISTENING] = {28, 36, 36},
        [MOBY_FACE_THINKING] = {20, 14, 20},
        [MOBY_FACE_SPEAKING] = {24, 22, 22},
        [MOBY_FACE_CONFUSED] = {24, 18, 30},
        [MOBY_FACE_MUTED] = {24, 6, 6},
        [MOBY_FACE_DISCONNECTED] = {22, 24, 24},
    };
    enum { LEFT_EYE = 40, RIGHT_EYE = 88 };

    memset(frame, 0, MOBY_FACE_BYTES);
    if ((unsigned)view.expression >= MOBY_FACE_COUNT) {
        view.expression = MOBY_FACE_ERROR;
    }

    if (view.expression == MOBY_FACE_HAPPY) {
        for (int x = -12; x <= 12; ++x) {
            const int y = 21 + x * x / 16;
            rounded_rect(frame, LEFT_EYE + x, y, 1, 4, 0);
            rounded_rect(frame, RIGHT_EYE + x, y, 1, 4, 0);
        }
    } else if (view.expression == MOBY_FACE_ERROR) {
        for (int offset = -9; offset <= 9; ++offset) {
            rounded_rect(frame, LEFT_EYE + offset, 28 + offset, 2, 2, 0);
            rounded_rect(frame, LEFT_EYE + offset, 28 - offset, 2, 2, 0);
            rounded_rect(frame, RIGHT_EYE + offset, 28 + offset, 2, 2, 0);
            rounded_rect(frame, RIGHT_EYE + offset, 28 - offset, 2, 2, 0);
        }
    } else if (view.expression == MOBY_FACE_DISCONNECTED && view.attention) {
        eye(frame, LEFT_EYE, 28, 36);
        eye(frame, RIGHT_EYE, 28, 36);
    } else {
        const bool closed = blink && (view.expression == MOBY_FACE_IDLE || view.expression == MOBY_FACE_DISCONNECTED);
        eye(frame, LEFT_EYE, eyes[view.expression].width, closed ? 6 : eyes[view.expression].left_height);
        eye(frame, RIGHT_EYE, eyes[view.expression].width, closed ? 6 : eyes[view.expression].right_height);
    }

    switch (view.expression) {
    case MOBY_FACE_THINKING:
        for (int x = 54; x <= 70; x += 8) {
            rounded_rect(frame, x, 50, 4, 4, 1);
        }
        break;
    case MOBY_FACE_SPEAKING:
        rounded_rect(frame, 58, 46, 12, 8, 3);
        break;
    case MOBY_FACE_MUTED:
        rounded_rect(frame, 61, 44, 6, 11, 2);
        rounded_rect(frame, 60, 55, 8, 2, 0);
        for (int offset = 0; offset < 14; ++offset) {
            pixel(frame, 57 + offset, 55 - offset);
        }
        break;
    case MOBY_FACE_DISCONNECTED:
        rounded_rect(frame, 52, 49, 8, 4, 1);
        rounded_rect(frame, 68, 49, 8, 4, 1);
        break;
    default:
        break;
    }
}
