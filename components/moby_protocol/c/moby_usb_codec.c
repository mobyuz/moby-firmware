#include "moby_usb_codec.h"

#include <string.h>

/* CRC-32/ISO-HDLC, reflected polynomial 0xEDB88320, one nibble at a time. */
static const uint32_t crc_nibbles[16] = {
    UINT32_C(0x00000000), UINT32_C(0x1DB71064), UINT32_C(0x3B6E20C8), UINT32_C(0x26D930AC),
    UINT32_C(0x76DC4190), UINT32_C(0x6B6B51F4), UINT32_C(0x4DB26158), UINT32_C(0x5005713C),
    UINT32_C(0xEDB88320), UINT32_C(0xF00F9344), UINT32_C(0xD6D6A3E8), UINT32_C(0xCB61B38C),
    UINT32_C(0x9B64C2B0), UINT32_C(0x86D3D2D4), UINT32_C(0xA00AE278), UINT32_C(0xBDBDF21C),
};

uint32_t moby_crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
    crc = ~crc;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ crc_nibbles[crc & 0x0fu];
        crc = (crc >> 4) ^ crc_nibbles[crc & 0x0fu];
    }
    return ~crc;
}

uint32_t moby_crc32(const uint8_t *data, size_t length) {
    return moby_crc32_update(0, data, length);
}

/* A COBS encoder that takes one byte at a time, so a frame needs no scratch copy. */
typedef struct cobs_encoder {
    uint8_t *output;
    size_t capacity;
    size_t position;
    size_t code_index;
    uint8_t code;
    bool overflow;
} cobs_encoder;

static void encoder_start(cobs_encoder *encoder, uint8_t *output, size_t capacity) {
    encoder->output = output;
    encoder->capacity = capacity;
    encoder->position = 1;
    encoder->code_index = 0;
    encoder->code = 1;
    encoder->overflow = output == NULL || capacity == 0;
}

/* Closes the current block and opens the next one. */
static void encoder_next_block(cobs_encoder *encoder) {
    encoder->output[encoder->code_index] = encoder->code;
    if (encoder->position >= encoder->capacity) {
        encoder->overflow = true;
        return;
    }
    encoder->code_index = encoder->position++;
    encoder->code = 1;
}

static void encoder_push(cobs_encoder *encoder, uint8_t byte) {
    if (encoder->overflow) {
        return;
    }
    /* A full block ends only when more input follows, so 254 bytes at the end
     * of the input need no trailing 0x01 block. */
    if (encoder->code == 0xff) {
        encoder_next_block(encoder);
        if (encoder->overflow) {
            return;
        }
    }
    if (byte == 0) {
        encoder_next_block(encoder);
    } else if (encoder->position >= encoder->capacity) {
        encoder->overflow = true;
    } else {
        encoder->output[encoder->position++] = byte;
        encoder->code++;
    }
}

static moby_status encoder_finish(cobs_encoder *encoder, size_t *length) {
    if (encoder->overflow) {
        return MOBY_ERR_OUTPUT_TOO_SMALL;
    }
    encoder->output[encoder->code_index] = encoder->code;
    *length = encoder->position;
    return MOBY_OK;
}

size_t moby_cobs_max_encoded_len(size_t length) {
    return length + length / 254 + 1;
}

moby_status moby_cobs_encode(const uint8_t *input, size_t input_length, uint8_t *output,
                             size_t output_capacity, size_t *output_length) {
    if ((input == NULL && input_length != 0) || output == NULL || output_length == NULL) {
        return MOBY_ERR_BAD_ARGUMENT;
    }
    cobs_encoder encoder;
    encoder_start(&encoder, output, output_capacity);
    for (size_t i = 0; i < input_length; i++) {
        encoder_push(&encoder, input[i]);
    }
    return encoder_finish(&encoder, output_length);
}

moby_status moby_cobs_decode(const uint8_t *input, size_t input_length, uint8_t *output,
                             size_t output_capacity, size_t *output_length) {
    if ((input == NULL && input_length != 0) || output == NULL || output_length == NULL) {
        return MOBY_ERR_BAD_ARGUMENT;
    }
    size_t read = 0;
    size_t written = 0;
    while (read < input_length) {
        const uint8_t code = input[read++];
        if (code == 0) {
            return MOBY_ERR_BAD_FRAME;
        }
        const size_t count = (size_t)code - 1;
        if (count > input_length - read) {
            return MOBY_ERR_BAD_FRAME;
        }
        for (size_t i = 0; i < count; i++) {
            if (input[read + i] == 0) {
                return MOBY_ERR_BAD_FRAME;
            }
        }
        const bool add_zero = code != 0xff && read + count < input_length;
        if (count + (add_zero ? 1u : 0u) > output_capacity - written) {
            return MOBY_ERR_OUTPUT_TOO_SMALL;
        }
        /* Output never passes the input it came from, so this is safe in place. */
        memmove(output + written, input + read, count);
        written += count;
        read += count;
        if (add_zero) {
            output[written++] = 0;
        }
    }
    *output_length = written;
    return MOBY_OK;
}

