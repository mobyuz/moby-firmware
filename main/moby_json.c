// SPDX-License-Identifier: GPL-3.0-only

#include "moby_json.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "moby_pcm.h"

// The deepest message is hello_ack, whose config object sits in data. cJSON
// recurses once per level, so a hostile payload must not choose the depth.
enum { MAX_DEPTH = 4 };

typedef struct {
    const char *name;
    const char *kind;
    bool desktop;
    moby_msg_name_t message;
} name_row_t;

// Every protocol name. Only the desktop's rows map to a message; the robot's
// own names are known so a reflected event is a wrong kind, not an unknown name.
static const name_row_t NAMES[] = {
    { MOBY_USB_MSG_HELLO, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_HEARTBEAT, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_WAKE_DETECTED, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_BUTTON_EVENT, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_MUTE_CHANGED, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_AUDIO_START, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_AUDIO_END, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_ERROR, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_PAIR_BUTTON_CONFIRMED, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_PAIR_RESET, "event", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_COMMAND_OK, "response", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_COMMAND_ERROR, "response", false, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_HELLO_ACK, "response", true, MOBY_MSG_HELLO_ACK },
    { MOBY_USB_MSG_EXPRESSION, "command", true, MOBY_MSG_EXPRESSION },
    { MOBY_USB_MSG_DISPLAY_TEXT, "command", true, MOBY_MSG_DISPLAY_TEXT },
    { MOBY_USB_MSG_AUDIO_PLAY_START, "command", true, MOBY_MSG_AUDIO_PLAY_START },
    { MOBY_USB_MSG_AUDIO_PLAY_END, "command", true, MOBY_MSG_AUDIO_PLAY_END },
    { MOBY_USB_MSG_CANCEL, "command", true, MOBY_MSG_CANCEL },
    { MOBY_USB_MSG_SET_CONFIG, "command", true, MOBY_MSG_SET_CONFIG },
    { MOBY_USB_MSG_SERVO_POSE, "command", true, MOBY_MSG_SERVO_POSE },
    { MOBY_USB_MSG_PAIR_BEGIN, "command", true, MOBY_MSG_PAIR_BEGIN },
    { MOBY_USB_MSG_PAIR_SET_KEY, "command", true, MOBY_MSG_PAIR_SET_KEY },
};

// Rejects what cJSON would let through: raw control bytes, a \u0000 escape that
// would cut a string short, and nesting deeper than MAX_DEPTH. Works on bytes,
// so it needs no allocation.
static bool prescan(const uint8_t *bytes, size_t length)
{
    unsigned depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = bytes[i];
        if (in_string) {
            if (byte < 0x20) {
                return false;
            }
            if (byte == '\\') {
                if (i + 1 >= length) {
                    return false;
                }
                if (bytes[i + 1] == 'u' && i + 5 < length && memcmp(bytes + i + 2, "0000", 4) == 0) {
                    return false;
                }
                ++i;
            } else if (byte == '"') {
                in_string = false;
            }
        } else if (byte == '"') {
            in_string = true;
        } else if (byte == '{' || byte == '[') {
            if (++depth > MAX_DEPTH) {
                return false;
            }
        } else if (byte == '}' || byte == ']') {
            if (depth == 0) {
                return false;
            }
            --depth;
        } else if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') {
            return false;
        }
    }
    return !in_string && depth == 0;
}

bool moby_text_valid(const char *text, size_t *code_points)
{
    static const uint32_t minimum[] = { 0, 0x80, 0x800, 0x10000 };
    const uint8_t *p = (const uint8_t *)text;
    size_t count = 0;
    while (*p) {
        uint32_t value;
        unsigned extra;
        if (*p < 0x80) {
            value = *p;
            extra = 0;
        } else if (*p >= 0xc2 && *p <= 0xdf) {
            value = *p & 0x1fU;
            extra = 1;
        } else if (*p >= 0xe0 && *p <= 0xef) {
            value = *p & 0x0fU;
            extra = 2;
        } else if (*p >= 0xf0 && *p <= 0xf4) {
            value = *p & 0x07U;
            extra = 3;
        } else {
            return false;
        }
        for (unsigned i = 1; i <= extra; ++i) {
            if ((p[i] & 0xc0U) != 0x80U) {
                return false;
            }
            value = (value << 6) | (p[i] & 0x3fU);
        }
        if (value < minimum[extra] || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU) ||
            value < 0x20U || (value >= 0x7fU && value <= 0x9fU)) {
            return false;
        }
        p += extra + 1;
        ++count;
    }
    if (code_points) {
        *code_points = count;
    }
    return true;
}

