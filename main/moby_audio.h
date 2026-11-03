// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_AUDIO_H
#define MOBY_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

enum {
    MOBY_AUDIO_PRESS = 1,
    MOBY_AUDIO_RELEASE = 2,
    MOBY_AUDIO_MUTE = 3,
    MOBY_AUDIO_COMMAND_MASK = 3,
    MOBY_AUDIO_EVENT_STEP = 4,
};

typedef enum {
    MOBY_CAPTURE_ACTIVE,
    MOBY_CAPTURE_RELEASED,
    MOBY_CAPTURE_CANCELLED,
} moby_capture_state_t;

static inline moby_capture_state_t moby_audio_request_state(uint32_t request, uint32_t latest, bool muted)
{
    if (muted) {
        return MOBY_CAPTURE_CANCELLED;
    }
    if (latest == request) {
        return MOBY_CAPTURE_ACTIVE;
    }
    const uint32_t next_sequence = (request & ~(uint32_t)MOBY_AUDIO_COMMAND_MASK) + MOBY_AUDIO_EVENT_STEP;
    return latest == (next_sequence | MOBY_AUDIO_RELEASE) ? MOBY_CAPTURE_RELEASED : MOBY_CAPTURE_CANCELLED;
}

// The low bits of the task notification hold the command, with a sequence above them.
void moby_audio_task(void *unused);

#endif
