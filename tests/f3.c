// SPDX-License-Identifier: GPL-3.0-only

// F3 host tests: framing recovery, the session state machine, pairing,
// session authentication, mute priority and the privacy invariants. Everything
// runs against a fake transport and fake storage, with no ESP-IDF header.

#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moby_config.h"
#include "moby_face.h"
#include "moby_font.h"
#include "moby_hmac.h"
#include "moby_json.h"
#include "moby_link.h"
#include "moby_session.h"

enum { LOG_MAX = 16384, TX_MAX = 65536, RX_MAX = 16384, JSON_MAX = 2048 };

typedef struct {
    // The fake transport.
    bool connected;
    bool tx_idle;
    bool write_ok;
    uint8_t rx[RX_MAX];
    size_t rx_length;
    size_t rx_position;
    uint8_t tx[TX_MAX];
    size_t tx_length;
    size_t tx_position;
    uint32_t last_sequence;
    moby_decoder tx_decoder;
    // The fake platform.
    unsigned random_calls;
    unsigned hmac_calls;
    bool random_ok;
    bool key_stored;
    uint8_t key[MOBY_USB_PAIR_KEY_BYTES];
    bool store_ok;
    bool erase_ok;
    unsigned erase_calls;
    char log[LOG_MAX];
    size_t log_length;
    // The code under test.
    moby_identity_t identity;
    moby_session_t session;
    moby_link_t link;
    moby_transport_t transport;
    uint32_t now;
    uint32_t desktop_sequence;
} fx_t;

static fx_t fx_storage;

static bool tr_connected(void *ctx)
{
    return ((fx_t *)ctx)->connected;
}

static size_t tr_read(void *ctx, uint8_t *buffer, size_t capacity)
{
    fx_t *fx = ctx;
    size_t count = fx->rx_length - fx->rx_position;
    count = count < capacity ? count : capacity;
    memcpy(buffer, fx->rx + fx->rx_position, count);
    fx->rx_position += count;
    return count;
}

static bool tr_write(void *ctx, const uint8_t *data, size_t length)
{
    fx_t *fx = ctx;
    if (!fx->write_ok || fx->tx_length + length > TX_MAX) {
        return false;
    }
    memcpy(fx->tx + fx->tx_length, data, length);
    fx->tx_length += length;
    return true;
}

static bool tr_idle(void *ctx)
{
    return ((fx_t *)ctx)->tx_idle;
}

// Different bytes on every call, so two nonces never match.
static bool op_random(void *ctx, uint8_t *out, size_t length)
{
    fx_t *fx = ctx;
    if (!fx->random_ok) {
        return false;
    }
    ++fx->random_calls;
    for (size_t i = 0; i < length; ++i) {
        out[i] = (uint8_t)(fx->random_calls * 29U + i * 7U + 3U);
    }
    return true;
}

static bool op_hmac(void *ctx, const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                    uint8_t out[32])
{
    ++((fx_t *)ctx)->hmac_calls;
    return moby_hmac_sha256(key, key_length, message, message_length, out);
}

static bool op_store(void *ctx, const uint8_t key[MOBY_USB_PAIR_KEY_BYTES])
{
    fx_t *fx = ctx;
    if (!fx->store_ok) {
        return false;
    }
    memcpy(fx->key, key, sizeof fx->key);
    fx->key_stored = true;
    return true;
}

static bool op_erase(void *ctx)
{
    fx_t *fx = ctx;
    ++fx->erase_calls;
    if (!fx->erase_ok) {
        return false;
    }
    fx->key_stored = false;
    return true;
}

static void op_log(void *ctx, const char *line)
{
    fx_t *fx = ctx;
    const size_t length = strlen(line);
    assert(fx->log_length + length + 2 < LOG_MAX);
    memcpy(fx->log + fx->log_length, line, length);
    fx->log_length += length;
    fx->log[fx->log_length++] = '\n';
    fx->log[fx->log_length] = '\0';
}

