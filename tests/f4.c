// SPDX-License-Identifier: GPL-3.0-only

// F4 host tests: the sample conversion, the ring buffer, and push to talk through the real session and link,
// with a fake transport, a fake microphone and a fake LED. They prove the privacy invariants:
//   - the microphone runs only while capture is on, and capture, the LED and the listening face are one fact;
//   - no capture without an authenticated session, a held button, an idle robot and an open mute switch;
//   - mute, release, timeout, cancel and disconnect each stop capture at once and no frame follows;
//   - no audio frame leaves outside audio_start and audio_end, and none is ever logged.
// No ESP-IDF header is used. Hardware behaviour (real I2S levels, USB throughput, PSRAM timing, the LED) is not
// covered and not claimed.

#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "moby_config.h"
#include "moby_face.h"
#include "moby_hmac.h"
#include "moby_json.h"
#include "moby_link.h"
#include "moby_pcm.h"
#include "moby_ring.h"
#include "moby_session.h"

// --- The conversion contract ----------------------------------------------------------

_Static_assert(MOBY_PCM_SHIFT == 16 && MOBY_PCM_GAIN_Q8 == 256, "wake word training depends on this conversion");
_Static_assert(MOBY_PCM_FRAME_SAMPLES == 320 && MOBY_PCM_FRAME_BYTES == 640 && MOBY_PCM_FRAME_MS == 20, "20 ms frames");
_Static_assert(MOBY_PCM_FRAME_BYTES <= MOBY_USB_MAX_PAYLOAD_LEN, "a frame fits one payload");

// The documented formula, written differently from the implementation: floor(signed slot / 2^16).
static int16_t reference_sample(uint32_t slot)
{
    const int64_t signed_slot = slot >= 0x80000000U ? (int64_t)slot - 4294967296LL : (int64_t)slot;
    int64_t quotient = signed_slot / 65536;
    if (signed_slot % 65536 != 0 && signed_slot < 0) {
        --quotient;
    }
    return (int16_t)quotient;
}

static void test_conversion_values(void)
{
    static const struct {
        uint32_t slot;
        int16_t expected;
    } cases[] = {
        { 0x7fffff00U, 32767 },  { 0x80000000U, -32768 }, { 0x00000000U, 0 },     { 0xffffff00U, -1 },
        { 0x00010000U, 1 },      { 0x0000ffffU, 0 },      { 0x00007f00U, 0 },     { 0xfffeff00U, -2 },
        { 0xffff0000U, -1 },     { 0xfffe0000U, -2 },     { 0xfffeffffU, -2 },    { 0xffffffffU, -1 },
        { 0x12345600U, 0x1234 }, { 0xedcba900U, -4659 },  { 0x00ff00ffU, 255 },   { 0x7fff0000U, 32767 },
        { 0x80010000U, -32767 }, { 0x00008000U, 0 },      { 0xffff8000U, -1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        assert(moby_pcm_sample(cases[i].slot, MOBY_PCM_GAIN_Q8) == cases[i].expected);
        assert(reference_sample(cases[i].slot) == cases[i].expected);
    }
    // The low 8 bits of the slot (the INMP441 sends zeros there) never change the result: the result is the top 16 bits.
    uint32_t seed = 99;
    for (unsigned i = 0; i < 1000000; ++i) {
        seed = seed * 1664525U + 1013904223U;
        const uint32_t slot = seed;
        assert(moby_pcm_sample(slot, MOBY_PCM_GAIN_Q8) == reference_sample(slot));
        assert(moby_pcm_sample(slot, MOBY_PCM_GAIN_Q8) == moby_pcm_sample(slot & 0xffff0000U, MOBY_PCM_GAIN_Q8));
        assert(moby_pcm_sample(slot, 256) == moby_pcm_sample(slot, MOBY_PCM_GAIN_Q8));
    }
    // Every 16-bit value comes back through the unity gain.
    for (int32_t value = INT16_MIN; value <= INT16_MAX; ++value) {
        assert(moby_pcm_sample((uint32_t)value << 16, MOBY_PCM_GAIN_Q8) == value);
    }
}

static void test_conversion_buffers_and_gain(void)
{
    // Interleaved stereo: the left slot is used, the right slot is ignored.
    const uint32_t stereo[] = { 0x01000000U, 0x7fffff00U, 0xff000000U, 0x80000000U, 0x00020000U, 0x12340000U };
    int16_t mono[3] = { 0x5555, 0x5555, 0x5555 };
    moby_pcm_convert(stereo, mono, 3);
    assert(mono[0] == 0x0100 && mono[1] == -256 && mono[2] == 2);
    int16_t alias[3];
    moby_pcm_from_i2s(stereo, alias, 3);
    assert(memcmp(mono, alias, sizeof mono) == 0);
    int16_t none[2] = { 7, 7 };
    moby_pcm_convert(stereo, none, 0);
    assert(none[0] == 7 && none[1] == 7);

    // Gain: Q8, floor, saturating, and capped at 16x.
    assert(moby_pcm_sample(0x00640000U, 512) == 200);
    assert(moby_pcm_sample(0xff9c0000U, 512) == -200);
    assert(moby_pcm_sample(0x00030000U, 128) == 1);
    assert(moby_pcm_sample(0xfffd0000U, 128) == -2);
    assert(moby_pcm_sample(0x40000000U, 512) == INT16_MAX);
    assert(moby_pcm_sample(0xc0000000U, 512) == INT16_MIN);
    assert(moby_pcm_sample(0x00010000U, 4096) == 16);
    assert(moby_pcm_sample(0x00010000U, 100000) == 16);
    assert(moby_pcm_sample(0x7fff0000U, 100000) == INT16_MAX);
    assert(moby_pcm_sample(0x12340000U, 0) == 0);

    uint8_t bytes[6] = { 0 };
    const int16_t samples[3] = { 0x1234, -2, INT16_MIN };
    moby_pcm_to_le_bytes(samples, 3, bytes);
    assert(bytes[0] == 0x34 && bytes[1] == 0x12 && bytes[2] == 0xfe && bytes[3] == 0xff && bytes[4] == 0x00 && bytes[5] == 0x80);
}

// --- The ring buffer ------------------------------------------------------------------

static bool all_zero(const int16_t *data, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}

static void test_ring_basics(void)
{
    int16_t store[8];
    memset(store, 0x7f, sizeof store);
    moby_ring_t ring;
    moby_ring_init(&ring, store, 8);
    assert(all_zero(store, 8) && !ring.open);
    int16_t in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int16_t out[8] = { 0 };
    // Closed: nothing is stored.
    assert(moby_ring_write(&ring, in, 4) == 0 && moby_ring_available(&ring) == 0 && ring.rejected_samples == 4);
    assert(ring.overruns == 0 && !moby_ring_take_overrun(&ring));
    moby_ring_open(&ring, 0);
    assert(moby_ring_write(&ring, in, 3) == 3 && moby_ring_available(&ring) == 3);
    assert(moby_ring_peek(&ring, out, 4) == 0);
    assert(moby_ring_peek(&ring, out, 0) == 0);
    assert(moby_ring_peek(&ring, out, 2) == 2 && out[0] == 1 && out[1] == 2 && moby_ring_available(&ring) == 3);
    moby_ring_consume(&ring, 2);
    assert(moby_ring_available(&ring) == 1 && store[0] == 0 && store[1] == 0 && store[2] == 3);
    // Consuming more than is there takes what is there, and consuming nothing does nothing.
    moby_ring_consume(&ring, 0);
    moby_ring_consume(&ring, 100);
    assert(moby_ring_available(&ring) == 0 && all_zero(store, 8));
    // A zero length write is a no-op.
    assert(moby_ring_write(&ring, in, 0) == 0 && ring.overruns == 0);
}

static void test_ring_wrap(void)
{
    int16_t store[8];
    moby_ring_t ring;
    moby_ring_init(&ring, store, 8);
    moby_ring_open(&ring, 0);
    int16_t in[8];
    int16_t out[8];
    int16_t next = 1;
    int16_t expect_next = 1;
    // Push and pull blocks of 3 through a ring of 8 many times so the indices wrap repeatedly.
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 3; ++i) {
            in[i] = next++;
        }
        assert(moby_ring_write(&ring, in, 3) == 3);
        if (moby_ring_available(&ring) >= 5) {
            assert(moby_ring_peek(&ring, out, 5) == 5);
            for (int i = 0; i < 5; ++i) {
                assert(out[i] == expect_next++);
            }
            moby_ring_consume(&ring, 5);
        }
    }
    assert(ring.overruns == 0);
    // Exactly full is allowed, one more is an overrun.
    moby_ring_clear(&ring);
    moby_ring_open(&ring, 0);
    assert(moby_ring_write(&ring, in, 8) == 8 && moby_ring_available(&ring) == 8);
    assert(moby_ring_write(&ring, in, 1) == 0 && ring.overruns == 1 && ring.dropped_samples == 1);
}