// Every key of the object must be listed in names, and none may repeat.
static bool keys_known(const cJSON *object, const char *const *names, size_t count)
{
    for (const cJSON *item = object->child; item; item = item->next) {
        if (!item->string) {
            return false;
        }
        bool known = false;
        for (size_t i = 0; i < count; ++i) {
            known = known || strcmp(item->string, names[i]) == 0;
        }
        if (!known) {
            return false;
        }
        for (const cJSON *other = item->next; other; other = other->next) {
            if (other->string && strcmp(other->string, item->string) == 0) {
                return false;
            }
        }
    }
    return true;
}

// An optional field counts as absent when missing or null.
static const cJSON *field(const cJSON *object, const char *name, bool optional)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (optional && cJSON_IsNull(item)) {
        return NULL;
    }
    return item;
}

static bool as_uint(const cJSON *item, uint32_t minimum, uint32_t maximum, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble != floor(item->valuedouble) ||
        item->valuedouble < (double)minimum || item->valuedouble > (double)maximum) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool get_uint(const cJSON *object, const char *name, uint32_t minimum, uint32_t maximum, uint32_t *out)
{
    return as_uint(field(object, name, false), minimum, maximum, out);
}

static bool get_bool(const cJSON *object, const char *name, bool *out)
{
    const cJSON *item = field(object, name, false);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

static bool get_id(const cJSON *object, const char *name, char out[MOBY_ID_MAX + 1])
{
    const cJSON *item = field(object, name, false);
    if (!cJSON_IsString(item) || !moby_stable_id_valid(item->valuestring)) {
        return false;
    }
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static bool codec_is_pcm(const cJSON *object)
{
    const cJSON *item = field(object, "codec", false);
    return cJSON_IsString(item) && strcmp(item->valuestring, "pcm_s16le") == 0;
}

// Reads one optional bounded number of a config patch. Returns false when it is present and invalid.
static bool optional_uint(const cJSON *object, const char *name, uint32_t minimum, uint32_t maximum,
                          bool *present, uint32_t *value)
{
    const cJSON *item = field(object, name, true);
    *present = false;
    *value = 0;
    if (!item) {
        return true;
    }
    *present = as_uint(item, minimum, maximum, value);
    return *present;
}

static bool parse_patch(const cJSON *object, moby_config_patch_t *patch)
{
    static const char *const keys[] = { "wake_sensitivity", "speaker_volume", "vad_silence_ms", "max_utterance_ms" };
    if (!cJSON_IsObject(object) || !keys_known(object, keys, 4)) {
        return false;
    }
    uint32_t value[4];
    *patch = (moby_config_patch_t){ 0 };
    if (!optional_uint(object, keys[0], MOBY_USB_PERCENT_MIN, MOBY_USB_PERCENT_MAX, &patch->has_wake_sensitivity, &value[0]) ||
        !optional_uint(object, keys[1], MOBY_USB_PERCENT_MIN, MOBY_USB_PERCENT_MAX, &patch->has_speaker_volume, &value[1]) ||
        !optional_uint(object, keys[2], MOBY_USB_VAD_SILENCE_MS_MIN, MOBY_USB_VAD_SILENCE_MS_MAX,
                       &patch->has_vad_silence_ms, &value[2]) ||
        !optional_uint(object, keys[3], MOBY_USB_MAX_UTTERANCE_MS_MIN, MOBY_USB_MAX_UTTERANCE_MS_MAX,
                       &patch->has_max_utterance_ms, &value[3])) {
        return false;
    }
    patch->wake_sensitivity = (uint8_t)value[0];
    patch->speaker_volume = (uint8_t)value[1];
    patch->vad_silence_ms = (uint16_t)value[2];
    patch->max_utterance_ms = (uint16_t)value[3];
    return true;
}

static bool parse_hello_ack(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "accepted", "session_id", "proof", "desktop_nonce", "config" };
    if (!keys_known(data, keys, 5) || !get_bool(data, "accepted", &msg->u.hello_ack.accepted)) {
        return false;
    }
    const cJSON *session = field(data, "session_id", true);
    if (session) {
        if (!cJSON_IsString(session) || !moby_stable_id_valid(session->valuestring)) {
            return false;
        }
        strcpy(msg->u.hello_ack.session_id, session->valuestring);
        msg->u.hello_ack.has_session_id = true;
    }
    // proof and desktop_nonce come together or not at all, and neither may be null.
    const cJSON *proof = field(data, "proof", false);
    if (proof) {
        if (!cJSON_IsString(proof) ||
            !moby_hex_decode(proof->valuestring, strlen(proof->valuestring), msg->u.hello_ack.proof, MOBY_USB_PROOF_BYTES)) {
            return false;
        }
        msg->u.hello_ack.has_proof = true;
    }
    const cJSON *desktop_nonce = field(data, "desktop_nonce", false);
    if (desktop_nonce) {
        if (!cJSON_IsString(desktop_nonce) ||
            !moby_hex_decode(desktop_nonce->valuestring, strlen(desktop_nonce->valuestring), msg->u.hello_ack.desktop_nonce,
                             MOBY_USB_NONCE_BYTES)) {
            return false;
        }
        msg->u.hello_ack.has_desktop_nonce = true;
    }
    if (msg->u.hello_ack.has_proof != msg->u.hello_ack.has_desktop_nonce) {
        return false;
    }
    return parse_patch(field(data, "config", false), &msg->u.hello_ack.config);
}

static bool parse_expression(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "state", "animation", "duration_ms" };
    const cJSON *state = field(data, "state", false);
    if (!keys_known(data, keys, 3) || !cJSON_IsString(state) || !get_id(data, "animation", msg->u.expression.animation) ||
        !get_uint(data, "duration_ms", MOBY_USB_ACTION_DURATION_MS_MIN, MOBY_USB_ACTION_DURATION_MS_MAX,
                  &msg->u.expression.duration_ms)) {
        return false;
    }
    for (int index = 0; index < MOBY_EXPRESSION_COUNT; ++index) {
        if (strcmp(state->valuestring, moby_expression_name((moby_expression)index)) == 0) {
            msg->u.expression.state = (moby_expression)index;
            return true;
        }
    }
    return false;
}