static const uint8_t TEST_KEY[MOBY_USB_PAIR_KEY_BYTES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
// The desktop_nonce every test ack carries next to its proof (the same value as in robot-proof-vectors.tsv).
#define TEST_DESKTOP_NONCE_HEX "0f1e2d3c4b5a69788796a5b4c3d2e1f0"
static const char TEST_KEY_HEX[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

static fx_t *fx_init(const uint8_t *pair_key, uint32_t capabilities)
{
    fx_t *fx = &fx_storage;
    memset(fx, 0, sizeof *fx);
    fx->tx_idle = true;
    fx->write_ok = true;
    fx->random_ok = true;
    fx->store_ok = true;
    fx->erase_ok = true;
    fx->now = 1000;
    moby_decoder_reset(&fx->tx_decoder);
    snprintf(fx->identity.device_id, sizeof fx->identity.device_id, "dev_0123456789abcdef0123456789abcdef");
    snprintf(fx->identity.hw_rev, sizeof fx->identity.hw_rev, "esp32-s3-r1");
    snprintf(fx->identity.fw_version, sizeof fx->identity.fw_version, "0.1.0");
    fx->identity.capabilities = capabilities;
    fx->transport = (moby_transport_t){ fx, tr_connected, tr_read, tr_write, tr_idle };
    const moby_session_ops_t ops = { fx, op_random, op_hmac, op_store, op_erase, op_log };
    moby_session_init(&fx->session, &fx->identity, pair_key, &ops, fx->now);
    moby_link_init(&fx->link, &fx->transport, &fx->session);
    return fx;
}

static void step(fx_t *fx, uint32_t advance_ms)
{
    fx->now += advance_ms;
    moby_link_step(&fx->link, fx->now);
}

static void push_wire(fx_t *fx, const uint8_t *wire, size_t length)
{
    if (fx->rx_position == fx->rx_length) {
        fx->rx_position = fx->rx_length = 0;
    }
    assert(fx->rx_length + length <= RX_MAX);
    memcpy(fx->rx + fx->rx_length, wire, length);
    fx->rx_length += length;
}

static void desktop_frame(fx_t *fx, uint8_t kind, uint16_t stream, const uint8_t *payload, size_t length)
{
    uint8_t wire[MOBY_USB_MAX_WIRE_LEN];
    size_t wire_length = 0;
    assert(moby_frame_build(kind, stream, fx->desktop_sequence++, payload, length, wire, sizeof wire, &wire_length) == MOBY_OK);
    push_wire(fx, wire, wire_length);
}

static void desktop(fx_t *fx, const char *json)
{
    desktop_frame(fx, MOBY_USB_CONTROL, 0, (const uint8_t *)json, strlen(json));
}

static void command(fx_t *fx, const char *id, const char *name, const char *data)
{
    char json[JSON_MAX];
    snprintf(json, sizeof json, "{\"v\":1,\"kind\":\"command\",\"name\":\"%s\",\"id\":\"%s\",\"ts_ms\":1,\"data\":%s}", name, id, data);
    desktop(fx, json);
}

static bool next_tx(fx_t *fx, char *json)
{
    while (fx->tx_position < fx->tx_length) {
        moby_frame frame;
        const moby_status status = moby_decoder_push(&fx->tx_decoder, fx->tx[fx->tx_position++], &frame);
        if (status == MOBY_OK) {
            assert(frame.kind == MOBY_USB_CONTROL && frame.payload_length < JSON_MAX);
            fx->last_sequence = frame.sequence;
            memcpy(json, frame.payload, frame.payload_length);
            json[frame.payload_length] = '\0';
            return true;
        }
        assert(status == MOBY_PENDING);
    }
    return false;
}

static void drain(fx_t *fx)
{
    char json[JSON_MAX];
    while (next_tx(fx, json)) {
    }
}

static bool has(const char *json, const char *text)
{
    return strstr(json, text) != NULL;
}

// The next message the robot sent must name the given message and contain every extra text.
static void expect(fx_t *fx, const char *name, const char *extra)
{
    char json[JSON_MAX];
    char want[96];
    if (!next_tx(fx, json)) {
        fprintf(stderr, "expected %s %s, got nothing\n", name, extra ? extra : "");
        exit(1);
    }
    snprintf(want, sizeof want, "\"name\":\"%s\"", name);
    if (!has(json, want) || (extra && !has(json, extra))) {
        fprintf(stderr, "expected %s %s, got:\n%s\n", name, extra ? extra : "", json);
        exit(1);
    }
}

static void expect_ok(fx_t *fx, const char *id)
{
    char extra[64];
    snprintf(extra, sizeof extra, "\"id\":\"%s\"", id);
    expect(fx, "command_ok", extra);
}

static void expect_error(fx_t *fx, const char *id, const char *code)
{
    char json[JSON_MAX];
    char want[96];
    if (!next_tx(fx, json)) {
        fprintf(stderr, "expected command_error %s %s, got nothing\n", id, code);
        exit(1);
    }
    snprintf(want, sizeof want, "\"name\":\"command_error\",\"id\":\"%s\"", id);
    if (!has(json, want) || !has(json, code)) {
        fprintf(stderr, "expected command_error %s %s, got:\n%s\n", id, code, json);
        exit(1);
    }
}

static void expect_silence(fx_t *fx)
{
    char json[JSON_MAX];
    if (next_tx(fx, json)) {
        fprintf(stderr, "unexpected message:\n%s\n", json);
        exit(1);
    }
}

// Copies the text after `"key":"` up to the closing quote.
static void field_of(const char *json, const char *key, char *out, size_t capacity)
{
    char pattern[48];
    snprintf(pattern, sizeof pattern, "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    assert(start);
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    assert(end && (size_t)(end - start) < capacity);
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
}

// A control reading, followed by a link pass so what the session queued goes out.
static void controls(fx_t *fx, bool muted, bool talk_down, bool attention)
{
    moby_session_controls(&fx->session, fx->now, muted, talk_down, attention);
    moby_link_step(&fx->link, fx->now);
}

static void press(fx_t *fx)
{
    controls(fx, false, true, true);
}

static void release(fx_t *fx)
{
    controls(fx, false, false, false);
}

// The config object that ack() sends; tests change it to check what an ack may set.
static const char *ack_config = "{}";

static void ack(fx_t *fx, const char *hello, const char *session, const uint8_t *key, bool accepted)
{
    char id[MOBY_ID_MAX + 1];
    char nonce_hex[2 * MOBY_USB_NONCE_BYTES + 1];
    char device[MOBY_ID_MAX + 1];
    char json[JSON_MAX];
    char proof[2 * MOBY_USB_PROOF_BYTES + 64] = "";
    field_of(hello, "id", id, sizeof id);
    field_of(hello, "nonce", nonce_hex, sizeof nonce_hex);
    field_of(hello, "device_id", device, sizeof device);
    if (key) {
        uint8_t nonce[MOBY_USB_NONCE_BYTES];
        uint8_t input[256];
        uint8_t mac[32];
        size_t length = 0;
        assert(moby_hex_decode(nonce_hex, 32, nonce, sizeof nonce));
        assert(moby_session_proof_input(nonce, device, session, input, sizeof input, &length) == MOBY_OK);
        assert(moby_hmac_sha256(key, MOBY_USB_PAIR_KEY_BYTES, input, length, mac));
        strcpy(proof, ",\"proof\":\"");
        moby_hex_encode(mac, sizeof mac, proof + strlen(proof));
        strcat(proof, "\",\"desktop_nonce\":\"" TEST_DESKTOP_NONCE_HEX "\"");
    }
    snprintf(json, sizeof json,
             "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":5,\"data\":{\"accepted\":%s%s%s%s%s,"
             "\"config\":%s}}",
             id, accepted ? "true" : "false", session ? ",\"session_id\":\"" : "", session ? session : "",
             session ? "\"" : "", proof, ack_config);
    desktop(fx, json);
}

// Brings a session up: connect, optionally unmute, answer the hello, and drop the opening events.
static void establish(fx_t *fx, bool muted, const uint8_t *key)
{
    char hello[JSON_MAX];
    fx->connected = true;
    step(fx, 0);
    if (!muted) {
        controls(fx, false, false, false);
    }
    assert(next_tx(fx, hello));
    ack(fx, hello, "session_1", key, true);
    step(fx, 10);
    assert(fx->session.established);
    if (fx->session.authenticated) {
        // The robot's proof of the key is the first thing it sends on an authenticated session.
        expect(fx, "session_proof", NULL);
    }
    expect(fx, "mute_changed", "\"source\":\"boot\"");
}

// The view lives until the next call.
static const moby_face_view_t *view_of(fx_t *fx)
{
    static moby_face_view_t view;
    moby_session_view(&fx->session, &view);
    return &view;
}

// --- Framing and link -------------------------------------------------------

static void test_hello_on_connect_and_retry(void)
{
    fx_t *fx = fx_init(NULL, 0);
    step(fx, 0);
    expect_silence(fx);
    assert(fx->session.state == MOBY_SESSION_DISCONNECTED);

    fx->connected = true;
    step(fx, 0);
    char first[JSON_MAX];
    char again[JSON_MAX];
    // The very first byte is a lone delimiter, so a host that saw ROM boot text resynchronizes.
    assert(fx->tx_length > 1 && fx->tx[0] == MOBY_USB_DELIMITER && fx->tx[1] != MOBY_USB_DELIMITER);
    assert(next_tx(fx, first));
    assert(has(first, "\"name\":\"hello\"") && has(first, "\"paired\":false") && has(first, "\"capabilities\":[]") &&
           has(first, "\"mute\":true") && has(first, "\"protocol_version\":1"));
    step(fx, 999);
    expect_silence(fx);
    step(fx, 1);
    assert(next_tx(fx, again));
    assert(strcmp(strstr(first, "\"id\""), strstr(again, "\"id\"")) != 0 || has(again, "\"ts_ms\":2000"));
    char id1[16], id2[16], n1[40], n2[40];
    field_of(first, "id", id1, sizeof id1);
    field_of(again, "id", id2, sizeof id2);
    field_of(first, "nonce", n1, sizeof n1);
    field_of(again, "nonce", n2, sizeof n2);
    assert(strcmp(id1, id2) == 0 && strcmp(n1, n2) == 0);

    // The 3 s handshake window ends: a new attempt with a fresh nonce and id.
    step(fx, 1000);
    expect(fx, "hello", NULL);
    step(fx, 1000);
    char fresh[JSON_MAX];
    assert(next_tx(fx, fresh));
    field_of(fresh, "nonce", n2, sizeof n2);
    field_of(fresh, "id", id2, sizeof id2);
    assert(strcmp(n1, n2) != 0 && strcmp(id1, id2) != 0);
    assert(fx->session.state == MOBY_SESSION_DISCONNECTED);
    assert(view_of(fx)->expression == MOBY_FACE_MUTED);
}

static void test_handshake_ack_paths(void)
{
    fx_t *fx = fx_init(NULL, 0);
    char hello[JSON_MAX];
    fx->connected = true;
    step(fx, 0);
    assert(next_tx(fx, hello));
    // An ack for another hello is ignored.
    desktop(fx, "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"e-9999\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
                "\"session_id\":\"session_1\",\"config\":{}}}");
    step(fx, 10);
    assert(!fx->session.established);
    // An accepted ack without a session id is a protocol error and does not admit the session.
    char id[16];
    field_of(hello, "id", id, sizeof id);
    char json[JSON_MAX];
    snprintf(json, sizeof json,
             "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
             "\"config\":{}}}", id);
    desktop(fx, json);
    step(fx, 10);
    assert(!fx->session.established);
    expect(fx, "error", "E_BAD_FRAME");
    // A rejection ends the attempt quietly: no resend until the window is over.
    ack(fx, hello, NULL, NULL, false);
    step(fx, 10);
    assert(!fx->session.established && fx->session.hello_rejected);
    step(fx, 1500);
    expect_silence(fx);
    step(fx, 1500);
    expect(fx, "hello", NULL);
    // The accepted ack of the new attempt admits the session and carries config.
    assert(next_tx(fx, hello) == false);
    fx->tx_position = 0;
    fx->tx_length = 0;
    moby_session_set_link(&fx->session, fx->now, false);
    moby_session_set_link(&fx->session, fx->now, true);
    drain(fx);
    fx->connected = true;
    step(fx, 0);
    assert(fx->session.hello_open);
}

static void test_session_flow_and_late_ack(void)
{
    fx_t *fx = fx_init(NULL, 0);
    char hello[JSON_MAX];
    fx->connected = true;
    step(fx, 0);
    controls(fx, false, false, false);
    assert(next_tx(fx, hello));
    desktop(fx, "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"e-0000\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
                "\"session_id\":\"session_1\",\"config\":{\"speaker_volume\":35,\"wake_sensitivity\":null}}}");
    step(fx, 5);
    assert(fx->session.established && fx->session.state == MOBY_SESSION_IDLE && !fx->session.authenticated);
    assert(fx->session.config.speaker_volume == 35 && fx->session.config.wake_sensitivity == 50);
    // The mute reading arrived before the ack, so the opening event reports it.
    expect(fx, "mute_changed", "\"muted\":false,\"source\":\"boot\"");
    expect_silence(fx);
    assert(view_of(fx)->expression == MOBY_FACE_IDLE);
    // A repeated or late ack changes nothing.
    desktop(fx, "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"e-0000\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
                "\"session_id\":\"session_2\",\"config\":{}}}");
    step(fx, 5);
    assert(strcmp(fx->session.session_id, "session_1") == 0);
}

static void test_heartbeat_content(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    const uint32_t start = fx->now;
    step(fx, 1999);
    expect_silence(fx);
    step(fx, 1);
    char json[JSON_MAX];
    assert(next_tx(fx, json));
    char want[96];
    snprintf(want, sizeof want, "\"data\":{\"state\":\"idle\",\"mute\":false,\"uptime_ms\":%u}", (unsigned)fx->now);
    assert(has(json, "\"name\":\"heartbeat\"") && has(json, want));
    assert(fx->now == start + 2000);
    // The desktop commands a state and mutes the robot: the next heartbeat tells the truth.
    command(fx, "c-0001", "expression", "{\"state\":\"thinking\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    step(fx, 2000);
    expect(fx, "heartbeat", "\"state\":\"thinking\",\"mute\":false");
    controls(fx, true, false, false);
    expect(fx, "mute_changed", "\"muted\":true,\"source\":\"switch\"");
    step(fx, 2000);
    expect(fx, "heartbeat", "\"state\":\"muted\",\"mute\":true");
}

static void test_missed_heartbeats_end_session(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "display_text", "{\"text\":\"Salom\",\"ttl_ms\":60000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    assert(view_of(fx)->text[0]);

    // Three heartbeats go unanswered; the fourth period ends the session.
    for (int beat = 0; beat < 3; ++beat) {
        step(fx, 2000);
        expect(fx, "heartbeat", NULL);
        assert(fx->session.established);
    }
    step(fx, 1999);
    assert(fx->session.established);
    step(fx, 1);
    assert(!fx->session.established && fx->session.state == MOBY_SESSION_DISCONNECTED);
    // Buffers clear and the face shows disconnected without the old text.
    const moby_face_view_t view = *view_of(fx);
    assert(view.expression == MOBY_FACE_DISCONNECTED && view.text[0] == '\0');
    assert(fx->session.text[0] == '\0' && fx->session.outbox_count == 0);
    expect(fx, "hello", NULL);
    expect_silence(fx);
    assert(!moby_session_recording(&fx->session));

    // A new hello_ack restores the session, and idle comes first.
    fx->tx_position = fx->tx_length = 0;
    step(fx, 1000);
    char hello[JSON_MAX];
    assert(next_tx(fx, hello));
    ack(fx, hello, "session_2", NULL, true);
    step(fx, 1);
    assert(fx->session.established && view_of(fx)->expression == MOBY_FACE_IDLE);
}

static void test_desktop_traffic_keeps_the_session(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    for (int period = 0; period < 10; ++period) {
        command(fx, "c-0001", "set_config", "{}");
        step(fx, 2000);
        assert(fx->session.established);
    }
}

static void test_reconnect(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    char first[JSON_MAX];
    fx->connected = false;
    step(fx, 100);
    assert(!fx->session.established && fx->session.state == MOBY_SESSION_DISCONNECTED);
    assert(view_of(fx)->expression == MOBY_FACE_DISCONNECTED);
    fx->connected = true;
    step(fx, 100);
    // Whatever the session queued before the link went down is gone, and hello restarts at once.
    drain(fx);
    fx->connected = false;
    step(fx, 10);
    fx->tx_position = fx->tx_length = 0;
    fx->connected = true;
    step(fx, 10);
    assert(next_tx(fx, first));
    assert(has(first, "\"name\":\"hello\""));
}

static void test_boot_state(void)
{
    fx_t *fx = fx_init(NULL, 0);
    assert(fx->session.state == MOBY_SESSION_BOOT && fx->session.muted);
    assert(!moby_session_recording(&fx->session));
    assert(view_of(fx)->expression == MOBY_FACE_MUTED);
    moby_session_tick(&fx->session, fx->now);
    assert(fx->session.state == MOBY_SESSION_DISCONNECTED);
    controls(fx, false, false, false);
    assert(view_of(fx)->expression == MOBY_FACE_DISCONNECTED);
}

static void test_clock_wraps(void)
{
    fx_t *fx = fx_init(NULL, 0);
    fx->now = UINT32_MAX - 2500;
    establish(fx, false, NULL);
    step(fx, 2000);
    expect(fx, "heartbeat", NULL);
    step(fx, 2000);
    expect(fx, "heartbeat", NULL);
    assert(fx->now < 2000);
    command(fx, "c-0001", "display_text", "{\"text\":\"wrap\",\"ttl_ms\":1000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    assert(view_of(fx)->text[0]);
    step(fx, 1000);
    assert(!view_of(fx)->text[0]);
}

static void test_frame_errors_and_recovery(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    // Boot text from the ROM, then the host's delimiter: one bad segment, and the decoder is back in sync.
    const uint8_t rom[] = "ESP-ROM:esp32s3-20210327\r\nrst:0x1 (POWERON)\r\n";
    push_wire(fx, rom, sizeof rom);
    step(fx, 0);
    expect(fx, "error", "E_BAD_FRAME");
    step(fx, 600);
    // A corrupted CRC is reported and the next frame still works.
    uint8_t wire[MOBY_USB_MAX_WIRE_LEN];
    size_t length = 0;
    const uint8_t payload[] = "{}";
    assert(moby_frame_build(MOBY_USB_CONTROL, 0, 7, payload, 2, wire, sizeof wire, &length) == MOBY_OK);
    wire[length - 3] ^= 0x55;
    push_wire(fx, wire, length);
    step(fx, 0);
    expect(fx, "error", "E_CRC");
    assert(fx->link.stats.bad_crc == 1);
    command(fx, "c-0001", "set_config", "{}");
    step(fx, 0);
    expect_ok(fx, "c-0001");

    // An oversized segment is dropped through its delimiter and the next frame decodes.
    step(fx, 600);
    drain(fx);
    uint8_t flood[3000];
    memset(flood, 0xff, sizeof flood);
    push_wire(fx, flood, sizeof flood);
    const uint8_t delimiter = 0;
    push_wire(fx, &delimiter, 1);
    command(fx, "c-0002", "set_config", "{}");
    // One pass reads at most 2 KiB, so the flood takes two.
    step(fx, 0);
    step(fx, 0);
    expect(fx, "error", "E_BAD_FRAME");
    expect_ok(fx, "c-0002");
    assert(fx->link.stats.oversized_frames == 1);
}

static void test_bad_frame_version(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    // A frame with version 2 and a correct CRC.
    uint8_t raw[MOBY_USB_HEADER_LEN + MOBY_USB_CRC_LEN] = { 2, MOBY_USB_CONTROL };
    const uint32_t crc = moby_crc32(raw, MOBY_USB_HEADER_LEN);
    for (unsigned i = 0; i < 4; ++i) {
        raw[MOBY_USB_HEADER_LEN + i] = (uint8_t)(crc >> (8 * i));
    }
    uint8_t wire[64];
    size_t length = 0;
    assert(moby_cobs_encode(raw, sizeof raw, wire, sizeof wire, &length) == MOBY_OK);
    wire[length++] = 0;
    push_wire(fx, wire, length);
    step(fx, 0);
    expect(fx, "error", "E_PROTO_VERSION");
    assert(fx->link.stats.bad_version == 1);
}

static void test_audio_frames_rejected(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    const uint8_t pcm[640] = { 1, 2, 3 };
    desktop_frame(fx, MOBY_USB_DESKTOP_AUDIO, 2, pcm, sizeof pcm);
    step(fx, 0);
    expect(fx, "error", "E_UNSUPPORTED");
    assert(fx->link.stats.desktop_audio_rejected == 1);
    // A robot-audio frame arriving from the desktop is the wrong direction.
    step(fx, 600);
    desktop_frame(fx, MOBY_USB_ROBOT_AUDIO, 3, pcm, sizeof pcm);
    step(fx, 0);
    expect(fx, "error", "E_BAD_FRAME");
    // Audio never reaches the face or the command path.
    assert(view_of(fx)->expression == MOBY_FACE_IDLE);
}

static void test_error_rate_limit(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    for (int i = 0; i < 10; ++i) {
        desktop(fx, "not json");
        step(fx, 40);
    }
    expect(fx, "error", "E_BAD_FRAME");
    char json[JSON_MAX];
    while (next_tx(fx, json)) {
        assert(!has(json, "\"name\":\"error\"") || fx->now - 1000 >= 400);
    }
    assert(fx->session.stats.errors_suppressed >= 6);
    step(fx, 600);
    desktop(fx, "not json");
    step(fx, 0);
    expect(fx, "error", "E_BAD_FRAME");
}

static void test_write_failures_are_counted(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    // A host that does not collect: hello and heartbeat are dropped, not queued behind each other.
    fx->tx_idle = false;
    step(fx, 2000);
    assert(fx->link.stats.frames_dropped == 1);
    expect_silence(fx);
    fx->tx_idle = true;
    fx->write_ok = false;
    command(fx, "c-0001", "set_config", "{}");
    step(fx, 0);
    assert(fx->link.stats.frames_dropped == 2);
    fx->write_ok = true;
    command(fx, "c-0002", "set_config", "{}");
    step(fx, 0);
    // The sequence numbers of the dropped frames were spent, so the desktop can see the gap.
    expect_ok(fx, "c-0002");
    assert(fx->last_sequence == 4);
}

static void test_sequence_gaps(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    const uint32_t before = fx->session.stats.rx_sequence_gaps;
    fx->desktop_sequence += 5;
    command(fx, "c-0001", "set_config", "{}");
    command(fx, "c-0002", "set_config", "{}");
    step(fx, 0);
    assert(fx->session.stats.rx_sequence_gaps == before + 1);
}

// --- Control messages -------------------------------------------------------

static void test_bad_messages(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    static const struct {
        const char *json;
        const char *code;
        const char *id;
    } cases[] = {
        // Unknown command names and unknown fields are rejected, with the command's id.
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"reboot\",\"id\":\"c-1\",\"ts_ms\":1,\"data\":{}}", "E_UNSUPPORTED", "c-1" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"c-2\",\"ts_ms\":1,\"data\":{\"brightness\":5}}", "E_BAD_FRAME", "c-2" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"c-3\",\"ts_ms\":1,\"data\":{},\"extra\":1}", "E_BAD_FRAME", "c-3" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"c-4\",\"ts_ms\":1,\"data\":{\"speaker_volume\":101}}", "E_BAD_FRAME", "c-4" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"c-5\",\"ts_ms\":1,\"data\":{\"speaker_volume\":5.5}}", "E_BAD_FRAME", "c-5" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"c-6\",\"ts_ms\":1,\"data\":{\"speaker_volume\":1,\"speaker_volume\":2}}", "E_BAD_FRAME", "c-6" },
        { "{\"v\":2,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"c-7\",\"ts_ms\":1,\"data\":{}}", "E_PROTO_VERSION", "c-7" },
        // A name only the robot sends, or the wrong kind for the name.
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"heartbeat\",\"id\":\"c-8\",\"ts_ms\":1,\"data\":{\"state\":\"idle\",\"mute\":false,\"uptime_ms\":1}}", "E_BAD_FRAME", "c-8" },
        { "{\"v\":1,\"kind\":\"event\",\"name\":\"cancel\",\"id\":\"c-9\",\"ts_ms\":1,\"data\":{\"stream_id\":1}}", "E_BAD_FRAME", NULL },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"Cancel\",\"id\":\"c-10\",\"ts_ms\":1,\"data\":{\"stream_id\":1}}", "E_UNSUPPORTED", "c-10" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"cancel\",\"id\":\"c-11\",\"ts_ms\":1,\"data\":{\"stream_id\":0}}", "E_BAD_FRAME", "c-11" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"display_text\",\"id\":\"c-12\",\"ts_ms\":1,\"data\":{\"text\":\"a\\nb\",\"ttl_ms\":1000}}", "E_BAD_FRAME", "c-12" },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"display_text\",\"id\":\"c-13\",\"ts_ms\":1,\"data\":{\"text\":\"a\\u0000b\",\"ttl_ms\":1000}}", "E_BAD_FRAME", NULL },
        // No usable id: an unsolicited error event.
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"C 1\",\"ts_ms\":1,\"data\":{}}", "E_BAD_FRAME", NULL },
        { "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"ts_ms\":1,\"data\":{}}", "E_BAD_FRAME", NULL },
        { "[1,2,3]", "E_BAD_FRAME", NULL },
        { "{\"v\":1,\"kind\":\"command\"", "E_BAD_FRAME", NULL },
        { "", "E_BAD_FRAME", NULL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        step(fx, 600);
        drain(fx);
        if (cases[i].json[0]) {
            desktop(fx, cases[i].json);
        } else {
            desktop_frame(fx, MOBY_USB_CONTROL, 0, (const uint8_t *)"", 0);
        }
        step(fx, 0);
        char json[JSON_MAX];
        assert(next_tx(fx, json));
        if (cases[i].id) {
            expect_silence(fx);
            char want[64];
            snprintf(want, sizeof want, "\"name\":\"command_error\",\"id\":\"%s\"", cases[i].id);
            if (!has(json, want) || !has(json, cases[i].code)) {
                fprintf(stderr, "case %zu: got %s\n", i, json);
                exit(1);
            }
        } else if (!has(json, "\"name\":\"error\"") || !has(json, cases[i].code)) {
            fprintf(stderr, "case %zu: got %s\n", i, json);
            exit(1);
        }
    }
    assert(view_of(fx)->expression == MOBY_FACE_IDLE && fx->session.established);
}

static void test_hostile_payloads(void)
{
    moby_parsed_t parsed;
    // Deep nesting is refused before cJSON recurses.
    char deep[800];
    memset(deep, '[', 700);
    memset(deep + 700, ']', 700 > 99 ? 99 : 0);
    deep[799] = '\0';
    assert(moby_json_parse((const uint8_t *)deep, strlen(deep), &parsed) == MOBY_PARSE_MALFORMED);
    const char nested[] = "{\"v\":1,\"kind\":\"command\",\"name\":\"set_config\",\"id\":\"c-1\",\"ts_ms\":1,\"data\":{\"a\":{\"b\":{\"c\":{}}}}}";
    assert(moby_json_parse((const uint8_t *)nested, strlen(nested), &parsed) == MOBY_PARSE_MALFORMED);
    // Control bytes, invalid UTF-8, overlong forms and surrogates.
    assert(moby_json_parse((const uint8_t *)"{\"v\":1}\x01", 8, &parsed) == MOBY_PARSE_MALFORMED);
    size_t points = 0;
    assert(moby_text_valid("Salom \xf0\x9f\x8c\x90", &points) && points == 7);
    assert(!moby_text_valid("\xc0\xaf", &points) && !moby_text_valid("\xed\xa0\x80", &points));
    assert(!moby_text_valid("\xf4\x90\x80\x80", &points) && !moby_text_valid("\x80", &points));
    assert(!moby_text_valid("a\x7f", &points) && !moby_text_valid("a\xc2\x85", &points));
    assert(!moby_text_valid("\xe2\x82", &points));
    // Escaped backslash followed by u0000 is the literal text, not a NUL escape.
    const char literal[] = "{\"v\":1,\"kind\":\"command\",\"name\":\"display_text\",\"id\":\"c-1\",\"ts_ms\":1,\"data\":{\"text\":\"\\\\u0000\",\"ttl_ms\":1000}}";
    assert(moby_json_parse((const uint8_t *)literal, strlen(literal), &parsed) == MOBY_PARSE_OK);
    assert(strcmp(parsed.msg.u.display_text.text, "\\u0000") == 0);
    // 64 code points are fine, 65 are not.
    char text[300];
    char json[600];
    for (unsigned count = 64; count <= 65; ++count) {
        text[0] = '\0';
        for (unsigned i = 0; i < count; ++i) {
            strcat(text, "\xf0\x9f\x8c\x90");
        }
        snprintf(json, sizeof json,
                 "{\"v\":1,\"kind\":\"command\",\"name\":\"display_text\",\"id\":\"c-1\",\"ts_ms\":1,\"data\":{\"text\":\"%s\",\"ttl_ms\":1000}}",
                 text);
        assert(moby_json_parse((const uint8_t *)json, strlen(json), &parsed) == (count == 64 ? MOBY_PARSE_OK : MOBY_PARSE_BAD_DATA));
    }
}

static void test_expression_transitions_exhaustive(void)
{
    static const char *const known[] = { "idle", "listening", "thinking", "speaking", "happy", "confused", "error", "muted", "disconnected" };
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    for (int from = 0; from < MOBY_EXPRESSION_COUNT - 2; ++from) {
        for (int to = 0; to < MOBY_EXPRESSION_COUNT; ++to) {
            fx->session.face = (moby_expression)from;
            fx->session.error_active = false;
            fx->session.state = MOBY_SESSION_IDLE;
            char data[96];
            snprintf(data, sizeof data, "{\"state\":\"%s\",\"animation\":\"none\",\"duration_ms\":0}", known[to]);
            command(fx, "c-0001", "expression", data);
            step(fx, 0);
            // F4: listening is shown only while capture is on, so the desktop cannot pick it either.
            const bool owned = to == MOBY_EXPRESSION_MUTED || to == MOBY_EXPRESSION_DISCONNECTED || to == MOBY_EXPRESSION_LISTENING;
            if (owned || !moby_can_transition((moby_expression)from, (moby_expression)to)) {
                expect_error(fx, "c-0001", "E_STATE");
                assert(fx->session.face == (moby_expression)from);
            } else {
                expect_ok(fx, "c-0001");
                assert(fx->session.face == (moby_expression)to);
            }
        }
    }
    // The face follows only validated transitions.
    fx->session.face = MOBY_EXPRESSION_IDLE;
    command(fx, "c-0002", "expression", "{\"state\":\"speaking\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_error(fx, "c-0002", "E_STATE");
    assert(view_of(fx)->expression == MOBY_FACE_IDLE);
}

static void test_expression_details(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "expression", "{\"state\":\"thinking\",\"animation\":\"spin\",\"duration_ms\":0}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_UNSUPPORTED");
    assert(view_of(fx)->expression == MOBY_FACE_IDLE);
    command(fx, "c-0002", "expression", "{\"state\":\"thinking\",\"animation\":\"blink\",\"duration_ms\":1500}");
    step(fx, 0);
    expect_ok(fx, "c-0002");
    assert(fx->session.state == MOBY_SESSION_THINKING && view_of(fx)->expression == MOBY_FACE_THINKING);
    step(fx, 1499);
    assert(view_of(fx)->expression == MOBY_FACE_THINKING);
    step(fx, 1);
    assert(view_of(fx)->expression == MOBY_FACE_IDLE && fx->session.state == MOBY_SESSION_IDLE);
    // Happy and confused are faces of the idle state.
    command(fx, "c-0003", "expression", "{\"state\":\"happy\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_ok(fx, "c-0003");
    assert(fx->session.state == MOBY_SESSION_IDLE && view_of(fx)->expression == MOBY_FACE_HAPPY);
}

static void test_display_text(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "display_text", "{\"text\":\"Salom, dunyo \xf0\x9f\x8c\x90\",\"ttl_ms\":3000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    assert(strcmp(view_of(fx)->text, "Salom, dunyo ?") == 0);
    step(fx, 2999);
    assert(view_of(fx)->text[0]);
    step(fx, 1);
    assert(view_of(fx)->text[0] == '\0');
    drain(fx);

    // Cyrillic and the Uzbek turned comma show as their own display cells, one per letter.
    command(fx, "c-0004", "display_text",
            "{\"text\":\"\xd0\x8e\xd0\xb7\xd0\xb1\xd0\xb5\xd0\xba O\xca\xbbz\",\"ttl_ms\":3000}");
    step(fx, 0);
    expect_ok(fx, "c-0004");
    assert(strcmp(view_of(fx)->text, "\xc2\xa8\xa2\xa6\xab O\xca" "z") == 0);
    drain(fx);

    // The longest text stays inside the bound, and a newer text replaces an older one.
    char data[400] = "{\"text\":\"";
    for (int i = 0; i < 64; ++i) {
        strcat(data, "\xf0\x9f\x8c\x90");
    }
    strcat(data, "\",\"ttl_ms\":60000}");
    command(fx, "c-0002", "display_text", data);
    step(fx, 0);
    expect_ok(fx, "c-0002");
    assert(strlen(view_of(fx)->text) == 64);
    command(fx, "c-0003", "display_text", "{\"text\":\"Yangi\",\"ttl_ms\":100}");
    step(fx, 0);
    expect_ok(fx, "c-0003");
    assert(strcmp(view_of(fx)->text, "Yangi") == 0);
    // An error face hides the text; the text is not lost for a moment's error.
    fx->session.error_active = true;
    fx->session.error_until_ms = fx->now + 50;
    assert(view_of(fx)->text[0] == '\0' && view_of(fx)->expression == MOBY_FACE_ERROR);
}

static void test_set_config(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "set_config", "{\"wake_sensitivity\":60,\"speaker_volume\":35,\"vad_silence_ms\":800,\"max_utterance_ms\":15000}");
    command(fx, "c-0002", "set_config", "{\"speaker_volume\":null,\"wake_sensitivity\":10}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    expect_ok(fx, "c-0002");
    assert(fx->session.config.wake_sensitivity == 10 && fx->session.config.speaker_volume == 35);
    assert(fx->session.config.vad_silence_ms == 800 && fx->session.config.max_utterance_ms == 15000);
}

static void test_unsupported_and_cancel(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "servo_pose", "{\"pose\":\"nod\",\"duration_ms\":600}");
    command(fx, "c-0002", "audio_play_start", "{\"stream_id\":2,\"codec\":\"pcm_s16le\",\"sample_rate\":16000}");
    command(fx, "c-0003", "audio_play_end", "{\"stream_id\":2,\"codec\":\"pcm_s16le\",\"sample_rate\":16000}");
    command(fx, "c-0004", "cancel", "{\"stream_id\":1}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_UNSUPPORTED");
    expect_error(fx, "c-0002", "E_UNSUPPORTED");
    expect_error(fx, "c-0003", "E_UNSUPPORTED");
    expect_error(fx, "c-0004", "E_STATE");

    // With the capabilities present the command is known but not built in this stage.
    fx = fx_init(NULL, MOBY_CAP_USB_PCM | MOBY_CAP_SERVO_POSE);
    establish(fx, false, NULL);
    command(fx, "c-0001", "servo_pose", "{\"pose\":\"nod\",\"duration_ms\":600}");
    command(fx, "c-0002", "audio_play_start", "{\"stream_id\":2,\"codec\":\"pcm_s16le\",\"sample_rate\":16000}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_STATE");
    expect_error(fx, "c-0002", "E_STATE");

    // Before a session every command is a state error.
    fx = fx_init(NULL, 0);
    fx->connected = true;
    step(fx, 0);
    drain(fx);
    command(fx, "c-0009", "set_config", "{}");
    step(fx, 0);
    expect_error(fx, "c-0009", "E_STATE");
}

// --- Pairing ------------------------------------------------------------------

static void test_pairing_happy_path(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    assert(fx->session.state == MOBY_SESSION_PAIRING);
    // Expression commands wait while pairing is in progress.
    command(fx, "c-0002", "expression", "{\"state\":\"thinking\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_error(fx, "c-0002", "E_BUSY");
    // The press confirms pairing and is not reported as a button event.
    step(fx, 5000);
    drain(fx);
    press(fx);
    step(fx, 0);
    expect(fx, "pair_button_confirmed", NULL);
    release(fx);
    step(fx, 0);
    expect_silence(fx);
    char data[120];
    snprintf(data, sizeof data, "{\"pair_key\":\"%s\"}", TEST_KEY_HEX);
    command(fx, "c-0003", "pair_set_key", data);
    step(fx, 0);
    expect_ok(fx, "c-0003");
    assert(fx->key_stored && memcmp(fx->key, TEST_KEY, sizeof TEST_KEY) == 0);
    assert(fx->session.paired && fx->session.authenticated && fx->session.state == MOBY_SESSION_IDLE);
    // A later press is an ordinary button event.
    press(fx);
    step(fx, 0);
    expect(fx, "button_event", "\"action\":\"press\"");
    release(fx);
    step(fx, 0);
    expect(fx, "button_event", "\"action\":\"release\"");
}

static void test_pairing_wrong_order(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    char data[120];
    snprintf(data, sizeof data, "{\"pair_key\":\"%s\"}", TEST_KEY_HEX);
    // The key before pair_begin.
    command(fx, "c-0001", "pair_set_key", data);
    step(fx, 0);
    expect_error(fx, "c-0001", "E_STATE");
    // The key after pair_begin but before the button.
    command(fx, "c-0002", "pair_begin", "{\"timeout_ms\":30000}");
    command(fx, "c-0003", "pair_set_key", data);
    step(fx, 0);
    expect_ok(fx, "c-0002");
    expect_error(fx, "c-0003", "E_STATE");
    assert(!fx->key_stored && !fx->session.paired && fx->session.state == MOBY_SESSION_PAIRING);
    // A second pair_begin while waiting.
    command(fx, "c-0004", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_error(fx, "c-0004", "E_BUSY");
    // Pairing from a non-idle state.
    fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "expression", "{\"state\":\"thinking\",\"animation\":\"none\",\"duration_ms\":0}");
    command(fx, "c-0002", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    expect_error(fx, "c-0002", "E_STATE");
}

static void test_already_paired_refuses_pair_begin(void)
{
    fx_t *fx = fx_init(TEST_KEY, 0);
    establish(fx, false, TEST_KEY);
    assert(fx->session.authenticated);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_STATE");
    assert(fx->session.state == MOBY_SESSION_IDLE);
    // Even a lucky key command cannot overwrite the stored key.
    press(fx);
    step(fx, 0);
    expect(fx, "button_event", "\"action\":\"press\"");
    char data[120];
    snprintf(data, sizeof data, "{\"pair_key\":\"%s\"}", "ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00");
    command(fx, "c-0002", "pair_set_key", data);
    step(fx, 0);
    expect_error(fx, "c-0002", "E_STATE");
    assert(!fx->key_stored && memcmp(fx->session.pair_key, TEST_KEY, sizeof TEST_KEY) == 0);
}

static void test_pairing_timeout_and_cancel(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":1000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    step(fx, 999);
    assert(fx->session.state == MOBY_SESSION_PAIRING);
    step(fx, 1);
    expect(fx, "error", "E_TIMEOUT");
    assert(fx->session.state == MOBY_SESSION_IDLE && view_of(fx)->expression == MOBY_FACE_ERROR);
    char data[120];
    snprintf(data, sizeof data, "{\"pair_key\":\"%s\"}", TEST_KEY_HEX);
    // A press after the window is an ordinary button press, and a key now is refused.
    press(fx);
    step(fx, 0);
    expect(fx, "button_event", "\"action\":\"press\"");
    command(fx, "c-0002", "pair_set_key", data);
    step(fx, 0);
    expect_error(fx, "c-0002", "E_STATE");
    step(fx, 3000);
    assert(view_of(fx)->expression == MOBY_FACE_IDLE);

    // The link going away cancels a confirmed pairing.
    release(fx);
    drain(fx);
    command(fx, "c-0003", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0003");
    press(fx);
    step(fx, 0);
    expect(fx, "pair_button_confirmed", NULL);
    fx->connected = false;
    step(fx, 10);
    assert(!fx->session.pair_confirmed && fx->session.state == MOBY_SESSION_DISCONNECTED);
    fx->connected = true;
    step(fx, 10);
    char hello[JSON_MAX];
    drain(fx);
    fx->tx_position = fx->tx_length = 0;
    step(fx, 1000);
    assert(next_tx(fx, hello));
    ack(fx, hello, "session_9", NULL, true);
    step(fx, 10);
    command(fx, "c-0004", "pair_set_key", data);
    step(fx, 0);
    drain(fx);
    assert(!fx->key_stored);
}

static void test_pairing_and_mute(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, true, NULL);
    // pair_begin while muted is refused and a press while muted confirms nothing.
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_MUTED");
    controls(fx, true, true, false);
    step(fx, 0);
    expect_silence(fx);
    controls(fx, true, false, false);
    controls(fx, false, false, false);
    expect(fx, "mute_changed", "\"muted\":false");
    command(fx, "c-0002", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0002");
    press(fx);
    step(fx, 0);
    expect(fx, "pair_button_confirmed", NULL);
    // The mute cancels the pairing at once.
    controls(fx, true, false, false);
    expect(fx, "mute_changed", "\"muted\":true");
    expect(fx, "error", "E_MUTED");
    assert(fx->session.state == MOBY_SESSION_IDLE && !fx->session.pair_confirmed);
    char data[120];
    snprintf(data, sizeof data, "{\"pair_key\":\"%s\"}", TEST_KEY_HEX);
    controls(fx, false, false, false);
    expect(fx, "mute_changed", "\"muted\":false");
    command(fx, "c-0003", "pair_set_key", data);
    step(fx, 0);
    expect_error(fx, "c-0003", "E_STATE");
    assert(!fx->key_stored);
}

static void test_pairing_store_failure(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    press(fx);
    step(fx, 0);
    expect(fx, "pair_button_confirmed", NULL);
    char data[120];
    snprintf(data, sizeof data, "{\"pair_key\":\"%s\"}", TEST_KEY_HEX);
    fx->store_ok = false;
    command(fx, "c-0002", "pair_set_key", data);
    step(fx, 0);
    expect_error(fx, "c-0002", "E_HW");
    assert(!fx->session.paired && fx->session.state == MOBY_SESSION_PAIRING);
    fx->store_ok = true;
    command(fx, "c-0003", "pair_set_key", data);
    step(fx, 0);
    expect_ok(fx, "c-0003");
    assert(fx->session.paired);
}

// Holds the controls steady for total milliseconds while the desktop keeps the session alive.
static void hold(fx_t *fx, uint32_t total, bool muted, bool talk, bool attention)
{
    while (total > 0) {
        const uint32_t chunk = total < 1000 ? total : 1000;
        total -= chunk;
        if (fx->session.established) {
            command(fx, "c-9999", "set_config", "{}");
        }
        step(fx, chunk);
        controls(fx, muted, talk, attention);
    }
}

// Like expect, but skips the heartbeats and acknowledgements that pile up during a long hold.
static void expect_next(fx_t *fx, const char *name, const char *extra)
{
    char json[JSON_MAX];
    char want[96];
    snprintf(want, sizeof want, "\"name\":\"%s\"", name);
    while (next_tx(fx, json)) {
        if (has(json, "\"name\":\"heartbeat\"") || has(json, "\"name\":\"command_ok\"")) {
            continue;
        }
        if (!has(json, want) || (extra && !has(json, extra))) {
            fprintf(stderr, "expected %s %s, got:\n%s\n", name, extra ? extra : "", json);
            exit(1);
        }
        return;
    }
    fprintf(stderr, "expected %s %s, got nothing\n", name, extra ? extra : "");
    exit(1);
}

static void test_long_press_wipes_pairing(void)
{
    fx_t *fx = fx_init(TEST_KEY, 0);
    establish(fx, false, TEST_KEY);
    fx->key_stored = true;
    controls(fx, false, true, true);
    expect(fx, "button_event", "\"action\":\"press\"");
    hold(fx, 9999, false, true, true);
    assert(fx->session.paired && fx->erase_calls == 0);
    hold(fx, 1, false, true, true);
    expect_next(fx, "button_event", "\"action\":\"long_press\"");
    expect_next(fx, "pair_reset", NULL);
    assert(!fx->session.paired && !fx->session.authenticated && !fx->key_stored && fx->erase_calls == 1);
    assert(fx->session.pair_key[0] == 0 && fx->session.pair_key[31] == 0);
    // Holding on does not repeat it.
    hold(fx, 20000, false, true, true);
    drain(fx);
    assert(fx->erase_calls == 1);
    // The wiped robot accepts pairing again.
    controls(fx, false, false, false);
    drain(fx);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    assert(fx->session.state == MOBY_SESSION_PAIRING);

    // Disconnected, the wipe still happens but nothing is sent.
    fx = fx_init(TEST_KEY, 0);
    fx->key_stored = true;
    moby_session_tick(&fx->session, fx->now);
    controls(fx, false, true, true);
    hold(fx, 10000, false, true, true);
    assert(!fx->session.paired && !fx->key_stored && fx->session.outbox_count == 0);

    // A wipe that cannot erase keeps the robot paired and reports the fault.
    fx = fx_init(TEST_KEY, 0);
    establish(fx, false, TEST_KEY);
    fx->erase_ok = false;
    controls(fx, false, true, false);
    hold(fx, 10000, false, true, false);
    expect_next(fx, "button_event", "long_press");
    expect_next(fx, "error", "E_HW");
    assert(fx->session.paired);
}

static void test_long_press_during_pairing(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    controls(fx, false, true, true);
    expect(fx, "pair_button_confirmed", NULL);
    hold(fx, 10000, false, true, true);
    expect_next(fx, "button_event", "long_press");
    expect_next(fx, "pair_reset", NULL);
    assert(fx->session.state == MOBY_SESSION_IDLE && !fx->session.pair_confirmed);
}

// --- Authentication -----------------------------------------------------------

static void test_proof_accepted_with_protocol_vectors(void)
{
    // proof-vectors.tsv session_proof_1: key, nonce, device_id and session_id all fixed by the protocol.
    static const uint8_t key[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    fx_t *fx = fx_init(key, 0);
    snprintf(fx->identity.device_id, sizeof fx->identity.device_id, "dev_123e4567-e89b-42d3-a456-426614174000");
    fx->session.identity = fx->identity;
    fx->connected = true;
    step(fx, 0);
    // Replace the random nonce of the open attempt with the vector's.
    assert(moby_hex_decode("00112233445566778899aabbccddeeff", 32, fx->session.nonce, 16));
    char hello[JSON_MAX];
    assert(next_tx(fx, hello));
    char id[16];
    field_of(hello, "id", id, sizeof id);
    char json[JSON_MAX];
    snprintf(json, sizeof json,
             "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":11,\"data\":{\"accepted\":true,"
             "\"session_id\":\"session_1\",\"config\":{},\"proof\":\"adfb56ea9a47e8af7dbf9cd7b9fc515b6099e510cc6862c072d7f1e29c1c95be\","
             "\"desktop_nonce\":\"" TEST_DESKTOP_NONCE_HEX "\"}}",
             id);
    desktop(fx, json);
    step(fx, 1);
    assert(fx->session.established && fx->session.authenticated);
    // robot-proof-vectors.tsv robot_proof_1: the same key, both nonces, device and session give this proof.
    assert(next_tx(fx, json));
    assert(has(json, "\"name\":\"session_proof\""));
    assert(has(json, "\"proof\":\"b3bae9e90d0e2cee7808185e64fd557087fc93f6e5e0e0c866467c2d28b921f4\""));
}

static void test_proof_separator_vectors(void)
{
    // separator_ab_c and separator_a_bc differ only in where the device ID ends.
    static const uint8_t key[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    static const char *const proofs[2] = { "185994479a6bf3aa2a407b238b04be856d5549a734348e297ae6922e0875566f",
                                           "f8f34535a6bb97dbb355eebea575b1a9ec2547e75163a8fab7b1d3187aca8bed" };
    for (int attempt = 0; attempt < 2; ++attempt) {
        fx_t *fx = fx_init(key, 0);
        snprintf(fx->identity.device_id, sizeof fx->identity.device_id, "ab");
        fx->session.identity = fx->identity;
        fx->connected = true;
        step(fx, 0);
        assert(moby_hex_decode("a1b2c3d4e5f60718293a4b5c6d7e8f90", 32, fx->session.nonce, 16));
        char hello[JSON_MAX];
        char id[16];
        char json[JSON_MAX];
        assert(next_tx(fx, hello));
        field_of(hello, "id", id, sizeof id);
        // Session "c" with the "ab"/"c" proof is right; the "a"/"bc" proof must not verify for it.
        snprintf(json, sizeof json,
                 "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
                 "\"session_id\":\"%s\",\"config\":{},\"proof\":\"%s\",\"desktop_nonce\":\"" TEST_DESKTOP_NONCE_HEX "\"}}",
                 id, "c", proofs[attempt]);
        desktop(fx, json);
        step(fx, 1);
        assert(fx->session.established && fx->session.authenticated == (attempt == 0));
    }
}

static void test_bad_and_missing_proof(void)
{
    for (int variant = 0; variant < 4; ++variant) {
        fx_t *fx = fx_init(TEST_KEY, 0);
        char hello[JSON_MAX];
        fx->connected = true;
        step(fx, 0);
        controls(fx, false, false, false);
        assert(next_tx(fx, hello));
        uint8_t wrong[32];
        memcpy(wrong, TEST_KEY, sizeof wrong);
        wrong[5] ^= 0x40;
        fx->hmac_calls = 0;
        if (variant == 0) {
            ack(fx, hello, "session_1", wrong, true);
        } else if (variant == 1) {
            ack(fx, hello, "session_1", NULL, true);
        } else if (variant == 2) {
            // A proof made for another session ID.
            char id[16];
            field_of(hello, "id", id, sizeof id);
            // A proof computed for "session_2", sent with "session_1".
            char nonce_hex[40], device[64];
            field_of(hello, "nonce", nonce_hex, sizeof nonce_hex);
            field_of(hello, "device_id", device, sizeof device);
            uint8_t nonce[16], input[256], mac[32];
            size_t length = 0;
            assert(moby_hex_decode(nonce_hex, 32, nonce, 16));
            assert(moby_session_proof_input(nonce, device, "session_2", input, sizeof input, &length) == MOBY_OK);
            assert(moby_hmac_sha256(TEST_KEY, 32, input, length, mac));
            char proof[80];
            moby_hex_encode(mac, 32, proof);
            proof[64] = '\0';
            char json[JSON_MAX];
            snprintf(json, sizeof json,
                     "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
                     "\"session_id\":\"session_1\",\"config\":{},\"proof\":\"%s\",\"desktop_nonce\":\"" TEST_DESKTOP_NONCE_HEX "\"}}", id, proof);
            desktop(fx, json);
        } else {
            // A proof recorded on an earlier connection: valid key, valid session, stale nonce.
            ack(fx, hello, "session_1", TEST_KEY, true);
            fx->rx_length = fx->rx_position = 0;
            fx->desktop_sequence = 0;
            fx->connected = false;
            step(fx, 10);
            fx->connected = true;
            step(fx, 10);
            drain(fx);
            fx->tx_position = fx->tx_length = 0;
            char fresh[JSON_MAX];
            step(fx, 1000);
            assert(next_tx(fx, fresh));
            char old_nonce[40], new_nonce[40];
            field_of(hello, "nonce", old_nonce, sizeof old_nonce);
            field_of(fresh, "nonce", new_nonce, sizeof new_nonce);
            assert(strcmp(old_nonce, new_nonce) != 0);
            fx->hmac_calls = 0;
            // The recorded ack answers the old hello id, so the new attempt ignores it outright.
            ack(fx, hello, "session_1", TEST_KEY, true);
            step(fx, 10);
            assert(!fx->session.established);
            // And an ack with the new id but the old nonce's proof fails verification.
            char id[16];
            field_of(fresh, "id", id, sizeof id);
            uint8_t nonce[16], input[256], mac[32];
            size_t length = 0;
            assert(moby_hex_decode(old_nonce, 32, nonce, 16));
            assert(moby_session_proof_input(nonce, fx->identity.device_id, "session_1", input, sizeof input, &length) == MOBY_OK);
            assert(moby_hmac_sha256(TEST_KEY, 32, input, length, mac));
            char proof[80];
            moby_hex_encode(mac, 32, proof);
            proof[64] = '\0';
            char json[JSON_MAX];
            snprintf(json, sizeof json,
                     "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
                     "\"session_id\":\"session_1\",\"config\":{},\"proof\":\"%s\",\"desktop_nonce\":\"" TEST_DESKTOP_NONCE_HEX "\"}}", id, proof);
            desktop(fx, json);
        }
        step(fx, 10);
        assert(fx->session.established && !fx->session.authenticated);
        // A bad or missing desktop proof gets nothing back: the robot computes no proof of its own (the one HMAC
        // is the check of the desktop's proof; a missing proof needs none) and sends no session_proof.
        assert(fx->hmac_calls == (variant == 1 ? 0U : 1U));
        char json[JSON_MAX];
        bool saw_error = false;
        while (next_tx(fx, json)) {
            saw_error = saw_error || (has(json, "\"name\":\"error\"") && has(json, "E_STATE"));
            assert(!has(json, "session_proof"));
        }
        assert(saw_error);
        assert(view_of(fx)->expression == MOBY_FACE_ERROR);
        // The unauthenticated session of a paired robot obeys no command.
        command(fx, "c-0001", "display_text", "{\"text\":\"hi\",\"ttl_ms\":1000}");
        command(fx, "c-0002", "expression", "{\"state\":\"thinking\",\"animation\":\"none\",\"duration_ms\":0}");
        command(fx, "c-0003", "pair_begin", "{\"timeout_ms\":30000}");
        command(fx, "c-0004", "set_config", "{\"speaker_volume\":1}");
        command(fx, "c-0005", "audio_play_start", "{\"stream_id\":2,\"codec\":\"pcm_s16le\",\"sample_rate\":16000}");
        step(fx, 0);
        expect_error(fx, "c-0001", "E_STATE");
        expect_error(fx, "c-0002", "E_STATE");
        expect_error(fx, "c-0003", "E_STATE");
        expect_error(fx, "c-0004", "E_STATE");
        expect_error(fx, "c-0005", "E_UNSUPPORTED");
        assert(fx->session.config.speaker_volume == 50 && fx->session.text[0] == '\0');
        // Later in the same session the robot still has nothing to prove with.
        step(fx, 6000);
        while (next_tx(fx, json)) {
            assert(!has(json, "session_proof"));
        }
        assert(fx->hmac_calls == (variant == 1 ? 0U : 1U));
    }
}

static void test_ack_config_needs_a_proven_session(void)
{
    static const char settings[] = "{\"wake_sensitivity\":7,\"speaker_volume\":8,\"vad_silence_ms\":300,\"max_utterance_ms\":2000}";
    uint8_t wrong[32];
    memcpy(wrong, TEST_KEY, sizeof wrong);
    wrong[0] ^= 1;
    // A paired robot: a wrong proof and a missing proof change nothing, a correct proof applies the settings.
    const uint8_t *proofs[3] = { wrong, NULL, TEST_KEY };
    for (int variant = 0; variant < 3; ++variant) {
        fx_t *fx = fx_init(TEST_KEY, 0);
        char hello[JSON_MAX];
        fx->connected = true;
        step(fx, 0);
        assert(next_tx(fx, hello));
        ack_config = settings;
        ack(fx, hello, "session_1", proofs[variant], true);
        ack_config = "{}";
        step(fx, 10);
        assert(fx->session.established && fx->session.authenticated == (variant == 2));
        const bool applied = fx->session.config.wake_sensitivity == 7 && fx->session.config.speaker_volume == 8 &&
                             fx->session.config.vad_silence_ms == 300 && fx->session.config.max_utterance_ms == 2000;
        assert(applied == (variant == 2));
        if (variant != 2) {
            assert(fx->session.config.wake_sensitivity == 50 && fx->session.config.speaker_volume == 50 &&
                   fx->session.config.vad_silence_ms == 800 && fx->session.config.max_utterance_ms == 15000);
        }
    }
    // An unpaired robot has no key to prove, and its open session takes the ack's settings.
    fx_t *fx = fx_init(NULL, 0);
    char hello[JSON_MAX];
    fx->connected = true;
    step(fx, 0);
    assert(next_tx(fx, hello));
    ack_config = settings;
    ack(fx, hello, "session_1", NULL, true);
    ack_config = "{}";
    step(fx, 10);
    assert(fx->session.established && fx->session.config.speaker_volume == 8);
}

static void test_proof_ignored_when_unpaired(void)
{
    fx_t *fx = fx_init(NULL, 0);
    fx->connected = true;
    step(fx, 0);
    char hello[JSON_MAX];
    assert(next_tx(fx, hello));
    ack(fx, hello, "session_1", TEST_KEY, true);
    step(fx, 10);
    assert(fx->session.established && !fx->session.authenticated && !fx->session.paired);
    // An unpaired robot has no key: it computes nothing and sends no session_proof, whatever the ack carries.
    assert(fx->hmac_calls == 0);
    char sent[JSON_MAX];
    while (next_tx(fx, sent)) {
        assert(!has(sent, "session_proof"));
    }
    // The open session of an unpaired robot still takes harmless commands.
    drain(fx);
    controls(fx, false, false, false);
    drain(fx);
    command(fx, "c-0001", "display_text", "{\"text\":\"hi\",\"ttl_ms\":1000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
}

// --- The robot's proof of the key (session_proof) ---------------------------------

// HMAC-SHA256(key, "moby-robot-proof-v1" || 0 || desktop_nonce || robot_nonce || device_id || 0 || session_id),
// built with the vendored input builder and the mbedTLS code the firmware links, as lowercase hex.
static void robot_proof_hex(const uint8_t *key, const char *desktop_nonce_hex, const char *robot_nonce_hex,
                            const char *device, const char *session, char out[65])
{
    uint8_t desktop_nonce[MOBY_USB_NONCE_BYTES];
    uint8_t robot_nonce[MOBY_USB_NONCE_BYTES];
    uint8_t input[256];
    uint8_t mac[32];
    size_t length = 0;
    assert(moby_hex_decode(desktop_nonce_hex, 32, desktop_nonce, sizeof desktop_nonce));
    assert(moby_hex_decode(robot_nonce_hex, 32, robot_nonce, sizeof robot_nonce));
    assert(moby_robot_proof_input(desktop_nonce, robot_nonce, device, session, input, sizeof input, &length) == MOBY_OK);
    assert(moby_hmac_sha256(key, MOBY_USB_PAIR_KEY_BYTES, input, length, mac));
    moby_hex_encode(mac, sizeof mac, out);
    out[64] = '\0';
}

// The desktop's own proof over (hello nonce, device_id, session_id), as lowercase hex.
static void desktop_proof_hex(const uint8_t *key, const char *nonce_hex, const char *device, const char *session,
                              char out[65])
{
    uint8_t nonce[MOBY_USB_NONCE_BYTES];
    uint8_t input[256];
    uint8_t mac[32];
    size_t length = 0;
    assert(moby_hex_decode(nonce_hex, 32, nonce, sizeof nonce));
    assert(moby_session_proof_input(nonce, device, session, input, sizeof input, &length) == MOBY_OK);
    assert(moby_hmac_sha256(key, MOBY_USB_PAIR_KEY_BYTES, input, length, mac));
    moby_hex_encode(mac, sizeof mac, out);
    out[64] = '\0';
}

static void test_robot_proof_follows_a_good_proof(void)
{
    fx_t *fx = fx_init(TEST_KEY, 0);
    char hello[JSON_MAX];
    fx->connected = true;
    step(fx, 0);
    controls(fx, false, false, false);
    assert(next_tx(fx, hello));
    char nonce_hex[40];
    char device[MOBY_ID_MAX + 1];
    field_of(hello, "nonce", nonce_hex, sizeof nonce_hex);
    field_of(hello, "device_id", device, sizeof device);
    fx->hmac_calls = 0;
    ack(fx, hello, "session_1", TEST_KEY, true);
    step(fx, 10);
    assert(fx->session.established && fx->session.authenticated);
    // One HMAC to check the desktop's proof, one to make the robot's.
    assert(fx->hmac_calls == 2);

    // session_proof is the first message after the ack, before the opening mute_changed.
    char json[JSON_MAX];
    assert(next_tx(fx, json));
    assert(has(json, "\"kind\":\"event\",\"name\":\"session_proof\""));
    char got[65];
    char want[65];
    field_of(json, "proof", got, sizeof got);
    robot_proof_hex(TEST_KEY, TEST_DESKTOP_NONCE_HEX, nonce_hex, device, "session_1", want);
    assert(strcmp(got, want) == 0);
    // The desktop proof over the same session is a different value: neither can be shown as the other.
    char desktop_proof[65];
    desktop_proof_hex(TEST_KEY, nonce_hex, device, "session_1", desktop_proof);
    assert(strcmp(got, desktop_proof) != 0);
    expect(fx, "mute_changed", "\"source\":\"boot\"");

    // It is sent once. The nonce of the hello is wiped, and no log line carries a nonce or a proof.
    step(fx, 2000);
    while (next_tx(fx, json)) {
        assert(!has(json, "session_proof"));
    }
    for (size_t i = 0; i < sizeof fx->session.nonce; ++i) {
        assert(fx->session.nonce[i] == 0);
    }
    assert(strstr(fx->log, nonce_hex) == NULL && strstr(fx->log, got) == NULL &&
           strstr(fx->log, TEST_DESKTOP_NONCE_HEX) == NULL && strstr(fx->log, TEST_KEY_HEX) == NULL);
    // Nothing the robot sent on the wire after the first message repeats the proof or the key.
    for (size_t i = 0; i + 8 <= fx->tx_length; ++i) {
        assert(memcmp(fx->tx + i, TEST_KEY, 8) != 0);
    }
}

// The proof goes out only after the desktop's proof, and each new session gets its own, bound to the new nonces.
static void test_robot_proof_is_fresh_for_every_session(void)
{
    fx_t *fx = fx_init(TEST_KEY, 0);
    char seen[2][65];
    for (int session = 0; session < 2; ++session) {
        char hello[JSON_MAX];
        char json[JSON_MAX];
        fx->connected = true;
        step(fx, session == 0 ? 0 : 1000);
        controls(fx, false, false, false);
        bool found = false;
        while (next_tx(fx, json)) {
            if (has(json, "\"name\":\"hello\"")) {
                strcpy(hello, json);
                found = true;
            }
        }
        assert(found);
        ack(fx, hello, "session_1", TEST_KEY, true);
        step(fx, 10);
        assert(fx->session.authenticated);
        assert(next_tx(fx, json) && has(json, "session_proof"));
        field_of(json, "proof", seen[session], sizeof seen[session]);
        // End the session: the link drops.
        fx->connected = false;
        step(fx, 10);
        drain(fx);
        fx->tx_position = fx->tx_length = 0;
        fx->tx_decoder.length = 0;
        assert(!fx->session.established);
    }
    // Same key, device and session ID, but another hello nonce: another proof.
    assert(strcmp(seen[0], seen[1]) != 0);
}

// The ack's two fields come together. A proof without the nonce, a nonce without the proof, a null, or a bad
// nonce is not an ack the robot admits, so it computes nothing and sends nothing.
static void test_ack_needs_proof_and_nonce_together(void)
{
    for (int variant = 0; variant < 7; ++variant) {
        fx_t *fx = fx_init(TEST_KEY, 0);
        char hello[JSON_MAX];
        fx->connected = true;
        step(fx, 0);
        assert(next_tx(fx, hello));
        char id[16];
        char nonce_hex[40];
        char device[MOBY_ID_MAX + 1];
        char proof[65];
        field_of(hello, "id", id, sizeof id);
        field_of(hello, "nonce", nonce_hex, sizeof nonce_hex);
        field_of(hello, "device_id", device, sizeof device);
        desktop_proof_hex(TEST_KEY, nonce_hex, device, "session_1", proof);
        char fields[200];
        const char *forms[7] = {
            ",\"proof\":\"%s\"",
            ",\"desktop_nonce\":\"" TEST_DESKTOP_NONCE_HEX "\"",
            ",\"proof\":null,\"desktop_nonce\":\"" TEST_DESKTOP_NONCE_HEX "\"",
            ",\"proof\":\"%s\",\"desktop_nonce\":null",
            ",\"proof\":\"%s\",\"desktop_nonce\":\"0f1e2d3c4b5a69788796a5b4c3d2e1\"",
            ",\"proof\":\"%s\",\"desktop_nonce\":\"0F1E2D3C4B5A69788796A5B4C3D2E1F0\"",
            ",\"proof\":\"%s\",\"desktop_nonce\":\"0f1e2d3c4b5a69788796a5b4c3d2e1f0\",\"extra\":1",
        };
        snprintf(fields, sizeof fields, forms[variant], proof);
        char json[JSON_MAX];
        snprintf(json, sizeof json,
                 "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":1,\"data\":{\"accepted\":true,"
                 "\"session_id\":\"session_1\",\"config\":{}%s}}",
                 id, fields);
        fx->hmac_calls = 0;
        desktop(fx, json);
        step(fx, 10);
        assert(!fx->session.established && !fx->session.authenticated);
        assert(fx->hmac_calls == 0);
        while (next_tx(fx, json)) {
            assert(!has(json, "session_proof"));
        }
        // Nothing is stuck: a proper ack of the same hello still admits the session.
        ack(fx, hello, "session_1", TEST_KEY, true);
        step(fx, 10);
        assert(fx->session.established && fx->session.authenticated);
    }
}

// A session that pair_set_key authenticated has no desktop_nonce, so the robot has nothing to prove with:
// it sends no session_proof there, and the desktop starts a new hello exchange (docs/protocol.md, step 8).
static void test_pairing_session_has_no_robot_proof(void)
{
    fx_t *fx = fx_init(NULL, 0);
    char hello[JSON_MAX];
    fx->connected = true;
    step(fx, 0);
    controls(fx, false, false, false);
    assert(next_tx(fx, hello));
    ack(fx, hello, "session_1", NULL, true);
    step(fx, 10);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    press(fx);
    step(fx, 0);
    command(fx, "c-0002", "pair_set_key", "{\"pair_key\":\"" "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f" "\"}");
    step(fx, 0);
    assert(fx->session.paired && fx->session.authenticated);
    assert(fx->hmac_calls == 0);
    char json[JSON_MAX];
    while (next_tx(fx, json)) {
        assert(!has(json, "session_proof"));
    }
    // The next session, with a proof and a nonce from the desktop, is proven.
    fx->connected = false;
    step(fx, 10);
    fx->connected = true;
    step(fx, 1000);
    bool found = false;
    while (next_tx(fx, json)) {
        if (has(json, "\"name\":\"hello\"")) {
            strcpy(hello, json);
            found = true;
        }
    }
    assert(found && has(hello, "\"paired\":true"));
    ack(fx, hello, "session_2", TEST_KEY, true);
    step(fx, 10);
    assert(fx->session.authenticated);
    assert(next_tx(fx, json) && has(json, "session_proof"));
}

// --- Mute, buttons and the face -------------------------------------------------

typedef void (*setup_fn)(fx_t *);

static void setup_boot(fx_t *fx) { (void)fx; }

static void setup_handshake(fx_t *fx)
{
    fx->connected = true;
    step(fx, 0);
    drain(fx);
}

static void setup_expression(fx_t *fx, const char *state)
{
    char data[96];
    establish(fx, false, NULL);
    snprintf(data, sizeof data, "{\"state\":\"%s\",\"animation\":\"none\",\"duration_ms\":0}", state);
    command(fx, "c-0001", "expression", data);
    step(fx, 0);
    expect_ok(fx, "c-0001");
}

static void setup_idle(fx_t *fx) { establish(fx, false, NULL); }
// The desktop cannot command listening any more (F4); the real capture case is in tests/f4.c. This forces the state
// so that the mute is still proven over it in an image without audio.
static void setup_listening(fx_t *fx)
{
    establish(fx, false, NULL);
    fx->session.face = MOBY_EXPRESSION_LISTENING;
    fx->session.state = MOBY_SESSION_LISTENING;
}
static void setup_thinking(fx_t *fx) { setup_expression(fx, "thinking"); }

static void setup_speaking(fx_t *fx)
{
    setup_expression(fx, "thinking");
    command(fx, "c-0002", "expression", "{\"state\":\"speaking\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_ok(fx, "c-0002");
}

static void setup_pairing(fx_t *fx)
{
    establish(fx, false, NULL);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
}

static void setup_text(fx_t *fx)
{
    establish(fx, false, NULL);
    command(fx, "c-0001", "display_text", "{\"text\":\"Salom\",\"ttl_ms\":60000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
}

static void setup_error(fx_t *fx)
{
    establish(fx, false, NULL);
    command(fx, "c-0001", "expression", "{\"state\":\"error\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
}

static void test_mute_wins_in_every_state(void)
{
    static const setup_fn setups[] = { setup_boot,     setup_handshake, setup_idle,  setup_listening, setup_thinking,
                                       setup_speaking, setup_pairing,   setup_text,  setup_error };
    for (size_t index = 0; index < sizeof setups / sizeof setups[0]; ++index) {
        fx_t *fx = fx_init(NULL, 0);
        setups[index](fx);
        const bool connected = fx->session.established;
        if (index != 0) {
            // Everything except the boot case starts unmuted.
            assert(!fx->session.muted || index == 1);
        }
        controls(fx, false, false, false);
        controls(fx, true, true, true);
        const moby_face_view_t muted = *view_of(fx);
        assert(muted.expression == MOBY_FACE_MUTED && muted.text[0] == '\0' && !muted.attention);
        assert(!moby_session_recording(&fx->session));
        if (connected) {
            // The desktop cannot change a muted robot, whatever it asks.
            drain(fx);
            static const char *const states[] = { "idle", "listening", "thinking", "speaking", "happy", "confused", "error", "muted", "disconnected" };
            for (size_t s = 0; s < sizeof states / sizeof states[0]; ++s) {
                char data[96];
                snprintf(data, sizeof data, "{\"state\":\"%s\",\"animation\":\"none\",\"duration_ms\":0}", states[s]);
                command(fx, "c-0010", "expression", data);
                step(fx, 0);
                expect_error(fx, "c-0010", "E_MUTED");
                assert(view_of(fx)->expression == MOBY_FACE_MUTED);
            }
            command(fx, "c-0011", "display_text", "{\"text\":\"hello\",\"ttl_ms\":1000}");
            command(fx, "c-0012", "pair_begin", "{\"timeout_ms\":30000}");
            step(fx, 0);
            expect_error(fx, "c-0011", "E_MUTED");
            expect_error(fx, "c-0012", "E_MUTED");
            assert(view_of(fx)->text[0] == '\0' && view_of(fx)->expression == MOBY_FACE_MUTED);
            step(fx, 2000);
            char json[JSON_MAX];
            bool heartbeat = false;
            while (next_tx(fx, json)) {
                if (has(json, "\"name\":\"heartbeat\"")) {
                    heartbeat = true;
                    assert(has(json, "\"state\":\"muted\",\"mute\":true"));
                }
            }
            assert(heartbeat);
            // Unmuting goes to idle, not back to whatever ran before.
            controls(fx, false, false, false);
            assert(view_of(fx)->expression == MOBY_FACE_IDLE && fx->session.state == MOBY_SESSION_IDLE);
        } else {
            assert(view_of(fx)->expression == MOBY_FACE_MUTED);
            controls(fx, false, false, false);
            assert(view_of(fx)->expression == MOBY_FACE_DISCONNECTED);
        }
    }
}

static void test_mute_beats_error_and_disconnect(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    fx->session.error_active = true;
    fx->session.error_until_ms = fx->now + 5000;
    controls(fx, true, false, false);
    assert(view_of(fx)->expression == MOBY_FACE_MUTED);
    // Losing the desktop while muted still shows muted.
    fx->connected = false;
    step(fx, 10);
    assert(view_of(fx)->expression == MOBY_FACE_MUTED);
    // And a hello during mute reports it.
    drain(fx);
    fx->connected = true;
    step(fx, 10);
    expect(fx, "hello", "\"mute\":true");
}

static void test_button_events(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, true, NULL);
    // A press that starts while muted is no press at all.
    controls(fx, true, true, false);
    step(fx, 0);
    expect_silence(fx);
    controls(fx, false, false, false);
    expect(fx, "mute_changed", "\"muted\":false,\"source\":\"switch\"");
    press(fx);
    expect(fx, "button_event", "\"action\":\"press\"");
    // Muting during a press ends it.
    controls(fx, true, true, false);
    expect(fx, "mute_changed", "\"muted\":true");
    expect(fx, "button_event", "\"action\":\"release\"");
    controls(fx, false, true, false);
    expect(fx, "mute_changed", "\"muted\":false");
    expect_silence(fx);
}

static void test_outbox_is_bounded(void)
{
    fx_t *fx = fx_init(NULL, 0);
    establish(fx, false, NULL);
    drain(fx);
    for (int i = 0; i < 40; ++i) {
        moby_msg_t msg;
        memset(&msg, 0, sizeof msg);
        msg.name = MOBY_MSG_SET_CONFIG;
        strcpy(msg.id, "c-0001");
        moby_session_on_message(&fx->session, fx->now, &msg);
    }
    assert(fx->session.outbox_count == MOBY_OUTBOX_LEN && fx->session.stats.outbox_dropped == 40 - MOBY_OUTBOX_LEN);
}

static void test_face_text_rendering(void)
{
    // The 95 glyphs of the font are all different from each other.
    for (int a = 0x20; a < 0x7f; ++a) {
        for (int b = a + 1; b < 0x7f; ++b) {
            assert(memcmp(moby_font_glyph((char)a), moby_font_glyph((char)b), MOBY_FONT_WIDTH) != 0);
        }
    }
    assert(memcmp(moby_font_glyph('\x01'), moby_font_glyph('?'), MOBY_FONT_WIDTH) == 0);
    assert(moby_font_glyph('A')[0] == 0x7e && moby_font_glyph(' ')[2] == 0);

    uint8_t plain[MOBY_FACE_BYTES];
    uint8_t texted[MOBY_FACE_BYTES];
    uint8_t guarded[MOBY_FACE_BYTES + 2];
    memset(guarded, 0xa5, sizeof guarded);
    moby_face_view_t view = { .expression = MOBY_FACE_IDLE };
    moby_face_render(plain, view, false);
    snprintf(view.text, sizeof view.text, "Salom, dunyo");
    moby_face_render(guarded + 1, view, false);
    assert(guarded[0] == 0xa5 && guarded[MOBY_FACE_BYTES + 1] == 0xa5);
    memcpy(texted, guarded + 1, MOBY_FACE_BYTES);
    assert(memcmp(plain, texted, MOBY_FACE_BYTES) != 0);
    // A longer text takes more rows and still stays inside the frame.
    memset(view.text, 'W', MOBY_FACE_TEXT_MAX);
    view.text[MOBY_FACE_TEXT_MAX] = '\0';
    moby_face_render(guarded + 1, view, false);
    assert(guarded[0] == 0xa5 && guarded[MOBY_FACE_BYTES + 1] == 0xa5);
    // Text never replaces the mute, disconnected or error faces.
    uint8_t reference[MOBY_FACE_BYTES];
    for (int expression = MOBY_FACE_ERROR; expression <= MOBY_FACE_DISCONNECTED; ++expression) {
        moby_face_view_t with_text = { .expression = (moby_expression_t)expression };
        snprintf(with_text.text, sizeof with_text.text, "hello");
        moby_face_view_t without = { .expression = (moby_expression_t)expression };
        moby_face_render(texted, with_text, false);
        moby_face_render(reference, without, false);
        assert(memcmp(texted, reference, MOBY_FACE_BYTES) == 0);
    }
    moby_face_view_t other = { .expression = MOBY_FACE_IDLE };
    snprintf(other.text, sizeof other.text, "hello");
    moby_face_view_t same = other;
    assert(moby_face_view_equal(&other, &same));
    same.text[0] = 'j';
    assert(!moby_face_view_equal(&other, &same));
}

// --- Identity and storage -------------------------------------------------------

typedef struct {
    char id[64];
    bool has_id;
    uint8_t key[64];
    size_t key_size;
    bool has_key;
    bool fail;
    unsigned random_calls;
} mem_store_t;

static moby_store_status_t mem_read(void *ctx, const char *name, void *buffer, size_t *size)
{
    mem_store_t *store = ctx;
    if (store->fail) {
        return MOBY_STORE_FAIL;
    }
    if (strcmp(name, "device_id") == 0) {
        if (!store->has_id) {
            return MOBY_STORE_MISSING;
        }
        *size = strlen(store->id) < *size ? strlen(store->id) : *size;
        memcpy(buffer, store->id, *size);
        return MOBY_STORE_OK;
    }
    if (!store->has_key) {
        return MOBY_STORE_MISSING;
    }
    assert(store->key_size <= *size + 40);
    *size = store->key_size < *size ? store->key_size : *size;
    memcpy(buffer, store->key, *size);
    return MOBY_STORE_OK;
}

static bool mem_write(void *ctx, const char *name, const void *data, size_t size)
{
    mem_store_t *store = ctx;
    if (store->fail) {
        return false;
    }
    if (strcmp(name, "device_id") == 0) {
        memcpy(store->id, data, size);
        store->id[size] = '\0';
        store->has_id = true;
    } else {
        memcpy(store->key, data, size);
        store->key_size = size;
        store->has_key = true;
    }
    return true;
}

static bool mem_erase(void *ctx, const char *name)
{
    mem_store_t *store = ctx;
    if (store->fail) {
        return false;
    }
    if (strcmp(name, "device_id") == 0) {
        store->has_id = false;
    } else {
        store->has_key = false;
    }
    return true;
}

static bool mem_random(void *ctx, uint8_t *out, size_t length)
{
    mem_store_t *store = ctx;
    ++store->random_calls;
    for (size_t i = 0; i < length; ++i) {
        out[i] = (uint8_t)(0xa0U + i + store->random_calls);
    }
    return true;
}

static bool no_random(void *ctx, uint8_t *out, size_t length)
{
    (void)ctx;
    (void)out;
    (void)length;
    return false;
}

static void test_config_identity_and_key(void)
{
    mem_store_t memory;
    memset(&memory, 0, sizeof memory);
    const moby_store_t store = { &memory, mem_read, mem_write, mem_erase };
    moby_stored_t stored;

    // First boot: a random 128-bit id, stored, and no key.
    assert(moby_config_load(&stored, &store, mem_random, &memory) == MOBY_CONFIG_OK);
    assert(strlen(stored.device_id) == MOBY_DEVICE_ID_LEN && strncmp(stored.device_id, "dev_", 4) == 0);
    assert(moby_stable_id_valid(stored.device_id) && !stored.paired && memory.has_id && memory.random_calls == 1);
    assert(strcmp(stored.device_id, "dev_a1a2a3a4a5a6a7a8a9aaabacadaeafb0") == 0);
    // Second boot: the same id, with no new random bytes.
    moby_stored_t again;
    assert(moby_config_load(&again, &store, mem_random, &memory) == MOBY_CONFIG_OK);
    assert(strcmp(again.device_id, stored.device_id) == 0 && memory.random_calls == 1);

    // A stored key survives a reboot and wipes cleanly.
    assert(moby_config_store_key(&store, TEST_KEY));
    assert(moby_config_load(&again, &store, mem_random, &memory) == MOBY_CONFIG_OK);
    assert(again.paired && memcmp(again.pair_key, TEST_KEY, sizeof TEST_KEY) == 0);
    assert(moby_config_erase_key(&store));
    assert(moby_config_load(&again, &store, mem_random, &memory) == MOBY_CONFIG_OK && !again.paired);

    // A key of the wrong length is not a key: it is erased.
    memory.has_key = true;
    memory.key_size = 7;
    assert(moby_config_load(&again, &store, mem_random, &memory) == MOBY_CONFIG_OK);
    assert(!again.paired && !memory.has_key);

    // A damaged device id is replaced.
    strcpy(memory.id, "dev_nope");
    assert(moby_config_load(&again, &store, mem_random, &memory) == MOBY_CONFIG_OK);
    assert(strcmp(again.device_id, "dev_nope") != 0 && memory.random_calls == 2);

    // Without random bytes there is no identity, and a failing store is reported.
    memory.has_id = false;
    assert(moby_config_load(&again, &store, no_random, &memory) == MOBY_CONFIG_NO_RANDOM);
    memory.fail = true;
    assert(moby_config_load(&again, &store, mem_random, &memory) == MOBY_CONFIG_STORE_FAILED);
    assert(!moby_config_store_key(&store, TEST_KEY) && !moby_config_erase_key(&store));
}

// --- Privacy --------------------------------------------------------------------

static void test_secrets_never_logged_or_echoed(void)
{
    static const uint8_t secret[32] = { 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x44, 0x55,
                                        0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    char secret_hex[65];
    moby_hex_encode(secret, sizeof secret, secret_hex);
    secret_hex[64] = '\0';

    fx_t *fx = fx_init(NULL, 0);
    char hello[JSON_MAX];
    fx->connected = true;
    step(fx, 0);
    controls(fx, false, false, false);
    assert(next_tx(fx, hello));
    char nonce_hex[40];
    field_of(hello, "nonce", nonce_hex, sizeof nonce_hex);
    ack(fx, hello, "session_1", NULL, true);
    step(fx, 10);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":30000}");
    step(fx, 0);
    press(fx);
    step(fx, 0);
    char data[120];
    snprintf(data, sizeof data, "{\"pair_key\":\"%s\"}", secret_hex);
    command(fx, "c-0002", "pair_set_key", data);
    step(fx, 0);
    // The same key again on a paired robot, a bad key, and a wipe.
    command(fx, "c-0003", "pair_set_key", data);
    step(fx, 0);
    step(fx, 12000);
    controls(fx, false, true, true);
    step(fx, 10000);
    controls(fx, false, true, true);
    assert(!fx->session.paired);

    // Connect again as a paired robot and fail the proof, so every path has run.
    fx = fx_init(secret, 0);
    fx->connected = true;
    step(fx, 0);
    assert(next_tx(fx, hello));
    field_of(hello, "nonce", nonce_hex, sizeof nonce_hex);
    ack(fx, hello, "session_1", TEST_KEY, true);
    step(fx, 10);

    assert(strstr(fx->log, secret_hex) == NULL && strstr(fx->log, nonce_hex) == NULL);
    // The key never goes back out on the wire, in any encoding the robot uses.
    for (size_t i = 0; i + 8 <= fx->tx_length; ++i) {
        assert(memcmp(fx->tx + i, secret, 8) != 0);
        assert(memcmp(fx->tx + i, secret_hex, 8) != 0);
    }
    // Hello carries the nonce, which is not a secret, but no other message may.
    char json[JSON_MAX];
    fx->tx_position = 0;
    fx->tx_decoder.length = 0;
    while (next_tx(fx, json)) {
        if (!has(json, "\"name\":\"hello\"")) {
            assert(!has(json, nonce_hex) && !has(json, secret_hex));
        }
    }
    assert(strstr(fx->log, "pair_key") == NULL && strstr(fx->log, "proof") == NULL);
}

static void test_default_identity_has_no_audio(void)
{
    fx_t *fx = fx_init(NULL, 0);
    fx->connected = true;
    step(fx, 0);
    char hello[JSON_MAX];
    assert(next_tx(fx, hello));
    assert(has(hello, "\"capabilities\":[]"));
    assert(!has(hello, "usb_pcm"));
    // No message the robot can send starts audio.
    establish(fx_init(NULL, 0), false, NULL);
    char json[JSON_MAX];
    fx = &fx_storage;
    step(fx, 20000);
    while (next_tx(fx, json)) {
        assert(!has(json, "audio_start") && !has(json, "audio_end") && !has(json, "wake_detected"));
    }
    fx->tx_position = fx->tx_length = 0;
    fx = fx_init(NULL, 0);
    fx->connected = true;
    step(fx, 0);
    controls(fx, false, false, false);
    press(fx);
    step(fx, 5000);
    while (next_tx(fx, json)) {
        assert(!has(json, "audio_start"));
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const struct {
        const char *name;
        void (*run)(void);
    } tests[] = {
        { "hello on connect and retry", test_hello_on_connect_and_retry },
        { "handshake ack paths", test_handshake_ack_paths },
        { "session flow and late ack", test_session_flow_and_late_ack },
        { "heartbeat content", test_heartbeat_content },
        { "missed heartbeats end the session", test_missed_heartbeats_end_session },
        { "desktop traffic keeps the session", test_desktop_traffic_keeps_the_session },
        { "reconnect", test_reconnect },
        { "boot state", test_boot_state },
        { "clock wraps", test_clock_wraps },
        { "frame errors and recovery", test_frame_errors_and_recovery },
        { "bad frame version", test_bad_frame_version },
        { "audio frames rejected", test_audio_frames_rejected },
        { "error rate limit", test_error_rate_limit },
        { "write failures are counted", test_write_failures_are_counted },
        { "sequence gaps", test_sequence_gaps },
        { "bad messages", test_bad_messages },
        { "hostile payloads", test_hostile_payloads },
        { "expression transitions", test_expression_transitions_exhaustive },
        { "expression details", test_expression_details },
        { "display text", test_display_text },
        { "set_config", test_set_config },
        { "unsupported commands and cancel", test_unsupported_and_cancel },
        { "pairing happy path", test_pairing_happy_path },
        { "pairing wrong order", test_pairing_wrong_order },
        { "already paired refuses pair_begin", test_already_paired_refuses_pair_begin },
        { "pairing timeout and cancel", test_pairing_timeout_and_cancel },
        { "pairing and mute", test_pairing_and_mute },
        { "pairing store failure", test_pairing_store_failure },
        { "long press wipes pairing", test_long_press_wipes_pairing },
        { "long press during pairing", test_long_press_during_pairing },
        { "proof accepted (protocol vector)", test_proof_accepted_with_protocol_vectors },
        { "proof separator vectors", test_proof_separator_vectors },
        { "bad and missing proof", test_bad_and_missing_proof },
        { "ack config needs a proven session", test_ack_config_needs_a_proven_session },
        { "proof ignored when unpaired", test_proof_ignored_when_unpaired },
        { "robot proof follows a good proof", test_robot_proof_follows_a_good_proof },
        { "robot proof is fresh for every session", test_robot_proof_is_fresh_for_every_session },
        { "ack needs proof and nonce together", test_ack_needs_proof_and_nonce_together },
        { "pairing session has no robot proof", test_pairing_session_has_no_robot_proof },
        { "mute wins in every state", test_mute_wins_in_every_state },
        { "mute beats error and disconnect", test_mute_beats_error_and_disconnect },
        { "button events", test_button_events },
        { "outbox is bounded", test_outbox_is_bounded },
        { "face text rendering", test_face_text_rendering },
        { "config identity and key", test_config_identity_and_key },
        { "secrets never logged or echoed", test_secrets_never_logged_or_echoed },
        { "default identity has no audio", test_default_identity_has_no_audio },
    };
    const size_t count = sizeof tests / sizeof tests[0];
    for (size_t i = 0; i < count; ++i) {
        printf("%s: ", tests[i].name);
        fflush(stdout);
        tests[i].run();
        puts("ok");
    }
    printf("%zu F3 host tests passed\n", count);
    return 0;
}
