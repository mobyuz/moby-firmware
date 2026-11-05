// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_JSON_H
#define MOBY_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "moby_msg.h"

typedef enum {
    MOBY_PARSE_OK,
    // Not a JSON object, or the id is missing or invalid, so no response can name it.
    MOBY_PARSE_MALFORMED,
    MOBY_PARSE_VERSION,
    MOBY_PARSE_UNKNOWN_NAME,
    // A known name with the wrong envelope kind, or a name only the robot may send.
    MOBY_PARSE_WRONG_KIND,
    // An unknown, missing, duplicate or out of range field.
    MOBY_PARSE_BAD_DATA,
} moby_parse_status_t;

typedef struct {
    // True when the envelope id was valid, so msg.id can correlate a response.
    bool has_id;
    // True when the envelope kind was `command`.
    bool command;
    moby_msg_t msg;
} moby_parsed_t;

// Parses one control payload from the desktop with cJSON. Rejects unknown names
// and fields, duplicates, wrong kinds, and every out of range value. It never
// keeps the payload and wipes a pair_set_key string before it frees it.
moby_parse_status_t moby_json_parse(const uint8_t *payload, size_t length, moby_parsed_t *parsed);

// Builds the payload of a robot message into buffer. Returns its length, or 0
// when it does not fit or a text is not JSON safe.
size_t moby_json_build(const moby_identity_t *identity, const moby_out_t *out, char *buffer, size_t capacity);

// Validates UTF-8 strictly and counts code points. Returns false for an overlong
// form, a surrogate, a value past U+10FFFF, or a control character (U+0000-001F,
// U+007F-009F).
bool moby_text_valid(const char *text, size_t *code_points);

#endif
