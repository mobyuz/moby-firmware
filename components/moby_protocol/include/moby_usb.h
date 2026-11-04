/* Generated from Rust. Run npm run generate. */
#ifndef MOBY_USB_H
#define MOBY_USB_H

#define MOBY_USB_PROTOCOL_VERSION 1u
#define MOBY_USB_FRAME_VERSION 1u
#define MOBY_USB_DELIMITER 0u
#define MOBY_USB_HEADER_LEN 12u
#define MOBY_USB_CRC_LEN 4u
#define MOBY_USB_MAX_PAYLOAD_LEN 1024u
#define MOBY_USB_MAX_DECODED_LEN 1040u
#define MOBY_USB_MAX_ENCODED_LEN 1045u
#define MOBY_USB_MAX_WIRE_LEN 1046u
#define MOBY_USB_VERSION_OFFSET 0u
#define MOBY_USB_KIND_OFFSET 1u
#define MOBY_USB_FLAGS_OFFSET 2u
#define MOBY_USB_RESERVED_OFFSET 3u
#define MOBY_USB_STREAM_ID_OFFSET 4u
#define MOBY_USB_SEQUENCE_OFFSET 6u
#define MOBY_USB_LENGTH_OFFSET 10u
#define MOBY_USB_CONTROL 1u
#define MOBY_USB_ROBOT_AUDIO 2u
#define MOBY_USB_DESKTOP_AUDIO 3u

/* Inclusive field bounds. Text bounds count code points, up to 4 UTF-8 bytes each. */
#define MOBY_USB_AUDIO_STREAM_ID_MIN 1u
#define MOBY_USB_AUDIO_STREAM_ID_MAX 65535u
#define MOBY_USB_ACTION_DURATION_MS_MIN 0u
#define MOBY_USB_ACTION_DURATION_MS_MAX 60000u
#define MOBY_USB_TEXT_TTL_MS_MIN 100u
#define MOBY_USB_TEXT_TTL_MS_MAX 60000u
#define MOBY_USB_PAIR_TIMEOUT_MS_MIN 1000u
#define MOBY_USB_PAIR_TIMEOUT_MS_MAX 30000u
#define MOBY_USB_SAMPLE_RATE_MIN 8000u
#define MOBY_USB_SAMPLE_RATE_MAX 48000u
#define MOBY_USB_CHANNEL_COUNT_MIN 1u
#define MOBY_USB_CHANNEL_COUNT_MAX 2u
#define MOBY_USB_FRAME_MS_MIN 10u
#define MOBY_USB_FRAME_MS_MAX 60u
#define MOBY_USB_PREROLL_MS_MIN 0u
#define MOBY_USB_PREROLL_MS_MAX 2000u
#define MOBY_USB_PERCENT_MIN 0u
#define MOBY_USB_PERCENT_MAX 100u
#define MOBY_USB_VAD_SILENCE_MS_MIN 200u
#define MOBY_USB_VAD_SILENCE_MS_MAX 5000u
#define MOBY_USB_MAX_UTTERANCE_MS_MIN 1000u
#define MOBY_USB_MAX_UTTERANCE_MS_MAX 60000u
#define MOBY_USB_SCREEN_TEXT_MIN_CHARS 1u
#define MOBY_USB_SCREEN_TEXT_MAX_CHARS 64u
#define MOBY_USB_ERROR_DETAIL_MIN_CHARS 1u
#define MOBY_USB_ERROR_DETAIL_MAX_CHARS 128u
#define MOBY_USB_VERSION_TEXT_MIN_CHARS 1u
#define MOBY_USB_VERSION_TEXT_MAX_CHARS 32u
#define MOBY_USB_CAPABILITIES_MAX 8u
#define MOBY_USB_NONCE_BYTES 16u
#define MOBY_USB_PROOF_BYTES 32u
#define MOBY_USB_PAIR_KEY_BYTES 32u

