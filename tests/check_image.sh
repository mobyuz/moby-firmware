#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Checks a built default image: no microphone, I2S or PCM code, and the console
# on UART0. Run inside an activated ESP-IDF shell: sh tests/check_image.sh [build dir]
set -eu
build=${1:-build}
elf=$build/moby_firmware.elf
fail=0
symbols=$(xtensa-esp32s3-elf-nm "$elf")
if echo "$symbols" | grep -Eiq 'moby_audio|moby_pcm|i2s_(new|channel)|i2s_std'; then
    echo "check_image: the default image contains audio or I2S code" >&2
    fail=1
fi
echo "$symbols" | grep -q ' T moby_link_step' || { echo "check_image: the link is missing" >&2; fail=1; }
grep -q 'CONFIG_MOBY_F2_MIC_TEST 1' "$build/config/sdkconfig.h" && { echo "check_image: the microphone bench is on" >&2; fail=1; }
grep -q 'CONFIG_ESP_CONSOLE_UART_DEFAULT 1' "$build/config/sdkconfig.h" || { echo "check_image: console is not UART" >&2; fail=1; }
grep -Eq 'CONFIG_ESP_CONSOLE_(USB_SERIAL_JTAG|SECONDARY_USB_SERIAL_JTAG) 1' "$build/config/sdkconfig.h" \
    && { echo "check_image: a USB console is on" >&2; fail=1; }
[ "$fail" -eq 0 ] && echo "default image: no audio code, console on UART0"
exit "$fail"
