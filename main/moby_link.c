// SPDX-License-Identifier: GPL-3.0-only

#include "moby_link.h"

#include <string.h>

#include "moby_json.h"

// Bytes read in one pass: enough for two maximum frames, so a busy host cannot starve the timers.
enum { READ_CHUNK = 64, READ_BUDGET = 2048 };

void moby_link_init(moby_link_t *link, const moby_transport_t *transport, moby_session_t *session)
{
    memset(link, 0, sizeof *link);
    link->transport = transport;
    link->session = session;
    moby_decoder_reset(&link->decoder);
}

static void handle_control(moby_link_t *link, uint32_t now_ms, const moby_frame *frame)
{
    moby_parsed_t parsed;
    const moby_parse_status_t status = moby_json_parse(frame->payload, frame->payload_length, &parsed);
    if (status == MOBY_PARSE_OK) {
        moby_session_on_message(link->session, now_ms, &parsed.msg);
        return;
    }
    ++link->stats.rejected_messages;
    const char *code = MOBY_USB_E_BAD_FRAME;
    const char *detail = "invalid control message";
    if (status == MOBY_PARSE_VERSION) {
        code = MOBY_USB_E_PROTO_VERSION;
        detail = "unsupported envelope version";
    } else if (status == MOBY_PARSE_UNKNOWN_NAME) {
        code = MOBY_USB_E_UNSUPPORTED;
        detail = "unknown message name";
    }
    moby_session_reject(link->session, now_ms, parsed.has_id && parsed.command ? parsed.msg.id : NULL, code, detail);
}

static void handle_frame(moby_link_t *link, uint32_t now_ms, const moby_frame *frame)
{
    ++link->stats.frames_received;
    moby_session_note_frame(link->session, frame->sequence);
    if (frame->kind == MOBY_USB_CONTROL) {
        handle_control(link, now_ms, frame);
    } else if (frame->kind == MOBY_USB_DESKTOP_AUDIO) {
        // This stage has no audio path, so desktop audio is dropped unread.
        ++link->stats.desktop_audio_rejected;
        moby_session_protocol_error(link->session, now_ms, MOBY_USB_E_UNSUPPORTED, "desktop audio unsupported");
    } else {
        moby_session_protocol_error(link->session, now_ms, MOBY_USB_E_BAD_FRAME, "robot audio kind from desktop");
    }
}

static void handle_decode_error(moby_link_t *link, uint32_t now_ms, moby_status status)
{
    const char *code = MOBY_USB_E_BAD_FRAME;
    const char *detail = "bad frame";
    if (status == MOBY_ERR_BAD_CRC) {
        ++link->stats.bad_crc;
        code = MOBY_USB_E_CRC;
        detail = "frame checksum mismatch";
    } else if (status == MOBY_ERR_UNSUPPORTED_VERSION) {
        ++link->stats.bad_version;
        code = MOBY_USB_E_PROTO_VERSION;
        detail = "unsupported frame version";
    } else if (status == MOBY_ERR_FRAME_TOO_LARGE) {
        ++link->stats.oversized_frames;
        detail = "frame too large";
    } else {
        ++link->stats.bad_frames;
    }
    moby_session_protocol_error(link->session, now_ms, code, detail);
}

static void receive(moby_link_t *link, uint32_t now_ms)
{
    uint8_t chunk[READ_CHUNK];
    size_t budget = READ_BUDGET;
    while (budget > 0) {
        const size_t count = link->transport->read(link->transport->ctx, chunk, budget < READ_CHUNK ? budget : READ_CHUNK);
        if (count == 0) {
            return;
        }
        budget = count < budget ? budget - count : 0;
        for (size_t i = 0; i < count; ++i) {
            moby_frame frame;
            const moby_status status = moby_decoder_push(&link->decoder, chunk[i], &frame);
            if (status == MOBY_OK) {
                handle_frame(link, now_ms, &frame);
            } else if (status != MOBY_PENDING) {
                handle_decode_error(link, now_ms, status);
            }
        }
    }
}

static void transmit(moby_link_t *link)
{
    moby_out_t out;
    while (moby_session_pop(link->session, &out)) {
        static const uint8_t SYNC = MOBY_USB_DELIMITER;
        if (link->needs_sync && link->transport->write(link->transport->ctx, &SYNC, 1)) {
            link->needs_sync = false;
        }
        const size_t length = moby_json_build(&link->session->identity, &out, (char *)link->payload, sizeof link->payload);
        size_t wire_length = 0;
        // The sequence is spent even when a frame is dropped, so the desktop sees the gap.
        const uint32_t sequence = link->tx_sequence++;
        if (length == 0 ||
            moby_frame_build(MOBY_USB_CONTROL, 0, sequence, link->payload, length, link->wire, sizeof link->wire,
                             &wire_length) != MOBY_OK) {
            ++link->stats.frames_dropped;
            continue;
        }
        // A hello or heartbeat is worth nothing late, so none queues behind an undelivered one.
        const bool disposable = out.kind == MOBY_OUT_HELLO || out.kind == MOBY_OUT_HEARTBEAT;
        const moby_transport_t *transport = link->transport;
        if ((disposable && transport->tx_idle && !transport->tx_idle(transport->ctx)) ||
            !transport->write(transport->ctx, link->wire, wire_length)) {
            ++link->stats.frames_dropped;
            continue;
        }
        ++link->stats.frames_sent;
    }
}

void moby_link_step(moby_link_t *link, uint32_t now_ms)
{
    const bool connected = link->transport->connected(link->transport->ctx);
    if (connected != link->was_connected) {
        link->was_connected = connected;
        link->needs_sync = connected;
        moby_decoder_reset(&link->decoder);
        moby_session_set_link(link->session, now_ms, connected);
    }
    if (connected) {
        receive(link, now_ms);
    }
    moby_session_tick(link->session, now_ms);
    if (connected) {
        transmit(link);
    }
}