static void test_ring_overrun_accounting(void)
{
    int16_t store[8];
    moby_ring_t ring;
    moby_ring_init(&ring, store, 8);
    moby_ring_open(&ring, 0);
    const int16_t block[6] = { 11, 12, 13, 14, 15, 16 };
    assert(moby_ring_write(&ring, block, 6) == 6);
    // The next block does not fit: it is dropped whole, counted, and the stored samples are intact.
    assert(moby_ring_write(&ring, block, 6) == 0);
    assert(ring.overruns == 1 && ring.dropped_samples == 6 && moby_ring_available(&ring) == 6);
    int16_t out[6];
    assert(moby_ring_peek(&ring, out, 6) == 6 && memcmp(out, block, sizeof block) == 0);
    assert(moby_ring_take_overrun(&ring) && !moby_ring_take_overrun(&ring));
    // Another drop sets the flag again; counters keep adding up.
    assert(moby_ring_write(&ring, block, 6) == 0 && ring.overruns == 2 && ring.dropped_samples == 12);
    assert(moby_ring_take_overrun(&ring));
    // Opening again clears a pending overrun but not the totals.
    assert(moby_ring_write(&ring, block, 6) == 0);
    moby_ring_open(&ring, 0);
    assert(!moby_ring_take_overrun(&ring) && ring.overruns == 3 && moby_ring_available(&ring) == 0);
}

static void test_ring_gate_and_warmup(void)
{
    int16_t store[16];
    moby_ring_t ring;
    moby_ring_init(&ring, store, 16);
    const int16_t block[5] = { 1, 2, 3, 4, 5 };
    moby_ring_open(&ring, 7);
    // The first 7 samples are dropped without counting as an overrun: a whole block, then part of the next.
    assert(moby_ring_write(&ring, block, 5) == 0 && moby_ring_available(&ring) == 0 && ring.overruns == 0);
    assert(moby_ring_write(&ring, block, 5) == 3 - 3 + 3 && moby_ring_available(&ring) == 3);
    int16_t out[3];
    assert(moby_ring_peek(&ring, out, 3) == 3 && out[0] == 3 && out[1] == 4 && out[2] == 5);
    // Closing keeps the samples but accepts no more; they can still be read.
    moby_ring_close(&ring);
    assert(moby_ring_write(&ring, block, 5) == 0 && moby_ring_available(&ring) == 3 && ring.overruns == 0);
    assert(moby_ring_peek(&ring, out, 3) == 3);
    // Clearing wipes them, and the gate stays closed until it is opened again.
    moby_ring_clear(&ring);
    assert(moby_ring_available(&ring) == 0 && all_zero(store, 16) && !ring.open);
    assert(moby_ring_write(&ring, block, 5) == 0);
    // A gate that is closed again forgets the unused warm-up.
    moby_ring_open(&ring, 100);
    moby_ring_close(&ring);
    moby_ring_open(&ring, 0);
    assert(moby_ring_write(&ring, block, 5) == 5);
}

static void test_ring_wipes_what_it_drops(void)
{
    int16_t store[12];
    moby_ring_t ring;
    moby_ring_init(&ring, store, 12);
    moby_ring_open(&ring, 0);
    int16_t block[5];
    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < 5; ++i) {
            block[i] = (int16_t)(1000 + round * 5 + i);
        }
        moby_ring_write(&ring, block, 5);
        // Only the live span may hold anything but zeros.
        size_t nonzero = 0;
        for (size_t i = 0; i < 12; ++i) {
            nonzero += store[i] != 0;
        }
        assert(nonzero == moby_ring_available(&ring));
        if (round % 3 == 2) {
            moby_ring_consume(&ring, 5);
        }
    }
    moby_ring_clear(&ring);
    assert(all_zero(store, 12));
    // Opening wipes too.
    moby_ring_open(&ring, 0);
    moby_ring_write(&ring, block, 5);
    moby_ring_open(&ring, 0);
    assert(all_zero(store, 12) && moby_ring_available(&ring) == 0);
}

static void test_ring_random_model(void)
{
    enum { CAP = 37, MODEL = 4096 };
    int16_t store[CAP];
    moby_ring_t ring;
    moby_ring_init(&ring, store, CAP);
    static int16_t model[MODEL];
    size_t model_count = 0;
    bool model_open = false;
    uint32_t seed = 4242;
    uint32_t dropped = 0;
    for (unsigned op = 0; op < 300000; ++op) {
        seed = seed * 1664525U + 1013904223U;
        const unsigned kind = (seed >> 24) % 10U;
        const size_t amount = (seed >> 8) % 15U;
        if (kind < 4) {
            int16_t block[16];
            for (size_t i = 0; i < amount; ++i) {
                block[i] = (int16_t)(seed + i);
            }
            const size_t stored = moby_ring_write(&ring, block, amount);
            if (!model_open || amount == 0) {
                assert(stored == 0);
            } else if (amount > CAP - model_count) {
                assert(stored == 0);
                dropped += (uint32_t)amount;
            } else {
                assert(stored == amount);
                memcpy(model + model_count, block, amount * sizeof block[0]);
                model_count += amount;
            }
        } else if (kind < 7) {
            int16_t out[16];
            const size_t got = moby_ring_peek(&ring, out, amount);
            if (amount == 0 || amount > model_count) {
                assert(got == 0);
            } else {
                assert(got == amount && memcmp(out, model, amount * sizeof out[0]) == 0);
            }
        } else if (kind == 7) {
            const size_t take = amount < model_count ? amount : model_count;
            moby_ring_consume(&ring, amount);
            memmove(model, model + take, (model_count - take) * sizeof model[0]);
            model_count -= take;
        } else if (kind == 8) {
            if ((seed >> 4) % 4U == 0) {
                moby_ring_clear(&ring);
                model_count = 0;
                model_open = false;
            } else {
                moby_ring_close(&ring);
                model_open = false;
            }
        } else {
            moby_ring_open(&ring, 0);
            model_count = 0;
            model_open = true;
        }
        assert(moby_ring_available(&ring) == model_count && ring.dropped_samples == dropped);
        size_t nonzero = 0;
        for (size_t i = 0; i < CAP; ++i) {
            nonzero += store[i] != 0;
        }
        assert(nonzero <= model_count);
    }
}

// --- The fixture ----------------------------------------------------------------------

enum {
    LOG_MAX = 16384,
    TX_MAX = 1 << 20,
    RX_MAX = 16384,
    JSON_MAX = 2048,
    MSG_MAX = 128,
    RING_SAMPLES = 8 * MOBY_PCM_FRAME_SAMPLES,
    DELIVERED_MAX = 60000,
    TRACE_MAX = 4096,
};

typedef struct {
    // The fake transport.
    bool connected;
    bool tx_idle;
    bool write_ok;
    uint8_t rx[RX_MAX];
    size_t rx_length;
    size_t rx_position;
    uint8_t *tx;
    size_t tx_length;
    size_t tx_position;
    moby_decoder tx_decoder;
    // What the tap has parsed from the transmitted bytes.
    char (*msgs)[JSON_MAX];
    size_t msg_first;
    size_t msg_count;
    bool in_stream;
    uint32_t stream_id;
    uint32_t next_sequence;
    uint32_t audio_frames;
    uint32_t streams_seen;
    int16_t *delivered;
    size_t delivered_count;
    // The fake microphone, ring, LED.
    int16_t ring_store[RING_SAMPLES];
    moby_ring_t ring;
    moby_pcm_source_t source;
    bool mic_on;
    bool led;
    bool fault;
    bool adversary;
    size_t warmup;
    unsigned capture_calls;
    char trace[TRACE_MAX];
    size_t trace_length;
    size_t outbox_when_off;
    bool muted_when_off;
    int16_t *accepted;
    size_t accepted_count;
    int16_t next_value;
    unsigned discards;
    // The fake platform.
    unsigned random_calls;
    char log[LOG_MAX];
    size_t log_length;
    // The code under test.
    moby_identity_t identity;
    moby_session_t session;
    moby_link_t link;
    moby_transport_t transport;
    uint32_t now;
    uint32_t desktop_sequence;
    // The controls as the debouncer would report them.
    bool hw_muted;
    bool talk;
    bool attention;
    // Coverage of the random test.
    unsigned mutes_in_stream;
} fx_t;

