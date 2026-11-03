// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_CONTROLS_H
#define MOBY_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool muted;
    bool talk_down;
    bool attention;
    bool raw_muted;
    bool raw_talk;
    bool talk_allowed;
    uint32_t mute_changed_ms;
    uint32_t talk_changed_ms;
} moby_controls_t;

void moby_controls_init(moby_controls_t *controls, bool muted, bool talk, uint32_t now_ms);
void moby_controls_update(moby_controls_t *controls, bool muted, bool talk, uint32_t now_ms);

#endif
