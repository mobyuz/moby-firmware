// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_SESSION_H
#define MOBY_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "moby_face.h"
#include "moby_msg.h"

// The session between the robot and the desktop. It is plain C with no
// ESP-IDF header, no clock and no I/O: callers pass the time in milliseconds
// (it may wrap), feed it parsed messages and control readings, and pull what to
// send. The hardware mute always wins, and nothing here records audio.

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
} moby_session_ops_t;

typedef struct {
    uint32_t rx_sequence_gaps;
    uint32_t outbox_dropped;
    uint32_t errors_suppressed;
} moby_session_stats_t;

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

// Always false: this stage has no audio path, so no code sets it.
bool moby_session_recording(const moby_session_t *session);

#endif