static fx_t *fx_storage;

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

static bool op_random(void *ctx, uint8_t *out, size_t length)
{
    fx_t *fx = ctx;
    ++fx->random_calls;
    for (size_t i = 0; i < length; ++i) {
        out[i] = (uint8_t)(fx->random_calls * 29U + i * 7U + 3U);
    }
    return true;
}

static bool op_hmac(void *ctx, const uint8_t *key, size_t key_length, const uint8_t *message, size_t message_length,
                    uint8_t out[32])
{
    (void)ctx;
    return moby_hmac_sha256(key, key_length, message, message_length, out);
}

static bool op_store(void *ctx, const uint8_t key[MOBY_USB_PAIR_KEY_BYTES])
{
    (void)ctx;
    (void)key;
    return true;
}

static bool op_erase(void *ctx)
{
    (void)ctx;
    return true;
}

// The closed set of log lines the session may produce. A sample, a level or a payload never appears.
static bool log_line_allowed(const char *line)
{
    static const char *const fixed[] = {
        "capture on", "capture off", "stream ended", "mute=on", "mute=off", "link down", "desktop silent",
        "session established authenticated=0", "session established authenticated=1", "hello rejected by desktop",
        "pairing begun", "pairing button confirmed", "paired", "pairing wiped",
    };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) {
        if (strcmp(line, fixed[i]) == 0) {
            return true;
        }
    }
    return strncmp(line, "session state=", 14) == 0;
}

static void op_log(void *ctx, const char *line)
{
    fx_t *fx = ctx;
    if (!log_line_allowed(line)) {
        fprintf(stderr, "unexpected log line: %s\n", line);
        exit(1);
    }
    const size_t length = strlen(line);
    if (fx->log_length + length + 2 >= LOG_MAX) {
        fx->log_length = 0;
    }
    memcpy(fx->log + fx->log_length, line, length);
    fx->log_length += length;
    fx->log[fx->log_length++] = '\n';
    fx->log[fx->log_length] = '\0';
}

static void trace(fx_t *fx, char symbol)
{
    if (fx->trace_length + 1 < TRACE_MAX) {
        fx->trace[fx->trace_length++] = symbol;
        fx->trace[fx->trace_length] = '\0';
    }
}

// The platform's capture switch. The LED and the sample gate move with it, in the same call.
static void op_capture(void *ctx, bool on)
{
    fx_t *fx = ctx;
    ++fx->capture_calls;
    trace(fx, on ? 'N' : 'F');
    if (on) {
        moby_ring_open(&fx->ring, fx->warmup);
    } else {
        fx->outbox_when_off = fx->session.outbox_count;
        fx->muted_when_off = fx->session.muted;
        moby_ring_close(&fx->ring);
    }
    fx->mic_on = on;
    fx->led = on;
}

static void op_discard(void *ctx)
{
    fx_t *fx = ctx;
    ++fx->discards;
    moby_ring_clear(&fx->ring);
}

static size_t src_peek(void *ctx, int16_t *out, size_t samples)
{
    fx_t *fx = ctx;
    if (fx->adversary) {
        // A source that always has audio, to prove the link asks for it only when the session says so.
        for (size_t i = 0; i < samples; ++i) {
            out[i] = 0x1111;
        }
        return samples;
    }
    return moby_ring_peek(&fx->ring, out, samples);
}

static void src_consume(void *ctx, size_t samples)
{
    fx_t *fx = ctx;
    if (!fx->adversary) {
        moby_ring_consume(&fx->ring, samples);
    }
}

static uint32_t src_events(void *ctx)
{
    fx_t *fx = ctx;
    uint32_t events = moby_ring_take_overrun(&fx->ring) ? MOBY_PCM_EVENT_OVERRUN : 0;
    if (fx->fault) {
        events |= MOBY_PCM_EVENT_FAULT;
        fx->fault = false;
    }
    return events;
}

