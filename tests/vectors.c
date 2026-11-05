// SPDX-License-Identifier: GPL-3.0-only

// Runs the vendored moby-protocol vectors through the firmware's own code: the
// codec, the control JSON parser and builder, the transition table, and the
// mbedTLS HMAC used for the session proof. Run: vectors <fixtures directory>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moby_hmac.h"
#include "moby_json.h"
#include "moby_usb_codec.h"

enum { MAX_FIELDS = 8, MAX_LINE = 8192 };

static size_t from_hex(const char *text, uint8_t *bytes, size_t capacity)
{
    const size_t length = strlen(text);
    assert(length % 2 == 0 && length / 2 <= capacity);
    assert(moby_hex_decode(text, length, bytes, length / 2));
    return length / 2;
}

static size_t split_tabs(char *line, char **fields)
{
    size_t count = 0;
    line[strcspn(line, "\r\n")] = '\0';
    fields[count++] = line;
    for (char *tab = strchr(line, '\t'); tab != NULL; tab = strchr(tab, '\t')) {
        assert(count < MAX_FIELDS);
        *tab++ = '\0';
        fields[count++] = tab;
    }
    return count;
}

typedef void (*row_handler)(char **fields, size_t count);

static size_t for_each_row(const char *directory, const char *name, row_handler handler)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    static char line[MAX_LINE];
    size_t rows = 0;
    while (fgets(line, sizeof line, file)) {
        if (line[0] == '#') {
            continue;
        }
        char *fields[MAX_FIELDS];
        const size_t count = split_tabs(line, fields);
        handler(fields, count);
        ++rows;
    }
    fclose(file);
    assert(rows > 0);
    return rows;
}

static moby_status decode_wire(moby_decoder *decoder, const uint8_t *wire, size_t length, moby_frame *frame)
{
    assert(length > 0 && wire[length - 1] == MOBY_USB_DELIMITER);
    for (size_t i = 0; i + 1 < length; ++i) {
        assert(moby_decoder_push(decoder, wire[i], frame) == MOBY_PENDING);
    }
    return moby_decoder_push(decoder, wire[length - 1], frame);
}

static moby_status expected_status(const char *name)
{
    if (strcmp(name, "ok") == 0) {
        return MOBY_OK;
    }
    if (strcmp(name, "bad_frame") == 0) {
        return MOBY_ERR_BAD_FRAME;
    }
    if (strcmp(name, "bad_crc") == 0) {
        return MOBY_ERR_BAD_CRC;
    }
    assert(strcmp(name, "unsupported_version") == 0);
    return MOBY_ERR_UNSUPPORTED_VERSION;
}

static moby_decoder decoder;

static void frame_row(char **fields, size_t count)
{
    assert(count == 7);
    uint8_t wire[MOBY_USB_MAX_WIRE_LEN];
    uint8_t payload[MOBY_USB_MAX_PAYLOAD_LEN];
    const size_t wire_length = from_hex(fields[6], wire, sizeof wire);
    moby_frame frame;
    const moby_status status = decode_wire(&decoder, wire, wire_length, &frame);
    assert(status == expected_status(fields[1]));
    if (status == MOBY_OK) {
        const size_t payload_length = from_hex(fields[5], payload, sizeof payload);
        assert(frame.kind == strtoul(fields[2], NULL, 10) && frame.stream_id == strtoul(fields[3], NULL, 10) &&
               frame.sequence == strtoul(fields[4], NULL, 10));
        assert(frame.payload_length == payload_length && memcmp(frame.payload, payload, payload_length) == 0);
        uint8_t rebuilt[MOBY_USB_MAX_WIRE_LEN];
        size_t rebuilt_length = 0;
        assert(moby_frame_build(frame.kind, frame.stream_id, frame.sequence, frame.payload, frame.payload_length, rebuilt,
                                sizeof rebuilt, &rebuilt_length) == MOBY_OK);
        assert(rebuilt_length == wire_length && memcmp(rebuilt, wire, wire_length) == 0);
    }
}

