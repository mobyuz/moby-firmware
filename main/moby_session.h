// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_SESSION_H
#define MOBY_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "moby_face.h"
#include "moby_msg.h"
#include "moby_pcm.h"

// The session between the robot and the desktop. It is plain C with no
// ESP-IDF header, no clock and no I/O: callers pass the time in milliseconds
// (it may wrap), feed it parsed messages and control readings, and pull what to
// send. The hardware mute always wins.
//
// Push to talk. The session alone decides when the microphone may run. A stream
// is open only between a talk press that was accepted (see can_start in the .c
// file) and its end. The one fact "capturing" lives in stream.phase and nowhere
// else: the platform's capture switch, the red LED and the listening face are
// all derived from it, through ops.capture_set and moby_session_recording, so
// they cannot disagree.

enum {
    MOBY_HELLO_RETRY_MS = 1000,
    MOBY_HANDSHAKE_MS = 3000,
    MOBY_HEARTBEAT_MS = 2000,
    // Heartbeats that go unanswered before the desktop counts as gone.
    MOBY_LIVENESS_MISSED = 3,
    MOBY_LONG_PRESS_MS = 10000,
    MOBY_ERROR_FACE_MS = 3000,
    // Unsolicited protocol errors are limited to one per this many milliseconds.
    MOBY_ERROR_GAP_MS = 500,
    MOBY_OUTBOX_LEN = 8,
    // After a button release or the time limit, the audio already captured is still sent. If the link does not
    // take it within this time, the stream ends with an error and the rest is discarded.
    MOBY_DRAIN_MS = 1000,
};

typedef enum {
    MOBY_SESSION_BOOT,
    MOBY_SESSION_DISCONNECTED,
    MOBY_SESSION_PAIRING,
    MOBY_SESSION_IDLE,
    MOBY_SESSION_LISTENING,
    MOBY_SESSION_THINKING,
    MOBY_SESSION_SPEAKING,
} moby_session_state_t;

// What the session needs from the platform. log may be NULL. A log line never
// carries a key, a nonce or a proof.
typedef struct {
    void *ctx;
    bool (*fill_random)(void *ctx, uint8_t *out, size_t length);
    bool (*hmac_sha256)(void *ctx, const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                        uint8_t out[32]);
    bool (*store_key)(void *ctx, const uint8_t key[MOBY_USB_PAIR_KEY_BYTES]);
    bool (*erase_key)(void *ctx);
    void (*log)(void *ctx, const char *line);
    // Switches capture on or off: the microphone clocks, the red LED and the sample gate. Called only when the
    // state changes, and with `false` before anything else on every stop. Without it (and without the usb_pcm
    // capability) the session never opens a stream. It must not block.
    void (*capture_set)(void *ctx, bool on);
    // Throws away and zeroes every buffered sample. Called when a stream ends without being sent in full, and
    // whenever the session ends.
    void (*capture_discard)(void *ctx);
} moby_session_ops_t;

typedef struct {
    uint32_t rx_sequence_gaps;
    uint32_t outbox_dropped;
    uint32_t errors_suppressed;
    uint32_t streams_started;
    uint32_t stream_frames_sent;
    uint32_t stream_overruns;
    uint32_t stream_faults;
    uint32_t stream_start_lost;
} moby_session_stats_t;

typedef enum {
    MOBY_STREAM_NONE,
    // The button is held: capture is on and the red LED is lit.
    MOBY_STREAM_CAPTURING,
    // Capture is off; the samples already buffered are being sent before audio_end.
    MOBY_STREAM_DRAINING,
} moby_stream_phase_t;

// What the link needs to know to send audio frames.
typedef struct {
    bool open;
    bool draining;
    uint16_t stream_id;
    // The sequence number of the next frame of this stream; frames start at 0 and have no gaps.
    uint32_t sequence;
} moby_stream_info_t;