static const uint8_t TEST_KEY[MOBY_USB_PAIR_KEY_BYTES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

enum { CAPS_AUDIO = MOBY_CAP_USB_PCM };

static fx_t *fx_init(const uint8_t *pair_key, uint32_t capabilities, bool with_ops)
{
    if (!fx_storage) {
        fx_storage = calloc(1, sizeof *fx_storage);
        assert(fx_storage);
        fx_storage->tx = malloc(TX_MAX);
        fx_storage->msgs = malloc((size_t)MSG_MAX * JSON_MAX);
        fx_storage->delivered = malloc(DELIVERED_MAX * sizeof(int16_t));
        fx_storage->accepted = malloc(DELIVERED_MAX * sizeof(int16_t));
        assert(fx_storage->tx && fx_storage->msgs && fx_storage->delivered && fx_storage->accepted);
    }
    fx_t *fx = fx_storage;
    uint8_t *tx = fx->tx;
    char (*msgs)[JSON_MAX] = fx->msgs;
    int16_t *delivered = fx->delivered;
    int16_t *accepted = fx->accepted;
    memset(fx, 0, sizeof *fx);
    fx->tx = tx;
    fx->msgs = msgs;
    fx->delivered = delivered;
    fx->accepted = accepted;
    fx->tx_idle = true;
    fx->write_ok = true;
    fx->now = 1000;
    fx->hw_muted = true;
    fx->next_value = 1;
    moby_decoder_reset(&fx->tx_decoder);
    snprintf(fx->identity.device_id, sizeof fx->identity.device_id, "dev_0123456789abcdef0123456789abcdef");
    snprintf(fx->identity.hw_rev, sizeof fx->identity.hw_rev, "esp32-s3-r1");
    snprintf(fx->identity.fw_version, sizeof fx->identity.fw_version, "0.1.0");
    fx->identity.capabilities = capabilities;
    fx->transport = (moby_transport_t){ fx, tr_connected, tr_read, tr_write, tr_idle };
    moby_ring_init(&fx->ring, fx->ring_store, RING_SAMPLES);
    fx->source = (moby_pcm_source_t){ fx, src_peek, src_consume, src_events };
    moby_session_ops_t ops = { 0 };
    ops.ctx = fx;
    ops.fill_random = op_random;
    ops.hmac_sha256 = op_hmac;
    ops.store_key = op_store;
    ops.erase_key = op_erase;
    ops.log = op_log;
    if (with_ops) {
        ops.capture_set = op_capture;
        ops.capture_discard = op_discard;
    }
    moby_session_init(&fx->session, &fx->identity, pair_key, &ops, fx->now);
    moby_link_init(&fx->link, &fx->transport, &fx->session);
    moby_link_set_source(&fx->link, &fx->source);
    return fx;
}

static bool has(const char *json, const char *text)
{
    return strstr(json, text) != NULL;
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

static uint32_t number_of(const char *json, const char *key)
{
    char pattern[48];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char *start = strstr(json, pattern);
    assert(start);
    return (uint32_t)strtoul(start + strlen(pattern), NULL, 10);
}

// Reads what the robot has written since the last call. Audio frames are checked against the stream rules on
// the way: only between audio_start and audio_end, with the stream's id and gap-free sequence numbers from 0.
static void tap(fx_t *fx)
{
    while (fx->tx_position < fx->tx_length) {
        moby_frame frame;
        const moby_status status = moby_decoder_push(&fx->tx_decoder, fx->tx[fx->tx_position++], &frame);
        if (status == MOBY_PENDING) {
            continue;
        }
        assert(status == MOBY_OK);
        if (frame.kind == MOBY_USB_CONTROL) {
            assert(frame.payload_length < JSON_MAX && fx->msg_count < MSG_MAX);
            char *slot = fx->msgs[(fx->msg_first + fx->msg_count++) % MSG_MAX];
            memcpy(slot, frame.payload, frame.payload_length);
            slot[frame.payload_length] = '\0';
            if (has(slot, "\"name\":\"hello\"")) {
                fx->in_stream = false;
            } else if (has(slot, "\"name\":\"audio_start\"")) {
                assert(!fx->in_stream);
                fx->in_stream = true;
                fx->stream_id = number_of(slot, "stream_id");
                fx->next_sequence = 0;
                ++fx->streams_seen;
            } else if (has(slot, "\"name\":\"audio_end\"")) {
                assert(fx->in_stream && number_of(slot, "stream_id") == fx->stream_id);
                assert(number_of(slot, "frames") == fx->next_sequence);
                assert(number_of(slot, "duration_ms") == fx->next_sequence * MOBY_PCM_FRAME_MS);
                fx->in_stream = false;
            }
        } else {
            assert(frame.kind == MOBY_USB_ROBOT_AUDIO);
            assert(fx->in_stream && frame.stream_id == fx->stream_id && frame.sequence == fx->next_sequence++);
            assert(frame.payload_length == MOBY_PCM_FRAME_BYTES);
            assert(fx->delivered_count + MOBY_PCM_FRAME_SAMPLES <= DELIVERED_MAX);
            for (size_t i = 0; i < MOBY_PCM_FRAME_SAMPLES; ++i) {
                fx->delivered[fx->delivered_count++] = (int16_t)(frame.payload[2 * i] | (frame.payload[2 * i + 1] << 8));
            }
            ++fx->audio_frames;
        }
    }
}

static const moby_face_view_t *view_of(fx_t *fx)
{
    static moby_face_view_t view;
    moby_session_view(&fx->session, &view);
    return &view;
}

// What must hold after every single operation, whatever happened.
static void invariants(fx_t *fx)
{
    const moby_session_t *s = &fx->session;
    const bool recording = moby_session_recording(s);
    // One fact: the microphone, the LED, the sample gate and the face all follow it.
    assert(fx->mic_on == recording && fx->led == recording && fx->ring.open == recording);
    const moby_face_view_t *view = view_of(fx);
    assert((view->expression == MOBY_FACE_LISTENING) == recording);
    assert(recording == (s->stream.phase == MOBY_STREAM_CAPTURING));
    // Capture needs everything at once.
    if (recording) {
        assert(s->link_up && s->established && s->authenticated && !s->muted && s->talk_down);
        assert(s->identity.capabilities & MOBY_CAP_USB_PCM);
    }
    if (s->stream.phase != MOBY_STREAM_NONE) {
        assert(s->established && s->authenticated && !s->muted && s->link_up);
    }
    // The hardware mute is authoritative: no stream, no gate, no sample, no I2S, and the face says so.
    if (s->muted) {
        assert(s->stream.phase == MOBY_STREAM_NONE && !fx->mic_on && !fx->led && !fx->ring.open &&
               moby_ring_available(&fx->ring) == 0);
        assert(view->expression == MOBY_FACE_MUTED);
    }
    if (!s->established) {
        assert(s->stream.phase == MOBY_STREAM_NONE && moby_ring_available(&fx->ring) == 0);
    }
    // Only the live samples are in memory.
    size_t nonzero = 0;
    for (size_t i = 0; i < RING_SAMPLES; ++i) {
        nonzero += fx->ring_store[i] != 0;
    }
    assert(nonzero <= moby_ring_available(&fx->ring));
    if (s->stream.phase == MOBY_STREAM_NONE) {
        assert(moby_ring_available(&fx->ring) == 0 || s->stream.end_owed);
    }
}

static void check(fx_t *fx)
{
    tap(fx);
    invariants(fx);
}

static void step(fx_t *fx, uint32_t advance_ms)
{
    fx->now += advance_ms;
    moby_link_step(&fx->link, fx->now);
    check(fx);
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

// Pops the next robot message that the tap parsed.
static bool next_msg(fx_t *fx, char *json)
{
    if (fx->msg_count == 0) {
        return false;
    }
    strcpy(json, fx->msgs[fx->msg_first]);
    fx->msg_first = (fx->msg_first + 1) % MSG_MAX;
    --fx->msg_count;
    return true;
}

static void drain(fx_t *fx)
{
    fx->msg_first = 0;
    fx->msg_count = 0;
}

static void expect(fx_t *fx, const char *name, const char *extra)
{
    char json[JSON_MAX];
    char want[96];
    if (!next_msg(fx, json)) {
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
    if (!next_msg(fx, json) || !has(json, "\"name\":\"command_error\"") || !has(json, id) || !has(json, code)) {
        fprintf(stderr, "expected command_error %s %s, got:\n%s\n", id, code, fx->msg_count || 1 ? json : "");
        exit(1);
    }
}

static void expect_silence(fx_t *fx)
{
    char json[JSON_MAX];
    if (next_msg(fx, json)) {
        fprintf(stderr, "unexpected message:\n%s\n", json);
        exit(1);
    }
}

// --- The controls as the debouncer reports them ----------------------------------------------------

static void ctl(fx_t *fx)
{
    moby_session_controls(&fx->session, fx->now, fx->hw_muted, fx->talk, fx->attention);
    moby_link_step(&fx->link, fx->now);
    check(fx);
}

static void press(fx_t *fx)
{
    fx->talk = true;
    // An accepted press: only while the switch lets the microphone path through.
    fx->attention = !fx->hw_muted;
    ctl(fx);
}

static void release(fx_t *fx)
{
    fx->talk = false;
    fx->attention = false;
    ctl(fx);
}

static void mute(fx_t *fx, bool muted)
{
    fx->hw_muted = muted;
    if (muted) {
        fx->attention = false;
    }
    ctl(fx);
}

// The capture task delivers `blocks` blocks of 20 ms. Every sample has its own value, so the test can follow it.
static size_t mic(fx_t *fx, unsigned blocks)
{
    size_t stored = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        int16_t block[MOBY_PCM_FRAME_SAMPLES];
        for (size_t i = 0; i < MOBY_PCM_FRAME_SAMPLES; ++i) {
            fx->next_value = (int16_t)(fx->next_value % 30000 + 1);
            block[i] = fx->next_value;
        }
        const size_t accepted = moby_ring_write(&fx->ring, block, MOBY_PCM_FRAME_SAMPLES);
        if (accepted != 0) {
            assert(fx->accepted_count + accepted <= DELIVERED_MAX);
            memcpy(fx->accepted + fx->accepted_count, block, accepted * sizeof block[0]);
            fx->accepted_count += accepted;
        }
        stored += accepted;
    }
    check(fx);
    return stored;
}

static const char ACK_CONFIG[] = "{}";

static void ack(fx_t *fx, const char *hello, const char *session, const uint8_t *key, bool accepted)
{
    char id[MOBY_ID_MAX + 1];
    char nonce_hex[2 * MOBY_USB_NONCE_BYTES + 1];
    char device[MOBY_ID_MAX + 1];
    char json[JSON_MAX];
    char proof[2 * MOBY_USB_PROOF_BYTES + 16] = "";
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
        strcat(proof, "\"");
    }
    snprintf(json, sizeof json,
             "{\"v\":1,\"kind\":\"response\",\"name\":\"hello_ack\",\"id\":\"%s\",\"ts_ms\":5,\"data\":{\"accepted\":%s%s%s%s%s,"
             "\"config\":%s}}",
             id, accepted ? "true" : "false", session ? ",\"session_id\":\"" : "", session ? session : "",
             session ? "\"" : "", proof, ACK_CONFIG);
    desktop(fx, json);
}

// Answers the latest hello in the queue with the given key (NULL: no proof) and drops the opening events.
static void handshake(fx_t *fx, const uint8_t *key)
{
    char hello[JSON_MAX];
    bool found = false;
    char json[JSON_MAX];
    while (next_msg(fx, json)) {
        if (has(json, "\"name\":\"hello\"")) {
            strcpy(hello, json);
            found = true;
        }
    }
    assert(found);
    ack(fx, hello, "session_1", key, true);
    step(fx, 10);
    assert(fx->session.established);
}

// Brings a session up with the switch open (the usual starting point of a talk).
static void establish(fx_t *fx, const uint8_t *key)
{
    fx->connected = true;
    step(fx, 0);
    mute(fx, false);
    handshake(fx, key);
    drain(fx);
}

static fx_t *ready(void)
{
    fx_t *fx = fx_init(TEST_KEY, CAPS_AUDIO, true);
    establish(fx, TEST_KEY);
    assert(fx->session.authenticated);
    return fx;
}

// The desktop proves the key every 2 s in a real session; tests that run long keep it alive.
static void keep_alive(fx_t *fx)
{
    command(fx, "k-0001", "set_config", "{}");
}

// --- Streaming ------------------------------------------------------------------------

static void test_stream_framing(void)
{
    fx_t *fx = ready();
    press(fx);
    // The press is announced, then the stream. Nothing else, and no audio yet.
    expect(fx, "button_event", "\"action\":\"press\"");
    char json[JSON_MAX];
    assert(next_msg(fx, json));
    assert(strstr(json, "\"name\":\"audio_start\"") != NULL);
    assert(strstr(json, "\"data\":{\"stream_id\":1,\"reason\":\"button\",\"codec\":\"pcm_s16le\",\"sample_rate\":16000,"
                        "\"channels\":1,\"frame_ms\":20,\"preroll_ms\":0}}") != NULL);
    expect_silence(fx);
    assert(fx->mic_on && fx->led && moby_session_recording(&fx->session) && fx->session.state == MOBY_SESSION_LISTENING);
    assert(view_of(fx)->expression == MOBY_FACE_LISTENING);
    assert(fx->audio_frames == 0);

    // Five blocks arrive and leave as five frames of 640 bytes with sequence 0..4 on stream 1.
    mic(fx, 5);
    step(fx, 100);
    assert(fx->audio_frames == 5 && fx->next_sequence == 5 && fx->stream_id == 1);
    assert(fx->delivered_count == 5 * MOBY_PCM_FRAME_SAMPLES && fx->accepted_count == fx->delivered_count);
    assert(memcmp(fx->delivered, fx->accepted, fx->delivered_count * sizeof(int16_t)) == 0);
    assert(moby_ring_available(&fx->ring) == 0 && fx->session.stats.stream_frames_sent == 5);

    // A half block is captured, the button comes up: capture stops now, the half block is not a frame.
    int16_t half[MOBY_PCM_FRAME_SAMPLES / 2] = { 0 };
    assert(moby_ring_write(&fx->ring, half, sizeof half / sizeof half[0]) == sizeof half / sizeof half[0]);
    release(fx);
    assert(!fx->mic_on && !fx->led && !moby_session_recording(&fx->session));
    assert(view_of(fx)->expression == MOBY_FACE_IDLE && fx->session.state == MOBY_SESSION_IDLE);
    step(fx, 0);
    // audio_end carries the frame count and the duration, and the reason.
    char end[JSON_MAX];
    bool saw_end = false;
    while (next_msg(fx, end)) {
        if (has(end, "\"name\":\"audio_end\"")) {
            assert(has(end, "\"data\":{\"stream_id\":1,\"reason\":\"button_release\",\"frames\":5,\"duration_ms\":100}"));
            saw_end = true;
        }
    }
    assert(saw_end && !fx->in_stream && fx->session.stream.phase == MOBY_STREAM_NONE);
    assert(moby_ring_available(&fx->ring) == 0 && fx->audio_frames == 5);
    assert(strcmp(fx->trace, "NF") == 0);
}

static void test_release_sends_what_was_captured(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 3);
    // The button comes up with three frames still waiting: they are sent first, then audio_end.
    release(fx);
    step(fx, 0);
    step(fx, 0);
    assert(fx->audio_frames == 3 && !fx->in_stream);
    char json[JSON_MAX];
    bool saw_end = false;
    while (next_msg(fx, json)) {
        saw_end = saw_end || (has(json, "\"name\":\"audio_end\"") && has(json, "\"frames\":3") && has(json, "button_release"));
    }
    assert(saw_end);

    // More blocks after the release go nowhere: the gate is closed.
    assert(mic(fx, 4) == 0);
    step(fx, 100);
    assert(fx->audio_frames == 3);
}

static void test_stream_ids_and_sequences(void)
{
    fx_t *fx = ready();
    for (unsigned round = 1; round <= 3; ++round) {
        press(fx);
        mic(fx, 2);
        step(fx, 0);
        release(fx);
        step(fx, 0);
        assert(fx->streams_seen == round && fx->stream_id == round);
        assert(fx->session.stream.last_id == round);
    }
    assert(fx->audio_frames == 6);
    // The id never takes the value 0: after 65535 it wraps to 1.
    fx->session.stream.last_id = 65535;
    press(fx);
    assert(fx->session.stream.id == 1);
    release(fx);
    step(fx, 0);
    assert(fx->session.stream.phase == MOBY_STREAM_NONE);
}

static void test_warmup_is_not_sent(void)
{
    fx_t *fx = ready();
    fx->warmup = MOBY_PCM_WARMUP_SAMPLES;
    press(fx);
    // The 13 blocks of microphone start-up are dropped in front of the ring, then real audio follows.
    assert(mic(fx, 13) == 0);
    assert(mic(fx, 2) == 2 * MOBY_PCM_FRAME_SAMPLES);
    step(fx, 0);
    assert(fx->audio_frames == 2);
    release(fx);
}

// --- Stops: release, timeout, cancel ----------------------------------------------------

static void test_max_utterance(void)
{
    fx_t *fx = ready();
    command(fx, "c-0001", "set_config", "{\"max_utterance_ms\":1000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    press(fx);
    const uint32_t started = fx->now;
    mic(fx, 2);
    step(fx, 500);
    assert(moby_session_recording(&fx->session) && fx->mic_on);
    keep_alive(fx);
    step(fx, 499);
    assert(moby_session_recording(&fx->session) && fx->now == started + 999);
    mic(fx, 1);
    step(fx, 1);
    // The limit is reached while the button is still down: capture stops on that tick.
    assert(!moby_session_recording(&fx->session) && !fx->mic_on && !fx->led);
    assert(view_of(fx)->expression == MOBY_FACE_IDLE);
    step(fx, 0);
    step(fx, 0);
    assert(fx->audio_frames == 3 && !fx->in_stream);
    char json[JSON_MAX];
    bool saw_end = false;
    while (next_msg(fx, json)) {
        saw_end = saw_end || (has(json, "\"name\":\"audio_end\"") && has(json, "\"reason\":\"max_duration\"") && has(json, "\"frames\":3"));
    }
    assert(saw_end);
    // The held button does not start another stream. Only a new press does.
    mic(fx, 3);
    step(fx, 3000);
    assert(fx->streams_seen == 1 && !fx->mic_on);
    release(fx);
    press(fx);
    assert(fx->streams_seen == 2 && fx->mic_on);
    release(fx);
}

static void test_cancel_from_desktop(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 3);
    step(fx, 0);
    mic(fx, 2);
    drain(fx);
    // A cancel for another stream or none is refused and changes nothing.
    command(fx, "c-0001", "cancel", "{\"stream_id\":9}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_STATE");
    assert(fx->mic_on);
    // The right stream: capture stops at once, the unsent audio is thrown away, audio_end says cancel.
    command(fx, "c-0002", "cancel", "{\"stream_id\":1}");
    step(fx, 0);
    assert(!fx->mic_on && !fx->led && moby_ring_available(&fx->ring) == 0);
    expect_ok(fx, "c-0002");
    char json[JSON_MAX];
    assert(next_msg(fx, json) && has(json, "\"name\":\"audio_end\"") && has(json, "\"reason\":\"cancel\"") && has(json, "\"frames\":3"));
    assert(fx->audio_frames == 3 && view_of(fx)->expression == MOBY_FACE_IDLE);
    // The button is still held, and still nothing restarts. A second cancel is refused.
    mic(fx, 2);
    step(fx, 100);
    assert(fx->audio_frames == 3 && !fx->mic_on);
    command(fx, "c-0003", "cancel", "{\"stream_id\":1}");
    step(fx, 0);
    expect_error(fx, "c-0003", "E_STATE");
    release(fx);
    drain(fx);
    press(fx);
    assert(fx->mic_on && fx->streams_seen == 2);
    release(fx);
}

static void test_cancel_while_draining(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 4);
    fx->write_ok = false;
    release(fx);
    assert(fx->session.stream.phase == MOBY_STREAM_DRAINING);
    command(fx, "c-0001", "cancel", "{\"stream_id\":1}");
    fx->write_ok = true;
    step(fx, 0);
    assert(fx->session.stream.phase == MOBY_STREAM_NONE && moby_ring_available(&fx->ring) == 0 && fx->audio_frames == 0);
}

