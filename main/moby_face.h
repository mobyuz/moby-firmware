// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_FACE_H
#define MOBY_FACE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    MOBY_FACE_WIDTH = 128,
    MOBY_FACE_HEIGHT = 64,
    MOBY_FACE_BYTES = MOBY_FACE_WIDTH * MOBY_FACE_HEIGHT / 8,
    // Display text is bounded: at most this many cells (see moby_font.h), drawn in rows of MOBY_FACE_TEXT_COLUMNS.
    MOBY_FACE_TEXT_MAX = 64,
    MOBY_FACE_TEXT_COLUMNS = 21,
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
    // Empty for the plain face. Otherwise the text replaces the eyes until it is empty again.
    char text[MOBY_FACE_TEXT_MAX + 1];
} moby_face_view_t;

static inline bool moby_face_view_equal(const moby_face_view_t *a, const moby_face_view_t *b)
{
    return a->expression == b->expression && a->attention == b->attention && strcmp(a->text, b->text) == 0;
}

void moby_face_render(uint8_t frame[MOBY_FACE_BYTES], moby_face_view_t view, bool blink);

#endif
