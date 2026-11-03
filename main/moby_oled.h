// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_OLED_H
#define MOBY_OLED_H

// Run as a task. The queue contains the most recent moby_face_view_t.
void moby_oled_task(void *view_queue);

#endif