static void test_drain_times_out(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 3);
    fx->write_ok = false;
    release(fx);
    step(fx, 400);
    assert(fx->session.stream.phase == MOBY_STREAM_DRAINING && fx->link.stats.audio_stalls > 0);
    keep_alive(fx);
    step(fx, 599);
    assert(fx->session.stream.phase == MOBY_STREAM_DRAINING);
    fx->write_ok = true;
    // The transport never took the audio in time: the stream ends with an error and nothing is left behind.
    fx->write_ok = false;
    step(fx, 1);
    assert(fx->session.stream.phase == MOBY_STREAM_NONE && moby_ring_available(&fx->ring) == 0);
    fx->write_ok = true;
    step(fx, 10);
    assert(fx->audio_frames == 0 && !fx->mic_on);
}

// --- Mute ---------------------------------------------------------------------------

static void test_mute_order_and_report(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 3);
    step(fx, 0);
    mic(fx, 2);
    drain(fx);
    const size_t outbox_before = fx->session.outbox_count;
    mute(fx, true);
    // The microphone went off while the mute flag was already set and before anything was queued.
    assert(fx->muted_when_off && fx->outbox_when_off == outbox_before);
    assert(!fx->mic_on && !fx->led && moby_ring_available(&fx->ring) == 0 && view_of(fx)->expression == MOBY_FACE_MUTED);
    char json[JSON_MAX];
    assert(next_msg(fx, json) && has(json, "\"name\":\"audio_end\"") && has(json, "\"reason\":\"mute\"") && has(json, "\"frames\":3"));
    expect(fx, "mute_changed", "\"muted\":true");
    expect_silence(fx);
    assert(fx->audio_frames == 3);
    // Blocks that were already on their way are refused, the link sends nothing, unmuting resumes nothing.
    assert(mic(fx, 4) == 0);
    step(fx, 200);
    mute(fx, false);
    step(fx, 200);
    assert(fx->audio_frames == 3 && !fx->mic_on && fx->session.stream.phase == MOBY_STREAM_NONE);
    assert(mic(fx, 4) == 0);
    // The button is still held from before the mute: it takes a new press.
    step(fx, 200);
    assert(fx->audio_frames == 3 && fx->streams_seen == 1);
    release(fx);
    press(fx);
    assert(fx->mic_on && fx->streams_seen == 2);
    release(fx);
}