static bool parse_display_text(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "text", "ttl_ms" };
    const cJSON *text = field(data, "text", false);
    size_t points = 0;
    if (!keys_known(data, keys, 2) || !cJSON_IsString(text) || strlen(text->valuestring) > MOBY_SCREEN_TEXT_MAX_BYTES ||
        !moby_text_valid(text->valuestring, &points) || points < MOBY_USB_SCREEN_TEXT_MIN_CHARS ||
        points > MOBY_USB_SCREEN_TEXT_MAX_CHARS ||
        !get_uint(data, "ttl_ms", MOBY_USB_TEXT_TTL_MS_MIN, MOBY_USB_TEXT_TTL_MS_MAX, &msg->u.display_text.ttl_ms)) {
        return false;
    }
    strcpy(msg->u.display_text.text, text->valuestring);
    return true;
}

static bool parse_audio_play(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "stream_id", "codec", "sample_rate" };
    uint32_t rate;
    return keys_known(data, keys, 3) &&
           get_uint(data, "stream_id", MOBY_USB_AUDIO_STREAM_ID_MIN, MOBY_USB_AUDIO_STREAM_ID_MAX,
                    &msg->u.cancel.stream_id) &&
           codec_is_pcm(data) && get_uint(data, "sample_rate", MOBY_USB_SAMPLE_RATE_MIN, MOBY_USB_SAMPLE_RATE_MAX, &rate);
}

static bool parse_cancel(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "stream_id" };
    return keys_known(data, keys, 1) &&
           get_uint(data, "stream_id", MOBY_USB_AUDIO_STREAM_ID_MIN, MOBY_USB_AUDIO_STREAM_ID_MAX,
                    &msg->u.cancel.stream_id);
}

