// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_RING_H
#define MOBY_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A bounded ring of 16-bit samples between the capture task and the link. The storage belongs to the caller (PSRAM
// on the device, an array in host tests). The ring is plain C and not thread safe: the platform wraps every call
// in one short lock.
//
// It is also the last gate in front of the link. While the gate is closed a write stores nothing, so a capture
// task that races a mute or a stop cannot leave a sample behind. The gate opens only through moby_ring_open.
// Consumed and cleared samples are overwritten with zeros, so a finished stream leaves no audio in memory.
// The ring never blocks: a write that does not fit is dropped whole and counted.

typedef struct {
    int16_t *data;
    size_t capacity;
    size_t head;
    size_t count;
    // Samples still to discard after the gate opened (microphone warm-up).
    size_t skip;
    bool open;
    bool overrun_pending;
    // Counters since init, never cleared by open/close/clear: a block that did not fit is `dropped_samples`.
    uint32_t overruns;
    uint32_t dropped_samples;
    // Samples offered while the gate was closed (discarded, harmless, counted for diagnostics only).
    uint32_t rejected_samples;
} moby_ring_t;

// The storage must hold `capacity` samples. A write is stored whole or not at all.
void moby_ring_init(moby_ring_t *ring, int16_t *storage, size_t capacity);

// Empties the ring (zeroing it), clears the pending overrun, and opens the gate. The first `warmup_samples` samples
// written afterwards are discarded.
void moby_ring_open(moby_ring_t *ring, size_t warmup_samples);

// Closes the gate. What is buffered stays until it is consumed or cleared.
void moby_ring_close(moby_ring_t *ring);

// Closes the gate, zeroes every buffered sample, and empties the ring.
void moby_ring_clear(moby_ring_t *ring);

// Stores the samples if the gate is open and they all fit. Returns how many were stored: `count`, or 0 when the
// gate is closed (rejected), the samples fell in the warm-up (discarded) or they did not fit (an overrun).
size_t moby_ring_write(moby_ring_t *ring, const int16_t *samples, size_t count);

size_t moby_ring_available(const moby_ring_t *ring);

// Copies exactly `count` samples to out without removing them, or returns 0 when fewer are buffered.
size_t moby_ring_peek(const moby_ring_t *ring, int16_t *out, size_t count);

// Removes and zeroes the first `count` samples (or all, when fewer are buffered).
void moby_ring_consume(moby_ring_t *ring, size_t count);

// True once after the first dropped write since the gate opened.
bool moby_ring_take_overrun(moby_ring_t *ring);

#endif