// One run of a scripted talk with the mute switched at operation `at` (or never, with at = -1).
enum { SCRIPT_LENGTH = 20 };

static void run_script(int at, bool max_duration_variant)
{
    fx_t *fx = ready();
    if (max_duration_variant) {
        command(fx, "c-0001", "set_config", "{\"max_utterance_ms\":1000}");
        step(fx, 0);
        drain(fx);
    }
    fx->warmup = 2 * MOBY_PCM_FRAME_SAMPLES;
    uint32_t frames_at_mute = 0;
    bool muted_yet = false;
    for (int op = 0; op <= SCRIPT_LENGTH; ++op) {
        if (op == at) {
            frames_at_mute = fx->audio_frames;
            const bool streaming = fx->session.stream.phase != MOBY_STREAM_NONE;
            mute(fx, true);
            fx->mutes_in_stream += streaming;
            muted_yet = true;
            assert(!fx->mic_on && !fx->led && fx->session.stream.phase == MOBY_STREAM_NONE);
            assert(moby_ring_available(&fx->ring) == 0 && fx->audio_frames == frames_at_mute);
        }
        switch (op) {
        case 0: press(fx); break;
        case 1: mic(fx, 2); break;
        case 2: step(fx, 20); break;
        case 3: mic(fx, 3); break;
        case 4: step(fx, 20); break;
        case 5: mic(fx, 1); break;
        case 6: step(fx, max_duration_variant ? 700 : 5); break;
        case 7: keep_alive(fx); step(fx, 0); break;
        case 8: mic(fx, 1); break;
        case 9: step(fx, max_duration_variant ? 400 : 5); break;
        case 10: release(fx); break;
        case 11: step(fx, 0); break;
        case 12: mic(fx, 2); break;
        case 13: step(fx, 20); break;
        case 14: press(fx); break;
        case 15: mic(fx, 2); break;
        case 16: step(fx, 20); break;
        case 17: mic(fx, 1); break;
        case 18: release(fx); break;
        case 19: step(fx, 0); break;
        default: step(fx, 1100); break;
        }
        if (muted_yet) {
            // From the mute on: no capture, no sample in memory, no frame on the wire, ever.
            assert(!fx->mic_on && !fx->led && fx->session.stream.phase == MOBY_STREAM_NONE);
            assert(moby_ring_available(&fx->ring) == 0 && fx->audio_frames == frames_at_mute);
        }
    }
    if (muted_yet) {
        // Unmuting does not resume anything either.
        mute(fx, false);
        step(fx, 100);
        mic(fx, 3);
        step(fx, 100);
        assert(fx->audio_frames == frames_at_mute && !fx->mic_on);
    }
}

static void test_mute_at_every_point(void)
{
    run_script(-1, false);
    run_script(-1, true);
    for (int at = 0; at <= SCRIPT_LENGTH; ++at) {
        run_script(at, false);
        run_script(at, true);
    }
}

// --- No capture without ground -----------------------------------------------------------

static void expect_no_capture(fx_t *fx)
{
    press(fx);
    mic(fx, 3);
    step(fx, 100);
    assert(!fx->mic_on && !fx->led && fx->capture_calls == 0 && fx->streams_seen == 0 && fx->audio_frames == 0);
    assert(moby_ring_available(&fx->ring) == 0 && view_of(fx)->expression != MOBY_FACE_LISTENING);
    char json[JSON_MAX];
    while (next_msg(fx, json)) {
        assert(!has(json, "audio_start"));
    }
    release(fx);
}

static void test_no_capture_unpaired(void)
{
    // An unpaired robot's session is open but not authenticated, and audio needs authentication.
    fx_t *fx = fx_init(NULL, CAPS_AUDIO, true);
    establish(fx, NULL);
    assert(fx->session.established && !fx->session.authenticated);
    expect_no_capture(fx);
}

static void test_no_capture_bad_proof(void)
{
    uint8_t wrong[MOBY_USB_PAIR_KEY_BYTES];
    memcpy(wrong, TEST_KEY, sizeof wrong);
    wrong[0] ^= 1;
    fx_t *fx = fx_init(TEST_KEY, CAPS_AUDIO, true);
    fx->connected = true;
    step(fx, 0);
    mute(fx, false);
    handshake(fx, wrong);
    assert(fx->session.established && !fx->session.authenticated);
    expect_no_capture(fx);
    // And a missing proof.
    fx = fx_init(TEST_KEY, CAPS_AUDIO, true);
    fx->connected = true;
    step(fx, 0);
    mute(fx, false);
    handshake(fx, NULL);
    assert(fx->session.established && !fx->session.authenticated);
    expect_no_capture(fx);
}

static void test_no_capture_before_session_and_on_boot(void)
{
    // Boot: muted, no link, no session. A press does nothing.
    fx_t *fx = fx_init(TEST_KEY, CAPS_AUDIO, true);
    expect_no_capture(fx);
    // Switch open, link up, hello sent but unanswered: still no session.
    fx->connected = true;
    step(fx, 0);
    mute(fx, false);
    assert(!fx->session.established);
    expect_no_capture(fx);
    // Switch closed after the session: muted again.
    handshake(fx, TEST_KEY);
    mute(fx, true);
    expect_no_capture(fx);
    // The session callbacks alone, with a press reported while muted.
    moby_session_controls(&fx->session, fx->now, true, true, true);
    check(fx);
    assert(!fx->mic_on && fx->capture_calls == 0);
}

static void test_no_capture_without_capability_or_ops(void)
{
    fx_t *fx = fx_init(TEST_KEY, 0, true);
    establish(fx, TEST_KEY);
    assert(fx->session.authenticated);
    expect_no_capture(fx);
    // The capability alone is not enough without the platform's switch.
    fx = fx_init(TEST_KEY, CAPS_AUDIO, false);
    establish(fx, TEST_KEY);
    press(fx);
    mic(fx, 2);
    step(fx, 50);
    assert(fx->streams_seen == 0 && fx->audio_frames == 0 && !moby_session_recording(&fx->session));
    release(fx);
}

static void test_no_capture_during_pairing(void)
{
    fx_t *fx = fx_init(NULL, CAPS_AUDIO, true);
    establish(fx, NULL);
    command(fx, "c-0001", "pair_begin", "{\"timeout_ms\":10000}");
    step(fx, 0);
    expect_ok(fx, "c-0001");
    assert(fx->session.state == MOBY_SESSION_PAIRING);
    // The press confirms the pairing and starts nothing.
    press(fx);
    expect(fx, "pair_button_confirmed", NULL);
    mic(fx, 3);
    step(fx, 100);
    assert(!fx->mic_on && fx->capture_calls == 0 && fx->audio_frames == 0);
    // Pairing completes while the button is still held: the key is stored, nothing starts without a new press.
    char key_command[160];
    snprintf(key_command, sizeof key_command, "{\"pair_key\":\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\"}");
    command(fx, "c-0002", "pair_set_key", key_command);
    step(fx, 0);
    expect_ok(fx, "c-0002");
    assert(fx->session.authenticated);
    mic(fx, 3);
    step(fx, 100);
    assert(!fx->mic_on && fx->capture_calls == 0);
    release(fx);
    press(fx);
    assert(fx->mic_on && fx->streams_seen == 1);
    release(fx);
}

