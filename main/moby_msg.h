// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_MSG_H
#define MOBY_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "moby_usb_codec.h"

// Control messages as C structs. moby_json.c turns JSON into moby_msg_t (what
// the desktop may send) and moby_out_t into JSON (what the robot sends). The
// session works on these structs only, so it needs neither cJSON nor the codec.

enum {
    MOBY_ID_MAX = 64,
    MOBY_SCREEN_TEXT_MAX_BYTES = MOBY_USB_SCREEN_TEXT_MAX_CHARS * 4,
    MOBY_DEVICE_ID_LEN = 36,
};

// Capability bits. The names on the wire are `usb_pcm` and `servo_pose`. This
// stage has neither, so the default image advertises an empty list.
enum {
    MOBY_CAP_USB_PCM = 1U << 0,
    MOBY_CAP_SERVO_POSE = 1U << 1,
};

typedef struct {
    char device_id[MOBY_ID_MAX + 1];
    char hw_rev[MOBY_USB_VERSION_TEXT_MAX_CHARS + 1];
    char fw_version[MOBY_USB_VERSION_TEXT_MAX_CHARS + 1];
    uint32_t capabilities;
} moby_identity_t;

typedef enum {
    MOBY_MSG_HELLO_ACK,
    MOBY_MSG_EXPRESSION,
    MOBY_MSG_DISPLAY_TEXT,
    MOBY_MSG_AUDIO_PLAY_START,
    MOBY_MSG_AUDIO_PLAY_END,
    MOBY_MSG_CANCEL,
    MOBY_MSG_SET_CONFIG,
    MOBY_MSG_SERVO_POSE,
    MOBY_MSG_PAIR_BEGIN,
    MOBY_MSG_PAIR_SET_KEY,
} moby_msg_name_t;

// The fields of a set_config or hello_ack config that were present. Null counts as absent.
typedef struct {
    bool has_wake_sensitivity;
    bool has_speaker_volume;
    bool has_vad_silence_ms;
    bool has_max_utterance_ms;
    uint8_t wake_sensitivity;
    uint8_t speaker_volume;
    uint16_t vad_silence_ms;
    uint16_t max_utterance_ms;
} moby_config_patch_t;

typedef struct {
    moby_msg_name_t name;
    char id[MOBY_ID_MAX + 1];
    uint32_t ts_ms;
    union {
        struct {
            bool accepted;
            bool has_session_id;
            bool has_proof;
            // Present exactly when proof is; the parser rejects one without the other.
            bool has_desktop_nonce;
            char session_id[MOBY_ID_MAX + 1];
            uint8_t proof[MOBY_USB_PROOF_BYTES];
            uint8_t desktop_nonce[MOBY_USB_NONCE_BYTES];
            moby_config_patch_t config;
        } hello_ack;
        struct {
            moby_expression state;
            char animation[MOBY_ID_MAX + 1];
            uint32_t duration_ms;
        } expression;
        struct {
            char text[MOBY_SCREEN_TEXT_MAX_BYTES + 1];
            uint32_t ttl_ms;
        } display_text;
        struct {
            uint32_t stream_id;
        } cancel;
        moby_config_patch_t set_config;
        struct {
            char pose[MOBY_ID_MAX + 1];
            uint32_t duration_ms;
        } servo_pose;
        struct {
            uint32_t timeout_ms;
        } pair_begin;
        struct {
            uint8_t key[MOBY_USB_PAIR_KEY_BYTES];
        } pair_set_key;
    } u;
} moby_msg_t;

typedef enum {
    MOBY_OUT_HELLO,
    MOBY_OUT_HEARTBEAT,
    MOBY_OUT_BUTTON_EVENT,
    MOBY_OUT_MUTE_CHANGED,
    MOBY_OUT_ERROR,
    MOBY_OUT_PAIR_BUTTON_CONFIRMED,
    MOBY_OUT_PAIR_RESET,
    MOBY_OUT_SESSION_PROOF,
    MOBY_OUT_COMMAND_OK,
    MOBY_OUT_COMMAND_ERROR,
    MOBY_OUT_AUDIO_START,
    MOBY_OUT_AUDIO_END,
} moby_out_kind_t;

// The protocol's words for audio_start.reason and audio_end.reason, so a message can be built byte for byte like
// the shared vectors. This firmware starts a stream for a button press only and ends it for the reasons it has.
typedef enum {
    MOBY_AUDIO_START_WAKE,
    MOBY_AUDIO_START_BUTTON,
} moby_audio_start_reason_t;

typedef enum {
    MOBY_AUDIO_END_VAD_SILENCE,
    MOBY_AUDIO_END_MAX_DURATION,
    MOBY_AUDIO_END_BUTTON_RELEASE,
    MOBY_AUDIO_END_MUTE,
    MOBY_AUDIO_END_CANCEL,
    MOBY_AUDIO_END_ERROR,
} moby_audio_end_reason_t;

typedef enum {
    MOBY_BUTTON_PRESS,
    MOBY_BUTTON_RELEASE,
    MOBY_BUTTON_LONG_PRESS,
} moby_button_action_t;

// A message the robot wants to send. For command_ok and command_error, id is the command's id.
typedef struct {
    moby_out_kind_t kind;
    uint32_t ts_ms;
    char id[MOBY_ID_MAX + 1];
    union {
        struct {
            uint8_t nonce[MOBY_USB_NONCE_BYTES];
            bool mute;
            bool paired;
        } hello;
        struct {
            moby_expression state;
            bool mute;
            uint32_t uptime_ms;
        } heartbeat;
        struct {
            moby_button_action_t action;
        } button;
        struct {
            bool muted;
            bool boot;
        } mute;
        // The stream is always pcm_s16le, 16000 Hz, mono, 20 ms frames (see moby_pcm.h).
        struct {
            uint16_t stream_id;
            moby_audio_start_reason_t reason;
            uint32_t preroll_ms;
        } audio_start;
        struct {
            uint16_t stream_id;
            moby_audio_end_reason_t reason;
            uint32_t frames;
            uint32_t duration_ms;
        } audio_end;
        // The robot's proof of the pair key (see moby_session.c). A MAC, not a secret, once sent.
        struct {
            uint8_t proof[MOBY_USB_PROOF_BYTES];
        } session_proof;
        // code and detail point at string literals; detail never carries a key or proof.
        struct {
            const char *code;
            const char *detail;
        } error;
    } u;
} moby_out_t;

#endif
