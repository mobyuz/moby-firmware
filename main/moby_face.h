// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_FACE_H
#define MOBY_FACE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    MOBY_FACE_WIDTH = 128,
    MOBY_FACE_HEIGHT = 64,
    MOBY_FACE_BYTES = MOBY_FACE_WIDTH * MOBY_FACE_HEIGHT / 8,
};

typedef enum {
    MOBY_FACE_IDLE,
    MOBY_FACE_LISTENING,
    MOBY_FACE_THINKING,
    MOBY_FACE_SPEAKING,
    MOBY_FACE_HAPPY,
    MOBY_FACE_CONFUSED,
    MOBY_FACE_ERROR,
    MOBY_FACE_MUTED,
    MOBY_FACE_DISCONNECTED,
    MOBY_FACE_COUNT,
} moby_expression_t;

typedef struct {
    moby_expression_t expression;
    bool attention;
} moby_face_view_t;

void moby_face_render(uint8_t frame[MOBY_FACE_BYTES], moby_face_view_t view, bool blink);

#endif
