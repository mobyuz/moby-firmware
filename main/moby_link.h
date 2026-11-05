// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_LINK_H
#define MOBY_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "moby_pcm.h"
#include "moby_session.h"
#include "moby_transport.h"
#include "moby_usb_codec.h"

// Moves frames between a transport and a session: decodes with the bounded
// moby_decoder (about 1 KiB), parses control JSON, drains the session's outbox
// into frames. It owns no task and takes no lock, so the host tests drive it
// with a fake transport. moby_link_task.c wraps it for the device.

typedef struct {
    uint32_t frames_received;
    uint32_t frames_sent;
    uint32_t frames_dropped;
    uint32_t bad_frames;
    uint32_t bad_crc;
    uint32_t oversized_frames;
    uint32_t bad_version;
    uint32_t rejected_messages;
    uint32_t desktop_audio_rejected;
    // Audio frames written, and passes where the transport had no room for the next frame (it is tried again).
    uint32_t audio_frames_sent;
    uint32_t audio_stalls;
} moby_link_stats_t;

// Audio frames sent per pass at most, so a long backlog cannot starve the timers or the controls.
enum { MOBY_LINK_AUDIO_FRAMES_PER_PASS = 8 };

typedef struct {
    const moby_transport_t *transport;
    moby_session_t *session;
    moby_decoder decoder;
    uint32_t tx_sequence;
    bool was_connected;
    // Set on every new connection: the first write is a lone delimiter, so the host decoder drops whatever
    // came before it (the ROM prints boot text on this port) instead of gluing it to our first frame.
    bool needs_sync;
    // Where audio samples come from; NULL in an image without audio, and then no audio frame is ever built.
    const moby_pcm_source_t *source;
    // The stream whose audio_start was written. Frames of any other stream are never sent.
    bool start_written;
    uint16_t start_written_id;
    uint8_t payload[MOBY_USB_MAX_PAYLOAD_LEN];
    uint8_t wire[MOBY_USB_MAX_WIRE_LEN];
    moby_link_stats_t stats;
} moby_link_t;

void moby_link_init(moby_link_t *link, const moby_transport_t *transport, moby_session_t *session);

// Gives the link its sample source. Audio frames flow only while the session has a stream open, only after the
// stream's audio_start was written, and never for samples the session did not ask for.
void moby_link_set_source(moby_link_t *link, const moby_pcm_source_t *source);

// One pass: follow the connection, read and decode what is waiting, run the
// session timers, and send what the session queued. now_ms may wrap.
void moby_link_step(moby_link_t *link, uint32_t now_ms);

#endif
