// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_LINK_TASK_H
#define MOBY_LINK_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "moby_controls.h"
#include "moby_face.h"

// Starts the F3 USB link: NVS identity, the USB-Serial-JTAG transport, and a
// task that runs moby_link_step. Fails without side effects on the face, the
// controls or the mute, which keep working with no link.
esp_err_t moby_link_task_start(void);

// Feeds the debounced controls to the session. Never blocks for long.
void moby_link_task_controls(const moby_controls_t *controls, uint32_t now_ms);

// Copies what the session wants the face to show. False before the link starts
// or when the session is busy this instant, and the caller keeps its own view.
bool moby_link_task_view(moby_face_view_t *view);

#endif