/* Control message names (the envelope `name`). */
#define MOBY_USB_MSG_HELLO "hello" /* event, from the robot */
#define MOBY_USB_MSG_HEARTBEAT "heartbeat" /* event, from the robot */
#define MOBY_USB_MSG_WAKE_DETECTED "wake_detected" /* event, from the robot */
#define MOBY_USB_MSG_BUTTON_EVENT "button_event" /* event, from the robot */
#define MOBY_USB_MSG_MUTE_CHANGED "mute_changed" /* event, from the robot */
#define MOBY_USB_MSG_AUDIO_START "audio_start" /* event, from the robot */
#define MOBY_USB_MSG_AUDIO_END "audio_end" /* event, from the robot */
#define MOBY_USB_MSG_ERROR "error" /* event, from the robot */
#define MOBY_USB_MSG_PAIR_BUTTON_CONFIRMED "pair_button_confirmed" /* event, from the robot */
#define MOBY_USB_MSG_PAIR_RESET "pair_reset" /* event, from the robot */
#define MOBY_USB_MSG_HELLO_ACK "hello_ack" /* response, from the desktop */
#define MOBY_USB_MSG_EXPRESSION "expression" /* command, from the desktop */
#define MOBY_USB_MSG_DISPLAY_TEXT "display_text" /* command, from the desktop */
#define MOBY_USB_MSG_AUDIO_PLAY_START "audio_play_start" /* command, from the desktop */
#define MOBY_USB_MSG_AUDIO_PLAY_END "audio_play_end" /* command, from the desktop */
#define MOBY_USB_MSG_CANCEL "cancel" /* command, from the desktop */
#define MOBY_USB_MSG_SET_CONFIG "set_config" /* command, from the desktop */
#define MOBY_USB_MSG_SERVO_POSE "servo_pose" /* command, from the desktop */
#define MOBY_USB_MSG_PAIR_BEGIN "pair_begin" /* command, from the desktop */
#define MOBY_USB_MSG_PAIR_SET_KEY "pair_set_key" /* command, from the desktop */
#define MOBY_USB_MSG_COMMAND_OK "command_ok" /* response, from the robot */
#define MOBY_USB_MSG_COMMAND_ERROR "command_error" /* response, from the robot */

/* Error codes in `error` events and `command_error` responses. */
#define MOBY_USB_E_PROTO_VERSION "E_PROTO_VERSION"
#define MOBY_USB_E_BAD_FRAME "E_BAD_FRAME"
#define MOBY_USB_E_CRC "E_CRC"
#define MOBY_USB_E_STATE "E_STATE"
#define MOBY_USB_E_MUTED "E_MUTED"
#define MOBY_USB_E_BUSY "E_BUSY"
#define MOBY_USB_E_AUDIO_OVERRUN "E_AUDIO_OVERRUN"
#define MOBY_USB_E_TIMEOUT "E_TIMEOUT"
#define MOBY_USB_E_UNSUPPORTED "E_UNSUPPORTED"
#define MOBY_USB_E_HW "E_HW"

/* Face states in the order of the transition table below. */
typedef enum moby_expression {
    MOBY_EXPRESSION_IDLE = 0,
    MOBY_EXPRESSION_LISTENING = 1,
    MOBY_EXPRESSION_THINKING = 2,
    MOBY_EXPRESSION_SPEAKING = 3,
    MOBY_EXPRESSION_HAPPY = 4,
    MOBY_EXPRESSION_CONFUSED = 5,
    MOBY_EXPRESSION_ERROR = 6,
    MOBY_EXPRESSION_MUTED = 7,
    MOBY_EXPRESSION_DISCONNECTED = 8,
    MOBY_EXPRESSION_COUNT = 9
} moby_expression;

static inline const char *moby_expression_name(moby_expression state) {
    static const char *const names[MOBY_EXPRESSION_COUNT] = {
        "idle",
        "listening",
        "thinking",
        "speaking",
        "happy",
        "confused",
        "error",
        "muted",
        "disconnected",
    };
    return (unsigned)state < MOBY_EXPRESSION_COUNT ? names[state] : "";
}

/* Row = old state, bit = new state. Generated from Rust can_transition. */
static inline bool moby_can_transition(moby_expression from, moby_expression to) {
    static const unsigned short rows[MOBY_EXPRESSION_COUNT] = {
        0x01f7u, /* idle */
        0x01c7u, /* listening */
        0x01fdu, /* thinking */
        0x01fbu, /* speaking */
        0x01d7u, /* happy */
        0x01e7u, /* confused */
        0x01c7u, /* error */
        0x01c1u, /* muted */
        0x01c1u, /* disconnected */
    };
    return (unsigned)from < MOBY_EXPRESSION_COUNT && (unsigned)to < MOBY_EXPRESSION_COUNT &&
           ((rows[from] >> to) & 1u) != 0;
}

#endif /* MOBY_USB_H */
