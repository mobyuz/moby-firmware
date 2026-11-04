// SPDX-License-Identifier: GPL-3.0-only

#include "moby_transport_usb_jtag.h"

#include "driver/usb_serial_jtag.h"
#include "sdkconfig.h"

// The protocol owns this port, so it must never be the console or its fallback.
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#error "The USB-Serial-JTAG port carries the Moby protocol; keep the console on UART0"
#endif

enum {
    // The TX ring must hold one maximum frame (1046 bytes) with room to spare. The RX ring is the
    // driver's; the decoder behind it holds the segment being assembled.
    TX_BUFFER_BYTES = 2048,
    RX_BUFFER_BYTES = 1024,
};

static bool usb_connected(void *ctx)
{
    (void)ctx;
    return usb_serial_jtag_is_connected();
}

static size_t usb_read(void *ctx, uint8_t *buffer, size_t capacity)
{
    (void)ctx;
    const int count = usb_serial_jtag_read_bytes(buffer, (uint32_t)capacity, 0);
    return count > 0 ? (size_t)count : 0;
}

// The driver's ring buffer takes the whole item or none of it, and a zero wait never blocks.
static bool usb_write(void *ctx, const uint8_t *data, size_t length)
{
    (void)ctx;
    return usb_serial_jtag_write_bytes(data, length, 0) == (int)length;
}

static bool usb_tx_idle(void *ctx)
{
    (void)ctx;
    return usb_serial_jtag_wait_tx_done(0) == ESP_OK;
}

esp_err_t moby_transport_usb_jtag_init(moby_transport_t *transport)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = TX_BUFFER_BYTES,
        .rx_buffer_size = RX_BUFFER_BYTES,
    };
    if (!usb_serial_jtag_is_driver_installed()) {
        const esp_err_t error = usb_serial_jtag_driver_install(&config);
        if (error != ESP_OK) {
            return error;
        }
    }
    *transport = (moby_transport_t){
        .ctx = NULL,
        .connected = usb_connected,
        .read = usb_read,
        .write = usb_write,
        .tx_idle = usb_tx_idle,
    };
    return ESP_OK;
}
