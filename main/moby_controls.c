// SPDX-License-Identifier: GPL-3.0-only

#include "moby_controls.h"

enum { DEBOUNCE_MS = 30 };

void moby_controls_init(moby_controls_t *controls, bool muted, bool talk, uint32_t now_ms)
{
    *controls = (moby_controls_t){
        .muted = true,
        .talk_down = talk,
        .raw_muted = muted,
        .raw_talk = talk,
        .mute_changed_ms = now_ms,
        .talk_changed_ms = now_ms,
    };
}

void moby_controls_update(moby_controls_t *controls, bool muted, bool talk, uint32_t now_ms)
{
    if (muted != controls->raw_muted) {
        controls->raw_muted = muted;
        controls->mute_changed_ms = now_ms;
    }

    const bool was_muted = controls->muted;
    if (muted) {
        controls->muted = true;
    } else if (was_muted && now_ms - controls->mute_changed_ms >= DEBOUNCE_MS) {
        controls->muted = false;
    }
    if (talk != controls->raw_talk) {
        controls->raw_talk = talk;
        controls->talk_changed_ms = now_ms;
        controls->talk_allowed = talk && !was_muted && !controls->muted;
    }
    if (controls->muted) {
        controls->attention = false;
        controls->talk_allowed = false;
    }
    if (talk != controls->talk_down && now_ms - controls->talk_changed_ms >= DEBOUNCE_MS) {
        controls->talk_down = talk;
        controls->attention = talk && controls->talk_allowed;
    }
}