static uint16_t read_u16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32(const uint8_t *bytes) {
    return bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool stream_id_matches_kind(uint8_t kind, uint16_t stream_id) {
    return (kind == MOBY_USB_CONTROL) == (stream_id == 0);
}

static bool kind_known(uint8_t kind) {
    return kind == MOBY_USB_CONTROL || kind == MOBY_USB_ROBOT_AUDIO ||
           kind == MOBY_USB_DESKTOP_AUDIO;
}

moby_status moby_frame_parse(const uint8_t *raw, size_t raw_length, moby_frame *frame) {
    if (raw == NULL || frame == NULL || raw_length < MOBY_USB_HEADER_LEN) {
        return MOBY_ERR_BAD_FRAME;
    }
    const size_t payload_length = read_u16(raw + MOBY_USB_LENGTH_OFFSET);
    const size_t body_length = MOBY_USB_HEADER_LEN + payload_length;
    if (payload_length > MOBY_USB_MAX_PAYLOAD_LEN || raw_length != body_length + MOBY_USB_CRC_LEN) {
        return MOBY_ERR_BAD_FRAME;
    }
    if (moby_crc32(raw, body_length) != read_u32(raw + body_length)) {
        return MOBY_ERR_BAD_CRC;
    }
    if (raw[MOBY_USB_VERSION_OFFSET] != MOBY_USB_FRAME_VERSION) {
        return MOBY_ERR_UNSUPPORTED_VERSION;
    }
    const uint8_t kind = raw[MOBY_USB_KIND_OFFSET];
    const uint16_t stream_id = read_u16(raw + MOBY_USB_STREAM_ID_OFFSET);
    if (raw[MOBY_USB_FLAGS_OFFSET] != 0 || raw[MOBY_USB_RESERVED_OFFSET] != 0 ||
        !kind_known(kind) || !stream_id_matches_kind(kind, stream_id)) {
        return MOBY_ERR_BAD_FRAME;
    }
    frame->kind = kind;
    frame->stream_id = stream_id;
    frame->sequence = read_u32(raw + MOBY_USB_SEQUENCE_OFFSET);
    frame->payload = raw + MOBY_USB_HEADER_LEN;
    frame->payload_length = payload_length;
    return MOBY_OK;
}

moby_status moby_frame_build(uint8_t kind, uint16_t stream_id, uint32_t sequence,
                             const uint8_t *payload, size_t payload_length, uint8_t *output,
                             size_t output_capacity, size_t *output_length) {
    if ((payload == NULL && payload_length != 0) || output == NULL || output_length == NULL) {
        return MOBY_ERR_BAD_ARGUMENT;
    }
    if (payload_length > MOBY_USB_MAX_PAYLOAD_LEN) {
        return MOBY_ERR_FRAME_TOO_LARGE;
    }
    if (!kind_known(kind) || !stream_id_matches_kind(kind, stream_id)) {
        return MOBY_ERR_BAD_FRAME;
    }
    if (output_capacity == 0) {
        return MOBY_ERR_OUTPUT_TOO_SMALL;
    }

    uint8_t header[MOBY_USB_HEADER_LEN] = {0};
    header[MOBY_USB_VERSION_OFFSET] = MOBY_USB_FRAME_VERSION;
    header[MOBY_USB_KIND_OFFSET] = kind;
    header[MOBY_USB_STREAM_ID_OFFSET] = (uint8_t)(stream_id & 0xffu);
    header[MOBY_USB_STREAM_ID_OFFSET + 1] = (uint8_t)(stream_id >> 8);
    for (unsigned i = 0; i < 4; i++) {
        header[MOBY_USB_SEQUENCE_OFFSET + i] = (uint8_t)(sequence >> (8 * i));
    }
    header[MOBY_USB_LENGTH_OFFSET] = (uint8_t)(payload_length & 0xffu);
    header[MOBY_USB_LENGTH_OFFSET + 1] = (uint8_t)(payload_length >> 8);

    uint32_t crc = moby_crc32(header, sizeof(header));
    crc = moby_crc32_update(crc, payload, payload_length);

    /* Leave room for the delimiter. */
    cobs_encoder encoder;
    encoder_start(&encoder, output, output_capacity - 1);
    for (size_t i = 0; i < sizeof(header); i++) {
        encoder_push(&encoder, header[i]);
    }
    for (size_t i = 0; i < payload_length; i++) {
        encoder_push(&encoder, payload[i]);
    }
    for (unsigned i = 0; i < MOBY_USB_CRC_LEN; i++) {
        encoder_push(&encoder, (uint8_t)(crc >> (8 * i)));
    }
    size_t encoded_length;
    const moby_status status = encoder_finish(&encoder, &encoded_length);
    if (status != MOBY_OK) {
        return status;
    }
    output[encoded_length] = MOBY_USB_DELIMITER;
    *output_length = encoded_length + 1;
    return MOBY_OK;
}

void moby_decoder_reset(moby_decoder *decoder) {
    decoder->length = 0;
    decoder->dropping = false;
}

moby_status moby_decoder_push(moby_decoder *decoder, uint8_t byte, moby_frame *frame) {
    if (byte != MOBY_USB_DELIMITER) {
        if (decoder->length == sizeof(decoder->buffer)) {
            decoder->dropping = true;
        } else {
            decoder->buffer[decoder->length++] = byte;
        }
        return MOBY_PENDING;
    }

    const size_t length = decoder->length;
    const bool dropping = decoder->dropping;
    moby_decoder_reset(decoder);
    if (dropping) {
        return MOBY_ERR_FRAME_TOO_LARGE;
    }
    if (length == 0) {
        return MOBY_PENDING;
    }
    size_t decoded_length;
    if (moby_cobs_decode(decoder->buffer, length, decoder->buffer, MOBY_USB_MAX_DECODED_LEN,
                         &decoded_length) != MOBY_OK) {
        return MOBY_ERR_BAD_FRAME;
    }
    return moby_frame_parse(decoder->buffer, decoded_length, frame);
}

bool moby_stable_id_valid(const char *text) {
    if (text == NULL || text[0] < 'a' || text[0] > 'z') {
        return false;
    }
    size_t length = 1;
    for (; text[length] != '\0'; length++) {
        const char ch = text[length];
        const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                             ch == '-';
        if (!allowed || length >= 64) {
            return false;
        }
    }
    return true;
}

static int hex_value(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    return -1;
}

bool moby_hex_decode(const char *hex, size_t hex_length, uint8_t *output, size_t output_length) {
    if (hex == NULL || (output == NULL && output_length != 0) || hex_length != 2 * output_length) {
        return false;
    }
    for (size_t i = 0; i < output_length; i++) {
        const int high = hex_value(hex[2 * i]);
        const int low = hex_value(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

void moby_hex_encode(const uint8_t *input, size_t length, char *output) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        output[2 * i] = digits[input[i] >> 4];
        output[2 * i + 1] = digits[input[i] & 0x0fu];
    }
}

bool moby_ct_equal(const uint8_t *a, const uint8_t *b, size_t length) {
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0;
}

moby_status moby_session_proof_input(const uint8_t nonce[MOBY_USB_NONCE_BYTES],
                                     const char *device_id, const char *session_id,
                                     uint8_t *output, size_t output_capacity,
                                     size_t *output_length) {
    if (nonce == NULL || output == NULL || output_length == NULL ||
        !moby_stable_id_valid(device_id) || !moby_stable_id_valid(session_id)) {
        return MOBY_ERR_BAD_ARGUMENT;
    }
    const size_t device_length = strlen(device_id);
    const size_t session_length = strlen(session_id);
    const size_t total = MOBY_USB_NONCE_BYTES + device_length + 1 + session_length;
    if (total > output_capacity) {
        return MOBY_ERR_OUTPUT_TOO_SMALL;
    }
    memcpy(output, nonce, MOBY_USB_NONCE_BYTES);
    memcpy(output + MOBY_USB_NONCE_BYTES, device_id, device_length);
    output[MOBY_USB_NONCE_BYTES + device_length] = 0;
    memcpy(output + MOBY_USB_NONCE_BYTES + device_length + 1, session_id, session_length);
    *output_length = total;
    return MOBY_OK;
}
