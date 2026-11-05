// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_CAPTURE_H
#define MOBY_CAPTURE_H

#include <stdbool.h>

#include "esp_err.h"
#include "moby_pcm.h"
#include "moby_session.h"

// The microphone side of push to talk (F4): an I2S capture task, a ring buffer in PSRAM and the red LED. It does
// nothing by itself. The session decides, through capture_set, and everything here follows that one switch:
// the sample gate, the LED and the I2S clocks. Between streams the I2S channel does not exist, so the microphone
// gets no clock. Compiled only when CONFIG_MOBY_F4_PTT_AUDIO is on.

// Allocates the ring (PSRAM, internal RAM as a fallback is refused) and starts the idle capture task. No I2S
// resource is created here. Fails without side effects on the face, the controls or the mute.
esp_err_t moby_capture_init(void);

// The sample source for the link, and the two session callbacks. All are cheap and never wait on I2S or the host.
const moby_pcm_source_t *moby_capture_source(void);
void moby_capture_set(void *ctx, bool on);
void moby_capture_discard(void *ctx);

#endif
