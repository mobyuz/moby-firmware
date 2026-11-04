// SPDX-License-Identifier: GPL-3.0-only

#ifndef MOBY_TRANSPORT_USB_JTAG_H
#define MOBY_TRANSPORT_USB_JTAG_H

#include "esp_err.h"
#include "moby_transport.h"

// The protocol channel over the ESP32-S3 USB-Serial-JTAG controller (the
// native USB-C port). The console stays on UART0; this port carries frames only.
// Installs the driver once. Never use this port for logs or the console.
esp_err_t moby_transport_usb_jtag_init(moby_transport_t *transport);

#endif