static bool parse_servo_pose(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "pose", "duration_ms" };
    return keys_known(data, keys, 2) && get_id(data, "pose", msg->u.servo_pose.pose) &&
           get_uint(data, "duration_ms", MOBY_USB_ACTION_DURATION_MS_MIN, MOBY_USB_ACTION_DURATION_MS_MAX,
                    &msg->u.servo_pose.duration_ms);
}

static bool parse_pair_begin(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "timeout_ms" };
    return keys_known(data, keys, 1) &&
           get_uint(data, "timeout_ms", MOBY_USB_PAIR_TIMEOUT_MS_MIN, MOBY_USB_PAIR_TIMEOUT_MS_MAX,
                    &msg->u.pair_begin.timeout_ms);
}

// The hex text is wiped before cJSON frees it, so the key does not linger in the heap.
static bool parse_pair_set_key(const cJSON *data, moby_msg_t *msg)
{
    static const char *const keys[] = { "pair_key" };
    const cJSON *key = field(data, "pair_key", false);
    const bool valid = keys_known(data, keys, 1) && cJSON_IsString(key) &&
                       moby_hex_decode(key->valuestring, strlen(key->valuestring), msg->u.pair_set_key.key,
                                       MOBY_USB_PAIR_KEY_BYTES);
    if (cJSON_IsString(key) && key->valuestring) {
        memset(key->valuestring, 0, strlen(key->valuestring));
    }
    return valid;
}

static bool parse_data(const cJSON *data, moby_msg_t *msg)
{
    switch (msg->name) {
    case MOBY_MSG_HELLO_ACK:
        return parse_hello_ack(data, msg);
    case MOBY_MSG_EXPRESSION:
        return parse_expression(data, msg);
    case MOBY_MSG_DISPLAY_TEXT:
        return parse_display_text(data, msg);
    case MOBY_MSG_AUDIO_PLAY_START:
    case MOBY_MSG_AUDIO_PLAY_END:
        return parse_audio_play(data, msg);
    case MOBY_MSG_CANCEL:
        return parse_cancel(data, msg);
    case MOBY_MSG_SET_CONFIG:
        return parse_patch(data, &msg->u.set_config);
    case MOBY_MSG_SERVO_POSE:
        return parse_servo_pose(data, msg);
    case MOBY_MSG_PAIR_BEGIN:
        return parse_pair_begin(data, msg);
    case MOBY_MSG_PAIR_SET_KEY:
        return parse_pair_set_key(data, msg);
    }
    return false;
}

static moby_parse_status_t parse_envelope(const cJSON *root, moby_parsed_t *parsed)
{
    static const char *const keys[] = { "v", "kind", "name", "id", "ts_ms", "data" };
    if (!cJSON_IsObject(root) || !get_id(root, "id", parsed->msg.id)) {
        return MOBY_PARSE_MALFORMED;
    }
    parsed->has_id = true;
    const cJSON *version = field(root, "v", false);
    const cJSON *kind = field(root, "kind", false);
    const cJSON *name = field(root, "name", false);
    const cJSON *data = field(root, "data", false);
    parsed->command = cJSON_IsString(kind) && strcmp(kind->valuestring, "command") == 0;
    if (!cJSON_IsNumber(version) || !cJSON_IsString(kind) || !cJSON_IsString(name)) {
        return MOBY_PARSE_BAD_DATA;
    }
    if (version->valuedouble != (double)MOBY_USB_PROTOCOL_VERSION) {
        return MOBY_PARSE_VERSION;
    }
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; ++i) {
        if (strcmp(NAMES[i].name, name->valuestring) != 0) {
            continue;
        }
        if (!NAMES[i].desktop || strcmp(kind->valuestring, NAMES[i].kind) != 0) {
            return MOBY_PARSE_WRONG_KIND;
        }
        parsed->msg.name = NAMES[i].message;
        if (!keys_known(root, keys, 6) || !get_uint(root, "ts_ms", 0, UINT32_MAX, &parsed->msg.ts_ms) ||
            !cJSON_IsObject(data)) {
            return MOBY_PARSE_BAD_DATA;
        }
        return parse_data(data, &parsed->msg) ? MOBY_PARSE_OK : MOBY_PARSE_BAD_DATA;
    }
    return MOBY_PARSE_UNKNOWN_NAME;
}

