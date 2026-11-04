// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_TRANSPORT_H
#define MOBY_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The byte pipe under the protocol. USB-Serial-JTAG implements it first
// (moby_transport_usb_jtag.c); TinyUSB CDC can implement it the same way, so
// the two can be compared on hardware without touching the link. Nothing here
// may block: the link polls it from one task.
typedef struct {
    void *ctx;
    // True while a host is attached. Going false ends the session.
    bool (*connected)(void *ctx);
    // Copies up to capacity waiting bytes and returns how many, 0 for none.
    size_t (*read)(void *ctx, uint8_t *buffer, size_t capacity);
    // Queues the whole frame or nothing. Returns false when it did not fit.
    bool (*write)(void *ctx, const uint8_t *data, size_t length);
    // True when nothing queued is still waiting for the host to collect it.
    bool (*tx_idle)(void *ctx);
} moby_transport_t;

#endif