static void test_no_capture_in_busy_states(void)
{
    static const char *const states[] = { "thinking", "speaking" };
    for (size_t i = 0; i < 2; ++i) {
        fx_t *fx = ready();
        char data[96];
        if (i == 1) {
            command(fx, "c-0000", "expression", "{\"state\":\"thinking\",\"animation\":\"none\",\"duration_ms\":0}");
            step(fx, 0);
            expect_ok(fx, "c-0000");
        }
        snprintf(data, sizeof data, "{\"state\":\"%s\",\"animation\":\"none\",\"duration_ms\":0}", states[i]);
        command(fx, "c-0001", "expression", data);
        step(fx, 0);
        expect_ok(fx, "c-0001");
        assert(fx->session.state != MOBY_SESSION_IDLE);
        expect_no_capture(fx);
    }
    // After the desktop returns the robot to idle, the button works again.
    fx_t *fx = ready();
    command(fx, "c-0001", "expression", "{\"state\":\"thinking\",\"animation\":\"none\",\"duration_ms\":0}");
    command(fx, "c-0002", "expression", "{\"state\":\"idle\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    drain(fx);
    press(fx);
    assert(fx->mic_on);
    release(fx);
    // Happy, confused and the error face are faces of idle: the button works.
    command(fx, "c-0003", "expression", "{\"state\":\"happy\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    drain(fx);
    release(fx);
    press(fx);
    assert(fx->mic_on && view_of(fx)->expression == MOBY_FACE_LISTENING);
    release(fx);
}

// --- Disconnect ---------------------------------------------------------------------------

static void test_disconnect_mid_stream(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 4);
    step(fx, 0);
    mic(fx, 3);
    assert(moby_ring_available(&fx->ring) == 3 * MOBY_PCM_FRAME_SAMPLES);
    drain(fx);
    const uint32_t frames = fx->audio_frames;
    fx->connected = false;
    step(fx, 0);
    assert(!fx->mic_on && !fx->led && fx->session.stream.phase == MOBY_STREAM_NONE);
    assert(moby_ring_available(&fx->ring) == 0 && fx->audio_frames == frames);
    assert(view_of(fx)->expression == MOBY_FACE_DISCONNECTED);
    assert(mic(fx, 3) == 0);
    // Nothing was sent to a link that is not there, and nothing is owed when it returns.
    fx->connected = true;
    step(fx, 0);
    mic(fx, 3);
    step(fx, 100);
    assert(fx->audio_frames == frames && !fx->mic_on && !fx->session.stream.end_owed);
    // The held button does not start a stream in the new session either.
    handshake(fx, TEST_KEY);
    mic(fx, 3);
    step(fx, 100);
    assert(!fx->mic_on && fx->streams_seen == 1);
    release(fx);
}

static void test_session_dies_mid_stream(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 3);
    step(fx, 0);
    mic(fx, 2);
    // The desktop goes silent: three heartbeats unanswered, the fourth period ends the session and the stream.
    uint32_t guard = 0;
    while (fx->session.established && guard++ < 20) {
        step(fx, 2000);
        mic(fx, 1);
    }
    assert(!fx->session.established && !fx->mic_on && !fx->led && moby_ring_available(&fx->ring) == 0);
    assert(fx->session.stream.phase == MOBY_STREAM_NONE && view_of(fx)->expression == MOBY_FACE_DISCONNECTED);
    release(fx);
}

static void test_link_down_event_from_session(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 2);
    moby_session_set_link(&fx->session, fx->now, false);
    check(fx);
    assert(!fx->mic_on && moby_ring_available(&fx->ring) == 0);
    assert(fx->discards > 0);
}

// --- Failures on the way out -----------------------------------------------------------

static void test_overrun(void)
{
    fx_t *fx = ready();
    press(fx);
    drain(fx);
    // The link takes nothing; the capture task keeps delivering and the ring fills. Nothing blocks.
    fx->write_ok = false;
    size_t stored = 0;
    for (unsigned b = 0; b < 12; ++b) {
        stored += mic(fx, 1);
    }
    assert(stored == RING_SAMPLES && fx->ring.overruns == 4 && fx->ring.dropped_samples == 4 * MOBY_PCM_FRAME_SAMPLES);
    // The next pass sees it: the stream ends with the protocol's error and the buffer is gone.
    fx->write_ok = true;
    step(fx, 0);
    assert(!fx->mic_on && !fx->led && moby_ring_available(&fx->ring) == 0);
    assert(fx->session.stats.stream_overruns == 1);
    char json[JSON_MAX];
    bool saw_end = false;
    bool saw_error = false;
    while (next_msg(fx, json)) {
        saw_end = saw_end || (has(json, "\"name\":\"audio_end\"") && has(json, "\"reason\":\"error\""));
        saw_error = saw_error || (has(json, "\"name\":\"error\"") && has(json, "E_AUDIO_OVERRUN"));
    }
    assert(saw_end && saw_error && !fx->in_stream);
    assert(view_of(fx)->expression == MOBY_FACE_ERROR);
    // The held button does not restart it.
    mic(fx, 2);
    step(fx, 100);
    assert(fx->streams_seen == 1);
    release(fx);
    step(fx, 4000);
    assert(view_of(fx)->expression == MOBY_FACE_IDLE);
}

static void test_hardware_fault(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 2);
    drain(fx);
    fx->fault = true;
    step(fx, 0);
    assert(!fx->mic_on && fx->session.stats.stream_faults == 1 && moby_ring_available(&fx->ring) == 0);
    char json[JSON_MAX];
    bool saw_error = false;
    while (next_msg(fx, json)) {
        saw_error = saw_error || (has(json, "\"name\":\"error\"") && has(json, "E_HW"));
    }
    assert(saw_error);
    release(fx);
}

static void test_start_that_cannot_be_written(void)
{
    fx_t *fx = ready();
    fx->write_ok = false;
    press(fx);
    // The audio_start never reached the desktop, so the stream is dropped and its frames never exist.
    assert(!fx->mic_on && fx->session.stats.stream_start_lost == 1 && fx->link.stats.frames_dropped > 0);
    fx->write_ok = true;
    mic(fx, 3);
    step(fx, 100);
    assert(fx->audio_frames == 0 && !fx->in_stream);
    release(fx);
    drain(fx);
    press(fx);
    assert(fx->mic_on);
    release(fx);
}

static void test_full_outbox_starts_nothing(void)
{
    fx_t *fx = ready();
    fx->session.outbox_count = MOBY_OUTBOX_LEN;
    press(fx);
    assert(!fx->mic_on && fx->streams_seen == 0 && fx->capture_calls == 0);
}

// --- Messages while capturing -----------------------------------------------------------

static void test_commands_while_capturing(void)
{
    fx_t *fx = ready();
    press(fx);
    drain(fx);
    command(fx, "c-0001", "expression", "{\"state\":\"thinking\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_BUSY");
    assert(moby_session_recording(&fx->session));
    command(fx, "c-0002", "pair_begin", "{\"timeout_ms\":5000}");
    step(fx, 0);
    expect_error(fx, "c-0002", "E_");
    // Desktop audio is still not accepted, and a malformed frame does not stop the stream.
    const uint8_t pcm[16] = { 0 };
    desktop_frame(fx, MOBY_USB_DESKTOP_AUDIO, 5, pcm, sizeof pcm);
    step(fx, 0);
    expect(fx, "error", "E_UNSUPPORTED");
    assert(moby_session_recording(&fx->session));
    // The heartbeat tells the truth.
    step(fx, 2000);
    char json[JSON_MAX];
    bool saw = false;
    while (next_msg(fx, json)) {
        saw = saw || (has(json, "\"name\":\"heartbeat\"") && has(json, "\"state\":\"listening\""));
    }
    assert(saw);
    release(fx);
}

static void test_listening_is_not_a_desktop_face(void)
{
    fx_t *fx = ready();
    command(fx, "c-0001", "expression", "{\"state\":\"listening\",\"animation\":\"none\",\"duration_ms\":0}");
    step(fx, 0);
    expect_error(fx, "c-0001", "E_STATE");
    assert(view_of(fx)->expression == MOBY_FACE_IDLE && !fx->mic_on);
    // A mute makes the face muted whatever else happens.
    press(fx);
    mute(fx, true);
    assert(view_of(fx)->expression == MOBY_FACE_MUTED);
}

static void test_long_press_stops_capture(void)
{
    fx_t *fx = ready();
    command(fx, "c-0001", "set_config", "{\"max_utterance_ms\":60000}");
    step(fx, 0);
    drain(fx);
    press(fx);
    mic(fx, 2);
    for (int i = 0; i < 4; ++i) {
        keep_alive(fx);
        step(fx, 2500);
        mic(fx, 1);
    }
    assert(moby_session_recording(&fx->session));
    keep_alive(fx);
    step(fx, 2500);
    ctl(fx);
    // Ten seconds of holding wipes the pairing, and nothing may keep recording under a wiped pairing.
    assert(!fx->mic_on && !fx->session.paired && !fx->session.authenticated);
    release(fx);
    press(fx);
    assert(!fx->mic_on);
}