moby_parse_status_t moby_json_parse(const uint8_t *payload, size_t length, moby_parsed_t *parsed)
{
    memset(parsed, 0, sizeof *parsed);
    if (length == 0 || length > MOBY_USB_MAX_PAYLOAD_LEN || !prescan(payload, length)) {
        return MOBY_PARSE_MALFORMED;
    }
    // cJSON reads one byte past the length to check for the terminator, so parse a terminated copy.
    char text[MOBY_USB_MAX_PAYLOAD_LEN + 1];
    memcpy(text, payload, length);
    text[length] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(text, length + 1, NULL, true);
    if (!root) {
        return MOBY_PARSE_MALFORMED;
    }
    const moby_parse_status_t status = parse_envelope(root, parsed);
    cJSON_Delete(root);
    return status;
}

// Appends formatted text and remembers when the buffer ran out.
typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool failed;
} writer_t;

static void put(writer_t *writer, const char *format, ...)
{
    if (writer->failed) {
        return;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(writer->buffer + writer->length, writer->capacity - writer->length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= writer->capacity - writer->length) {
        writer->failed = true;
        return;
    }
    writer->length += (size_t)written;
}

// A text goes into a JSON string unescaped, so it must hold no quote, backslash or control.
static bool json_safe(const char *text)
{
    for (; *text; ++text) {
        if (*text == '"' || *text == '\\' || (unsigned char)*text < 0x20 || (unsigned char)*text > 0x7e) {
            return false;
        }
    }
    return true;
}

static void put_hello(writer_t *writer, const moby_identity_t *identity, const moby_out_t *out)
{
    char nonce[2 * MOBY_USB_NONCE_BYTES + 1] = { 0 };
    moby_hex_encode(out->u.hello.nonce, MOBY_USB_NONCE_BYTES, nonce);
    put(writer, "{\"device_id\":\"%s\",\"hw_rev\":\"%s\",\"fw_version\":\"%s\",\"protocol_version\":%u,\"capabilities\":[",
        identity->device_id, identity->hw_rev, identity->fw_version, (unsigned)MOBY_USB_PROTOCOL_VERSION);
    const char *separator = "";
    if (identity->capabilities & MOBY_CAP_USB_PCM) {
        put(writer, "%s\"usb_pcm\"", separator);
        separator = ",";
    }
    if (identity->capabilities & MOBY_CAP_SERVO_POSE) {
        put(writer, "%s\"servo_pose\"", separator);
    }
    put(writer, "],\"mute\":%s,\"paired\":%s,\"nonce\":\"%s\"}", out->u.hello.mute ? "true" : "false",
        out->u.hello.paired ? "true" : "false", nonce);
}

size_t moby_json_build(const moby_identity_t *identity, const moby_out_t *out, char *buffer, size_t capacity)
{
    static const char *const actions[] = { "press", "release", "long_press" };
    static const char *const start_reasons[] = { "wake", "button" };
    static const char *const end_reasons[] = { "vad_silence", "max_duration", "button_release", "mute", "cancel", "error" };
    writer_t writer = { buffer, capacity, 0, capacity == 0 };
    const bool response = out->kind == MOBY_OUT_COMMAND_OK || out->kind == MOBY_OUT_COMMAND_ERROR;
    const char *name = "";
    switch (out->kind) {
    case MOBY_OUT_HELLO:
        name = MOBY_USB_MSG_HELLO;
        break;
    case MOBY_OUT_HEARTBEAT:
        name = MOBY_USB_MSG_HEARTBEAT;
        break;
    case MOBY_OUT_BUTTON_EVENT:
        name = MOBY_USB_MSG_BUTTON_EVENT;
        break;
    case MOBY_OUT_MUTE_CHANGED:
        name = MOBY_USB_MSG_MUTE_CHANGED;
        break;
    case MOBY_OUT_ERROR:
        name = MOBY_USB_MSG_ERROR;
        break;
    case MOBY_OUT_PAIR_BUTTON_CONFIRMED:
        name = MOBY_USB_MSG_PAIR_BUTTON_CONFIRMED;
        break;
    case MOBY_OUT_PAIR_RESET:
        name = MOBY_USB_MSG_PAIR_RESET;
        break;
    case MOBY_OUT_SESSION_PROOF:
        name = MOBY_USB_MSG_SESSION_PROOF;
        break;
    case MOBY_OUT_COMMAND_OK:
        name = MOBY_USB_MSG_COMMAND_OK;
        break;
    case MOBY_OUT_COMMAND_ERROR:
        name = MOBY_USB_MSG_COMMAND_ERROR;
        break;
    case MOBY_OUT_AUDIO_START:
        name = MOBY_USB_MSG_AUDIO_START;
        break;
    case MOBY_OUT_AUDIO_END:
        name = MOBY_USB_MSG_AUDIO_END;
        break;
    }
    if (!moby_stable_id_valid(out->id) || ((out->kind == MOBY_OUT_ERROR || out->kind == MOBY_OUT_COMMAND_ERROR) &&
                                           (!json_safe(out->u.error.code) || !json_safe(out->u.error.detail)))) {
        return 0;
    }
    put(&writer, "{\"v\":1,\"kind\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"ts_ms\":%" PRIu32 ",\"data\":",
        response ? "response" : "event", name, out->id, out->ts_ms);
    switch (out->kind) {
    case MOBY_OUT_HELLO:
        put_hello(&writer, identity, out);
        break;
    case MOBY_OUT_HEARTBEAT:
        put(&writer, "{\"state\":\"%s\",\"mute\":%s,\"uptime_ms\":%" PRIu32 "}",
            moby_expression_name(out->u.heartbeat.state), out->u.heartbeat.mute ? "true" : "false",
            out->u.heartbeat.uptime_ms);
        break;
    case MOBY_OUT_BUTTON_EVENT:
        put(&writer, "{\"button\":\"talk\",\"action\":\"%s\"}", actions[out->u.button.action]);
        break;
    case MOBY_OUT_MUTE_CHANGED:
        put(&writer, "{\"muted\":%s,\"source\":\"%s\"}", out->u.mute.muted ? "true" : "false",
            out->u.mute.boot ? "boot" : "switch");
        break;
    case MOBY_OUT_ERROR:
    case MOBY_OUT_COMMAND_ERROR:
        put(&writer, "{\"code\":\"%s\",\"detail\":\"%s\"}", out->u.error.code, out->u.error.detail);
        break;
    case MOBY_OUT_AUDIO_START:
        put(&writer,
            "{\"stream_id\":%u,\"reason\":\"%s\",\"codec\":\"pcm_s16le\",\"sample_rate\":%u,\"channels\":1,"
            "\"frame_ms\":%u,\"preroll_ms\":%" PRIu32 "}",
            (unsigned)out->u.audio_start.stream_id, start_reasons[out->u.audio_start.reason & 1U],
            (unsigned)MOBY_PCM_SAMPLE_RATE, (unsigned)MOBY_PCM_FRAME_MS, out->u.audio_start.preroll_ms);
        break;
    case MOBY_OUT_AUDIO_END:
        put(&writer, "{\"stream_id\":%u,\"reason\":\"%s\",\"frames\":%" PRIu32 ",\"duration_ms\":%" PRIu32 "}",
            (unsigned)out->u.audio_end.stream_id, end_reasons[(unsigned)out->u.audio_end.reason % 6U],
            out->u.audio_end.frames, out->u.audio_end.duration_ms);
        break;
    case MOBY_OUT_SESSION_PROOF: {
        char proof[2 * MOBY_USB_PROOF_BYTES + 1] = { 0 };
        moby_hex_encode(out->u.session_proof.proof, MOBY_USB_PROOF_BYTES, proof);
        put(&writer, "{\"proof\":\"%s\"}", proof);
        break;
    }
    case MOBY_OUT_PAIR_BUTTON_CONFIRMED:
    case MOBY_OUT_PAIR_RESET:
    case MOBY_OUT_COMMAND_OK:
        put(&writer, "{}");
        break;
    }
    put(&writer, "}");
    return writer.failed ? 0 : writer.length;
}