// Rows the desktop may send, by prefix of the row name. Everything else is a robot message.
static bool desktop_row(const char *name)
{
    static const char *const prefixes[] = { "hello_ack", "expression", "display_text", "audio_play", "cancel",
                                            "set_config", "servo_pose", "pair_begin", "pair_set_key" };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; ++i) {
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

static const moby_identity_t VECTOR_IDENTITY = {
    .device_id = "dev_123e4567-e89b-42d3-a456-426614174000",
    .hw_rev = "esp32-s3-r1",
    .fw_version = "0.1.0",
    .capabilities = MOBY_CAP_USB_PCM,
};

// The robot's own messages must come out byte for byte as the vectors have them.
static void check_built(const char *name, const uint8_t *payload, size_t payload_length)
{
    moby_out_t out;
    memset(&out, 0, sizeof out);
    if (strcmp(name, "hello") == 0 || strcmp(name, "hello_paired") == 0) {
        out.kind = MOBY_OUT_HELLO;
        const bool paired = strcmp(name, "hello_paired") == 0;
        strcpy(out.id, paired ? "e-0002" : "e-0001");
        out.ts_ms = paired ? 20 : 10;
        out.u.hello.mute = true;
        out.u.hello.paired = paired;
        assert(moby_hex_decode(paired ? "00112233445566778899aabbccddeeff" : "a1b2c3d4e5f60718293a4b5c6d7e8f90", 32,
                               out.u.hello.nonce, 16));
    } else if (strcmp(name, "heartbeat") == 0) {
        out.kind = MOBY_OUT_HEARTBEAT;
        strcpy(out.id, "e-0006");
        out.ts_ms = 60;
        out.u.heartbeat.state = MOBY_EXPRESSION_MUTED;
        out.u.heartbeat.mute = true;
        out.u.heartbeat.uptime_ms = 2010;
    } else if (strcmp(name, "button_event_press") == 0 || strcmp(name, "button_event_long_press") == 0) {
        const bool press = strcmp(name, "button_event_press") == 0;
        out.kind = MOBY_OUT_BUTTON_EVENT;
        strcpy(out.id, press ? "e-0008" : "e-0009");
        out.ts_ms = press ? 80 : 90;
        out.u.button.action = press ? MOBY_BUTTON_PRESS : MOBY_BUTTON_LONG_PRESS;
    } else if (strcmp(name, "mute_changed") == 0) {
        out.kind = MOBY_OUT_MUTE_CHANGED;
        strcpy(out.id, "e-0010");
        out.ts_ms = 100;
        out.u.mute.muted = true;
    } else if (strcmp(name, "error") == 0) {
        out.kind = MOBY_OUT_ERROR;
        strcpy(out.id, "e-0013");
        out.ts_ms = 130;
        out.u.error.code = MOBY_USB_E_AUDIO_OVERRUN;
        out.u.error.detail = "ring buffer overrun";
    } else if (strcmp(name, "audio_start") == 0) {
        out.kind = MOBY_OUT_AUDIO_START;
        strcpy(out.id, "e-0011");
        out.ts_ms = 110;
        out.u.audio_start.stream_id = 1;
        out.u.audio_start.reason = MOBY_AUDIO_START_WAKE;
        out.u.audio_start.preroll_ms = 300;
    } else if (strcmp(name, "audio_end") == 0) {
        out.kind = MOBY_OUT_AUDIO_END;
        strcpy(out.id, "e-0012");
        out.ts_ms = 120;
        out.u.audio_end.stream_id = 1;
        out.u.audio_end.reason = MOBY_AUDIO_END_VAD_SILENCE;
        out.u.audio_end.frames = 150;
        out.u.audio_end.duration_ms = 3000;
    } else if (strcmp(name, "pair_button_confirmed") == 0 || strcmp(name, "pair_reset") == 0) {
        const bool confirmed = strcmp(name, "pair_button_confirmed") == 0;
        out.kind = confirmed ? MOBY_OUT_PAIR_BUTTON_CONFIRMED : MOBY_OUT_PAIR_RESET;
        strcpy(out.id, confirmed ? "e-0014" : "e-0015");
        out.ts_ms = confirmed ? 140 : 150;
    } else if (strcmp(name, "session_proof") == 0) {
        out.kind = MOBY_OUT_SESSION_PROOF;
        strcpy(out.id, "e-0031");
        out.ts_ms = 310;
        assert(moby_hex_decode("b3bae9e90d0e2cee7808185e64fd557087fc93f6e5e0e0c866467c2d28b921f4", 64,
                               out.u.session_proof.proof, MOBY_USB_PROOF_BYTES));
    } else if (strcmp(name, "command_ok") == 0) {
        out.kind = MOBY_OUT_COMMAND_OK;
        strcpy(out.id, "r-0029");
        out.ts_ms = 290;
    } else if (strcmp(name, "command_error") == 0) {
        out.kind = MOBY_OUT_COMMAND_ERROR;
        strcpy(out.id, "r-0030");
        out.ts_ms = 300;
        out.u.error.code = MOBY_USB_E_STATE;
        out.u.error.detail = "pairing was not confirmed";
    } else {
        return;
    }
    char built[MOBY_USB_MAX_PAYLOAD_LEN];
    const size_t length = moby_json_build(&VECTOR_IDENTITY, &out, built, sizeof built);
    if (length != payload_length || memcmp(built, payload, payload_length) != 0) {
        fprintf(stderr, "%s: built message differs from the vector\n%.*s\n", name, (int)length, built);
        exit(1);
    }
}

static size_t built_rows;

static void control_row(char **fields, size_t count)
{
    assert(count == 5);
    const bool valid = strcmp(fields[1], "valid") == 0;
    assert(valid || strcmp(fields[1], "invalid") == 0);
    uint8_t wire[MOBY_USB_MAX_WIRE_LEN];
    uint8_t payload[MOBY_USB_MAX_PAYLOAD_LEN];
    const size_t wire_length = from_hex(fields[4], wire, sizeof wire);
    const size_t payload_length = from_hex(fields[3], payload, sizeof payload);
    moby_frame frame;
    assert(decode_wire(&decoder, wire, wire_length, &frame) == MOBY_OK);
    assert(frame.kind == MOBY_USB_CONTROL && frame.payload_length == payload_length &&
           memcmp(frame.payload, payload, payload_length) == 0);

    moby_parsed_t parsed;
    const moby_parse_status_t status = moby_json_parse(payload, payload_length, &parsed);
    const bool expect_ok = valid && desktop_row(fields[0]);
    if ((status == MOBY_PARSE_OK) != expect_ok) {
        fprintf(stderr, "%s: parser says %d, expected %s\n", fields[0], (int)status, expect_ok ? "ok" : "a rejection");
        exit(1);
    }
    if (valid && !desktop_row(fields[0])) {
        check_built(fields[0], payload, payload_length);
        ++built_rows;
    }
}

static void cobs_row(char **fields, size_t count)
{
    assert(count == 3);
    uint8_t raw[MOBY_USB_MAX_DECODED_LEN];
    uint8_t encoded[MOBY_USB_MAX_ENCODED_LEN];
    uint8_t output[MOBY_USB_MAX_ENCODED_LEN];
    size_t output_length = 0;
    const size_t raw_length = from_hex(fields[1], raw, sizeof raw);
    const size_t encoded_length = from_hex(fields[2], encoded, sizeof encoded);
    assert(moby_cobs_encode(raw, raw_length, output, sizeof output, &output_length) == MOBY_OK);
    assert(output_length == encoded_length && memcmp(output, encoded, encoded_length) == 0);
    uint8_t decoded[MOBY_USB_MAX_DECODED_LEN];
    size_t decoded_length = 0;
    assert(moby_cobs_decode(encoded, encoded_length, decoded, sizeof decoded, &decoded_length) == MOBY_OK);
    assert(decoded_length == raw_length && memcmp(decoded, raw, raw_length) == 0);
}

// RFC 4231, computed by the mbedTLS code the firmware links.
static void hmac_row(char **fields, size_t count)
{
    assert(count == 4);
    uint8_t key[256];
    uint8_t message[256];
    uint8_t expected[32];
    uint8_t actual[32];
    const size_t key_length = from_hex(fields[1], key, sizeof key);
    const size_t message_length = from_hex(fields[2], message, sizeof message);
    assert(from_hex(fields[3], expected, sizeof expected) == 32);
    assert(moby_hmac_sha256(key, key_length, message, message_length, actual));
    if (memcmp(actual, expected, 32) != 0) {
        fprintf(stderr, "%s: HMAC differs from the vector\n", fields[0]);
        exit(1);
    }
}

static void proof_row(char **fields, size_t count)
{
    assert(count == 7);
    uint8_t key[MOBY_USB_PAIR_KEY_BYTES];
    uint8_t nonce[MOBY_USB_NONCE_BYTES];
    uint8_t expected_input[256];
    uint8_t proof[MOBY_USB_PROOF_BYTES];
    assert(from_hex(fields[1], key, sizeof key) == sizeof key);
    assert(from_hex(fields[2], nonce, sizeof nonce) == sizeof nonce);
    const size_t expected_length = from_hex(fields[5], expected_input, sizeof expected_input);
    assert(from_hex(fields[6], proof, sizeof proof) == sizeof proof);
    uint8_t input[256];
    size_t input_length = 0;
    assert(moby_session_proof_input(nonce, fields[3], fields[4], input, sizeof input, &input_length) == MOBY_OK);
    assert(input_length == expected_length && memcmp(input, expected_input, expected_length) == 0);
    uint8_t actual[32];
    assert(moby_hmac_sha256(key, sizeof key, input, input_length, actual));
    if (!moby_ct_equal(actual, proof, sizeof proof)) {
        fprintf(stderr, "%s: proof differs from the vector\n", fields[0]);
        exit(1);
    }
    // A flipped bit must not verify.
    actual[0] ^= 1;
    assert(!moby_ct_equal(actual, proof, sizeof proof));
}

// The robot's proof (session_proof): the vendored input builder and the mbedTLS HMAC reproduce every row.
static void robot_proof_row(char **fields, size_t count)
{
    assert(count == 8);
    uint8_t key[MOBY_USB_PAIR_KEY_BYTES];
    uint8_t desktop_nonce[MOBY_USB_NONCE_BYTES];
    uint8_t robot_nonce[MOBY_USB_NONCE_BYTES];
    uint8_t expected_input[256];
    uint8_t proof[MOBY_USB_PROOF_BYTES];
    assert(from_hex(fields[1], key, sizeof key) == sizeof key);
    assert(from_hex(fields[2], desktop_nonce, sizeof desktop_nonce) == sizeof desktop_nonce);
    assert(from_hex(fields[3], robot_nonce, sizeof robot_nonce) == sizeof robot_nonce);
    const size_t expected_length = from_hex(fields[6], expected_input, sizeof expected_input);
    assert(from_hex(fields[7], proof, sizeof proof) == sizeof proof);
    uint8_t input[256];
    size_t input_length = 0;
    assert(moby_robot_proof_input(desktop_nonce, robot_nonce, fields[4], fields[5], input, sizeof input, &input_length) ==
           MOBY_OK);
    assert(input_length == expected_length && memcmp(input, expected_input, expected_length) == 0);
    uint8_t actual[32];
    assert(moby_hmac_sha256(key, sizeof key, input, input_length, actual));
    if (!moby_ct_equal(actual, proof, sizeof proof)) {
        fprintf(stderr, "%s: robot proof differs from the vector\n", fields[0]);
        exit(1);
    }
    actual[0] ^= 1;
    assert(!moby_ct_equal(actual, proof, sizeof proof));
}

// Desktop and robot inputs differ, so their proofs do; both are recomputed here.
static void reflection_row(char **fields, size_t count)
{
    assert(count == 7);
    uint8_t key[MOBY_USB_PAIR_KEY_BYTES];
    uint8_t desktop_input[256];
    uint8_t robot_input[256];
    uint8_t desktop_proof[MOBY_USB_PROOF_BYTES];
    uint8_t robot_proof[MOBY_USB_PROOF_BYTES];
    assert(from_hex(fields[1], key, sizeof key) == sizeof key);
    const size_t desktop_length = from_hex(fields[2], desktop_input, sizeof desktop_input);
    assert(from_hex(fields[3], desktop_proof, sizeof desktop_proof) == sizeof desktop_proof);
    const size_t robot_length = from_hex(fields[4], robot_input, sizeof robot_input);
    assert(from_hex(fields[5], robot_proof, sizeof robot_proof) == sizeof robot_proof);
    uint8_t actual[32];
    assert(moby_hmac_sha256(key, sizeof key, desktop_input, desktop_length, actual));
    assert(moby_ct_equal(actual, desktop_proof, sizeof actual));
    assert(moby_hmac_sha256(key, sizeof key, robot_input, robot_length, actual));
    assert(moby_ct_equal(actual, robot_proof, sizeof actual));
    assert(!moby_ct_equal(desktop_proof, robot_proof, sizeof actual));
    size_t first = 0;
    while (first < desktop_length && first < robot_length && desktop_input[first] == robot_input[first]) {
        ++first;
    }
    assert(first == (size_t)strtoul(fields[6], NULL, 10) && first <= 16);
}

static size_t transition_index;

static void transition_row(char **fields, size_t count)
{
    assert(count == 2 && transition_index < MOBY_EXPRESSION_COUNT && strlen(fields[1]) == MOBY_EXPRESSION_COUNT);
    assert(strcmp(moby_expression_name((moby_expression)transition_index), fields[0]) == 0);
    for (size_t to = 0; to < MOBY_EXPRESSION_COUNT; ++to) {
        assert(moby_can_transition((moby_expression)transition_index, (moby_expression)to) == (fields[1][to] == '1'));
    }
    ++transition_index;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixtures directory>\n", argv[0]);
        return 2;
    }
    moby_decoder_reset(&decoder);
    const size_t frames = for_each_row(argv[1], "usb-vectors.tsv", frame_row);
    const size_t controls = for_each_row(argv[1], "control-vectors.tsv", control_row);
    const size_t cobs = for_each_row(argv[1], "cobs-vectors.tsv", cobs_row);
    const size_t hmacs = for_each_row(argv[1], "hmac-vectors.tsv", hmac_row);
    const size_t proofs = for_each_row(argv[1], "proof-vectors.tsv", proof_row);
    const size_t robot_proofs = for_each_row(argv[1], "robot-proof-vectors.tsv", robot_proof_row);
    const size_t reflections = for_each_row(argv[1], "reflection-vectors.tsv", reflection_row);
    const size_t transitions = for_each_row(argv[1], "transition-vectors.tsv", transition_row);
    assert(transition_index == MOBY_EXPRESSION_COUNT);
    printf("vectors passed: %zu frame, %zu control (%zu built byte for byte), %zu COBS, %zu HMAC, %zu proof, "
           "%zu robot proof, %zu reflection, %zu transition rows\n",
           frames, controls, built_rows, cobs, hmacs, proofs, robot_proofs, reflections, transitions);
    return 0;
}
