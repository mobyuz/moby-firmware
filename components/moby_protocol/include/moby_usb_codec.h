/*
 * Moby USB v1 framing for firmware and host tests.
 *
 * Dependency free C11: no malloc, no JSON, no HMAC. Control JSON belongs to
 * cJSON and HMAC-SHA256 to mbedTLS on the device. The test vectors in
 * tests/fixtures say what both must produce. Constants come from the
 * generated moby_usb.h. Nothing here is thread safe; use one moby_decoder per
 * serial stream from one task.
 */
#ifndef MOBY_USB_CODEC_H
#define MOBY_USB_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "moby_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum moby_status {
    MOBY_OK = 0,
    /* Decoder only: the byte was consumed and no segment is complete. */
    MOBY_PENDING,
    MOBY_ERR_BAD_FRAME,
    MOBY_ERR_BAD_CRC,
    MOBY_ERR_UNSUPPORTED_VERSION,
    MOBY_ERR_FRAME_TOO_LARGE,
    MOBY_ERR_OUTPUT_TOO_SMALL,
    MOBY_ERR_BAD_ARGUMENT
} moby_status;

/* CRC-32/ISO-HDLC, as zlib: start with 0 and pass the result back to continue. */
uint32_t moby_crc32_update(uint32_t crc, const uint8_t *data, size_t length);
uint32_t moby_crc32(const uint8_t *data, size_t length);

/* The most bytes COBS can produce for length input bytes, without the delimiter. */
size_t moby_cobs_max_encoded_len(size_t length);

/*
 * Standard minimal COBS without the trailing zero delimiter. Returns
 * MOBY_OK, MOBY_ERR_OUTPUT_TOO_SMALL, or MOBY_ERR_BAD_ARGUMENT. On error the
 * output holds partial bytes.
 */
moby_status moby_cobs_encode(const uint8_t *input, size_t input_length, uint8_t *output,
                             size_t output_capacity, size_t *output_length);

/*
 * Decodes one segment that excludes the delimiter. Input and output may be the
 * same buffer. Returns MOBY_ERR_BAD_FRAME for a zero code byte or a block that
 * runs past the end, and MOBY_ERR_OUTPUT_TOO_SMALL when the output cannot hold
 * the result.
 */
moby_status moby_cobs_decode(const uint8_t *input, size_t input_length, uint8_t *output,
                             size_t output_capacity, size_t *output_length);

/* A decoded frame. The payload points into the decoder or parse buffer. */
typedef struct moby_frame {
    uint8_t kind;
    uint16_t stream_id;
    uint32_t sequence;
    const uint8_t *payload;
    size_t payload_length;
} moby_frame;

/* Checks a decoded frame (header, length, CRC, version, reserved, stream rules). */
moby_status moby_frame_parse(const uint8_t *raw, size_t raw_length, moby_frame *frame);

/*
 * Writes a complete wire frame including the zero delimiter. Control frames
 * need stream_id 0 and audio frames need a nonzero one. Needs no scratch
 * buffer. On error the output holds partial bytes and must be discarded.
 */
moby_status moby_frame_build(uint8_t kind, uint16_t stream_id, uint32_t sequence,
                             const uint8_t *payload, size_t payload_length, uint8_t *output,
                             size_t output_capacity, size_t *output_length);

/* A bounded streaming decoder: about 1 KiB of state and no heap. */
typedef struct moby_decoder {
    uint8_t buffer[MOBY_USB_MAX_ENCODED_LEN];
    size_t length;
    bool dropping;
} moby_decoder;

void moby_decoder_reset(moby_decoder *decoder);

/*
 * Feeds one serial byte. Returns MOBY_PENDING while a segment is open or for
 * an idle delimiter, MOBY_OK with *frame set when a frame completes, or the
 * error for a bad segment. A delimiter always resynchronizes. The frame stays
 * valid until the next push or reset.
 */
moby_status moby_decoder_push(moby_decoder *decoder, uint8_t byte, moby_frame *frame);

/* Registry IDs: 1-64 chars, a lowercase letter first, then a-z 0-9 _ - only. */
bool moby_stable_id_valid(const char *text);

/* Lowercase hexadecimal only. hex_length must be exactly 2 * output_length. */
bool moby_hex_decode(const char *hex, size_t hex_length, uint8_t *output, size_t output_length);
void moby_hex_encode(const uint8_t *input, size_t length, char *output);

/* Compares in constant time, for keys and proofs. */
bool moby_ct_equal(const uint8_t *a, const uint8_t *b, size_t length);

/*
 * Builds the bytes the session proof covers: the nonce, the device ID, a zero
 * byte, then the session ID. The proof is HMAC-SHA256(pair_key, input).
 */
moby_status moby_session_proof_input(const uint8_t nonce[MOBY_USB_NONCE_BYTES],
                                     const char *device_id, const char *session_id,
                                     uint8_t *output, size_t output_capacity,
                                     size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif /* MOBY_USB_CODEC_H */