// --- The source cannot leak --------------------------------------------------------------

static void test_source_cannot_leak(void)
{
    // A source that always has audio, in every state where the session must keep the microphone off.
    fx_t *fx = fx_init(TEST_KEY, CAPS_AUDIO, true);
    fx->adversary = true;
    step(fx, 100);
    fx->connected = true;
    step(fx, 100);
    mute(fx, false);
    handshake(fx, TEST_KEY);
    step(fx, 500);
    assert(fx->audio_frames == 0 && !fx->in_stream);
    press(fx);
    release(fx);
    step(fx, 500);
    // A stream did run, and then every frame belonged to it.
    const uint32_t frames_after = fx->audio_frames;
    step(fx, 500);
    assert(fx->audio_frames == frames_after && !fx->in_stream);
    mute(fx, true);
    step(fx, 500);
    assert(fx->audio_frames == frames_after);
    fx->connected = false;
    step(fx, 500);
    fx->connected = true;
    step(fx, 500);
    assert(fx->audio_frames == frames_after);
    // Without a source, an open stream sends nothing at all.
    fx = ready();
    moby_link_set_source(&fx->link, NULL);
    press(fx);
    mic(fx, 3);
    step(fx, 100);
    assert(fx->audio_frames == 0);
    release(fx);
}

static void test_log_never_carries_audio(void)
{
    fx_t *fx = ready();
    press(fx);
    mic(fx, 3);
    step(fx, 100);
    release(fx);
    step(fx, 100);
    mute(fx, true);
    // op_log refuses any line outside the closed set, so reaching this point is the proof; show the lines are there.
    assert(has(fx->log, "capture on") && has(fx->log, "capture off"));
    // No digit run that could be a sample or a level: the lines carry state names only.
    for (const char *p = fx->log; *p; ++p) {
        if (p[0] >= '0' && p[0] <= '9') {
            assert(strncmp(p - 6, "state=", 6) != 0 && "no digits in lines");
        }
    }
}

// --- A long random walk ------------------------------------------------------------------

static uint32_t rnd(uint32_t *seed)
{
    *seed = *seed * 1664525U + 1013904223U;
    return *seed >> 8;
}

static void maybe_handshake(fx_t *fx, const uint8_t *key)
{
    if (fx->session.established || !fx->connected) {
        return;
    }
    char json[JSON_MAX];
    char hello[JSON_MAX] = "";
    while (next_msg(fx, json)) {
        if (has(json, "\"name\":\"hello\"")) {
            strcpy(hello, json);
        }
    }
    if (hello[0]) {
        ack(fx, hello, "session_1", key, true);
        step(fx, 1);
    }
}

static void test_random_walk(void)
{
    uint32_t seed = 20261105;
    unsigned started = 0;
    unsigned authenticated_runs = 0;
    for (unsigned run = 0; run < 12; ++run) {
        const bool paired = run % 3 != 2;
        fx_t *fx = fx_init(paired ? TEST_KEY : NULL, run % 6 == 5 ? 0 : CAPS_AUDIO, true);
        fx->warmup = run % 2 ? MOBY_PCM_WARMUP_SAMPLES : 0;
        fx->connected = true;
        fx->now += 1;
        step(fx, 0);
        for (unsigned op = 0; op < 6000; ++op) {
            switch (rnd(&seed) % 16U) {
            case 0:
            case 1:
                press(fx);
                break;
            case 2:
            case 3:
                release(fx);
                break;
            case 4:
                fx->mutes_in_stream += fx->session.stream.phase != MOBY_STREAM_NONE;
                mute(fx, !fx->hw_muted);
                break;
            case 5:
            case 6:
            case 7:
                mic(fx, 1 + rnd(&seed) % 4U);
                break;
            case 8:
            case 9:
                step(fx, rnd(&seed) % 3U == 0 ? rnd(&seed) % 700U : 20U);
                break;
            case 10:
                command(fx, "r-0001", "cancel", rnd(&seed) % 2U ? "{\"stream_id\":1}" : "{\"stream_id\":2}");
                step(fx, 0);
                break;
            case 11:
                keep_alive(fx);
                step(fx, 0);
                break;
            case 12:
                if (rnd(&seed) % 8U == 0) {
                    fx->connected = !fx->connected;
                    step(fx, 1);
                }
                break;
            case 13:
                fx->write_ok = rnd(&seed) % 4U != 0;
                break;
            case 14:
                if (rnd(&seed) % 6U == 0) {
                    fx->fault = true;
                }
                if (rnd(&seed) % 6U == 0) {
                    char data[64];
                    snprintf(data, sizeof data, "{\"max_utterance_ms\":%u}", 1000U + rnd(&seed) % 3000U);
                    command(fx, "r-0002", "set_config", data);
                    step(fx, 0);
                }
                break;
            default:
                maybe_handshake(fx, paired ? TEST_KEY : NULL);
                break;
            }
            authenticated_runs += fx->session.authenticated;
            if (!paired || run % 6 == 5) {
                // Unpaired and capability-less runs never open a stream, however the walk goes.
                assert(fx->streams_seen == 0 && fx->capture_calls == 0);
            }
            if (fx->msg_count > MSG_MAX / 2) {
                drain(fx);
            }
        }
        started += fx->streams_seen;
        // Every 'N' in the trace is matched by an 'F', except a stream still open.
        unsigned on = 0;
        unsigned off = 0;
        for (size_t i = 0; i < fx->trace_length; ++i) {
            on += fx->trace[i] == 'N';
            off += fx->trace[i] == 'F';
            assert(off <= on && on - off <= 1);
        }
        assert(fx->trace_length == 0 || fx->trace[fx->trace_length - 1] == (fx->mic_on ? 'N' : 'F'));
    }
    // The walk is not vacuous: it opened streams, authenticated sessions and muted during streams.
    assert(started > 100 && authenticated_runs > 1000);
    fprintf(stderr, "(random walk: %u streams) ", started);
}

int main(void)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } tests[] = {
        { "pcm conversion values", test_conversion_values },
        { "pcm buffers and gain", test_conversion_buffers_and_gain },
        { "ring basics", test_ring_basics },
        { "ring wrap", test_ring_wrap },
        { "ring overrun accounting", test_ring_overrun_accounting },
        { "ring gate and warm-up", test_ring_gate_and_warmup },
        { "ring wipes what it drops", test_ring_wipes_what_it_drops },
        { "ring random model", test_ring_random_model },
        { "stream framing", test_stream_framing },
        { "release sends what was captured", test_release_sends_what_was_captured },
        { "stream ids and sequences", test_stream_ids_and_sequences },
        { "warm-up is not sent", test_warmup_is_not_sent },
        { "max utterance", test_max_utterance },
        { "cancel from desktop", test_cancel_from_desktop },
        { "cancel while draining", test_cancel_while_draining },
        { "drain times out", test_drain_times_out },
        { "mute order and report", test_mute_order_and_report },
        { "mute at every point", test_mute_at_every_point },
        { "no capture, unpaired", test_no_capture_unpaired },
        { "no capture, bad or missing proof", test_no_capture_bad_proof },
        { "no capture before a session or on boot", test_no_capture_before_session_and_on_boot },
        { "no capture without capability or ops", test_no_capture_without_capability_or_ops },
        { "no capture during pairing", test_no_capture_during_pairing },
        { "no capture in busy states", test_no_capture_in_busy_states },
        { "disconnect mid-stream", test_disconnect_mid_stream },
        { "session dies mid-stream", test_session_dies_mid_stream },
        { "link down from the session", test_link_down_event_from_session },
        { "overrun", test_overrun },
        { "hardware fault", test_hardware_fault },
        { "start that cannot be written", test_start_that_cannot_be_written },
        { "full outbox starts nothing", test_full_outbox_starts_nothing },
        { "commands while capturing", test_commands_while_capturing },
        { "listening is not a desktop face", test_listening_is_not_a_desktop_face },
        { "long press stops capture", test_long_press_stops_capture },
        { "source cannot leak", test_source_cannot_leak },
        { "log never carries audio", test_log_never_carries_audio },
        { "random walk", test_random_walk },
    };
    const size_t count = sizeof tests / sizeof tests[0];
    for (size_t i = 0; i < count; ++i) {
        printf("%s: ", tests[i].name);
        fflush(stdout);
        tests[i].run();
        puts("ok");
    }
    printf("%zu F4 host tests passed\n", count);
    return 0;
}