typedef struct {
    moby_session_ops_t ops;
    moby_identity_t identity;
    moby_session_state_t state;
    moby_expression face;
    struct {
        uint8_t wake_sensitivity;
        uint8_t speaker_volume;
        uint16_t vad_silence_ms;
        uint16_t max_utterance_ms;
    } config;

    bool link_up;
    bool muted;
    bool talk_down;
    bool attention;
    bool press_announced;
    bool long_fired;
    uint32_t talk_since_ms;

    // Handshake.
    bool hello_open;
    bool hello_rejected;
    uint8_t nonce[MOBY_USB_NONCE_BYTES];
    char hello_id[MOBY_ID_MAX + 1];
    uint32_t attempt_ms;
    uint32_t hello_sent_ms;
    bool established;
    bool authenticated;
    char session_id[MOBY_ID_MAX + 1];
    uint32_t event_counter;

    // Liveness.
    uint32_t next_heartbeat_ms;
    unsigned unanswered;
    bool rx_sequence_known;
    uint32_t rx_sequence_next;

    // Pairing.
    bool paired;
    uint8_t pair_key[MOBY_USB_PAIR_KEY_BYTES];
    bool pair_confirmed;
    uint32_t pair_deadline_ms;

    // Face.
    bool error_active;
    uint32_t error_until_ms;
    bool expression_timed;
    uint32_t expression_until_ms;
    char text[MOBY_USB_SCREEN_TEXT_MAX_CHARS + 1];
    uint32_t text_until_ms;

    bool error_sent;
    uint32_t error_sent_ms;

    // Push to talk.
    struct {
        moby_stream_phase_t phase;
        uint16_t id;
        uint16_t last_id;
        uint32_t frames;
        uint32_t started_ms;
        uint32_t drain_since_ms;
        // The audio_end that is owed to the desktop, until it fits into the outbox.
        bool end_owed;
        moby_audio_end_reason_t end_reason;
        uint16_t end_id;
        uint32_t end_frames;
    } stream;

    moby_out_t outbox[MOBY_OUTBOX_LEN];
    unsigned outbox_head;
    unsigned outbox_count;
    moby_session_stats_t stats;
} moby_session_t;

// pair_key is NULL when the robot is unpaired. The session starts in BOOT with
// the hardware mute assumed on, like the controls do, until a reading says otherwise.
void moby_session_init(moby_session_t *session, const moby_identity_t *identity, const uint8_t *pair_key,
                       const moby_session_ops_t *ops, uint32_t now_ms);

// The USB link came up or went down. Coming up sends hello at once.
void moby_session_set_link(moby_session_t *session, uint32_t now_ms, bool up);

// The debounced controls. The mute is applied first, before anything else.
void moby_session_controls(moby_session_t *session, uint32_t now_ms, bool muted, bool talk_down, bool attention);

// Timers: hello retries, the handshake window, heartbeats, liveness, and expiry.
void moby_session_tick(moby_session_t *session, uint32_t now_ms);

// A CRC-valid frame arrived from the desktop. Counts as proof of life.
void moby_session_note_frame(moby_session_t *session, uint32_t sequence);

void moby_session_on_message(moby_session_t *session, uint32_t now_ms, const moby_msg_t *msg);

// Rejects a command that could not be parsed into a message: command_error for
// a command id, and an unsolicited error event otherwise.
void moby_session_reject(moby_session_t *session, uint32_t now_ms, const char *command_id, const char *code,
                         const char *detail);

// Sends an unsolicited error event, rate limited.
void moby_session_protocol_error(moby_session_t *session, uint32_t now_ms, const char *code, const char *detail);

bool moby_session_pop(moby_session_t *session, moby_out_t *out);

// What the face should show now. The mute wins over everything, a disconnected
// robot shows disconnected, and display text shows only while connected and unmuted.
void moby_session_view(const moby_session_t *session, moby_face_view_t *view);

// True exactly while capture is on (stream.phase is capturing). The red LED, the microphone and the listening face
// follow this value and nothing else.
bool moby_session_recording(const moby_session_t *session);

// For the link: whether audio frames may flow, with the stream id and the next sequence number.
void moby_session_stream_info(const moby_session_t *session, moby_stream_info_t *info);

// A frame of the open stream was written to the transport.
void moby_session_stream_frame_sent(moby_session_t *session);

// The link has sent every whole frame of a draining stream: the stream ends with its audio_end.
void moby_session_stream_drained(moby_session_t *session, uint32_t now_ms);

// MOBY_PCM_EVENT_* bits from the sample source: an overrun or a hardware fault ends the stream with an error.
void moby_session_stream_events(moby_session_t *session, uint32_t now_ms, uint32_t events);

// The audio_start of the open stream could not be written, so the desktop must never see its frames: the stream
// is dropped and nothing more is sent for it.
void moby_session_stream_start_lost(moby_session_t *session, uint32_t now_ms);

#endif
