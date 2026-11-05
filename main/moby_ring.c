// SPDX-License-Identifier: GPL-3.0-only

#include "moby_ring.h"

// Zeroing must not be optimised away: a plain memset of a buffer that is not read again may be.
static void wipe(volatile int16_t *samples, size_t count)
{
    while (count-- != 0) {
        *samples++ = 0;
    }
}

static void wipe_span(moby_ring_t *ring, size_t first, size_t count)
{
    volatile int16_t *data = ring->data;
    for (size_t i = 0; i < count; ++i) {
        data[(first + i) % ring->capacity] = 0;
    }
}

void moby_ring_init(moby_ring_t *ring, int16_t *storage, size_t capacity)
{
    *ring = (moby_ring_t){ .data = storage, .capacity = capacity };
    wipe(storage, capacity);
}

void moby_ring_open(moby_ring_t *ring, size_t warmup_samples)
{
    moby_ring_clear(ring);
    ring->skip = warmup_samples;
    ring->overrun_pending = false;
    ring->open = true;
}

void moby_ring_close(moby_ring_t *ring)
{
    ring->open = false;
}

void moby_ring_clear(moby_ring_t *ring)
{
    ring->open = false;
    ring->skip = 0;
    wipe(ring->data, ring->capacity);
    ring->head = 0;
    ring->count = 0;
}

size_t moby_ring_write(moby_ring_t *ring, const int16_t *samples, size_t count)
{
    if (!ring->open) {
        ring->rejected_samples += (uint32_t)count;
        return 0;
    }
    if (ring->skip != 0) {
        const size_t dropped = count < ring->skip ? count : ring->skip;
        ring->skip -= dropped;
        samples += dropped;
        count -= dropped;
    }
    if (count == 0) {
        return 0;
    }
    if (count > ring->capacity - ring->count) {
        ++ring->overruns;
        ring->dropped_samples += (uint32_t)count;
        ring->overrun_pending = true;
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        ring->data[(ring->head + ring->count + i) % ring->capacity] = samples[i];
    }
    ring->count += count;
    return count;
}

size_t moby_ring_available(const moby_ring_t *ring)
{
    return ring->count;
}

size_t moby_ring_peek(const moby_ring_t *ring, int16_t *out, size_t count)
{
    if (count == 0 || count > ring->count) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = ring->data[(ring->head + i) % ring->capacity];
    }
    return count;
}

void moby_ring_consume(moby_ring_t *ring, size_t count)
{
    if (count > ring->count) {
        count = ring->count;
    }
    if (count == 0) {
        return;
    }
    wipe_span(ring, ring->head, count);
    ring->head = (ring->head + count) % ring->capacity;
    ring->count -= count;
}

bool moby_ring_take_overrun(moby_ring_t *ring)
{
    const bool pending = ring->overrun_pending;
    ring->overrun_pending = false;
    return pending;
}
